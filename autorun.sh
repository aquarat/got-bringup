#!/bin/bash
# Run one Ghost of Tsushima measurement end to end, with no human in the loop.
#
#   ./autorun.sh <name> [VAR=VALUE ...]
#   ./autorun.sh baseline
#   ./autorun.sh notess HK_PERFTEST=notess
#
# Produces runs/<name>/ containing the raw reports, a summary, the steady-state
# window, and screenshots of whatever was on screen at each stage.
#
# WHY THIS EXISTS
# Every measurement so far has cost a human several minutes of clicking through
# a launcher, an intro video and a menu, and the human is the slow part. The
# numbers that matter (GPU busy %, compute share, fps) only mean anything at
# steady-state gameplay, which is 3-4 minutes after launch. This drives that
# unattended so experiments can be queued.
#
# HOW IT NAVIGATES
# Wine renders into an X11 window on Xwayland (:0), so xdotool can send input
# and ImageMagick's import can capture it -- neither needs the compositor's
# cooperation the way a Wayland client would. The click/key sequence lives in
# nav.txt, kept separate because it is the fragile, game-version-specific part.
set -uo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
NAME="${1:?usage: autorun.sh <name> [VAR=VALUE ...]}"; shift
OUT="$DIR/runs/$NAME"
LOG="$HOME/steam-2215430.log"
export DISPLAY=:0

# How long to allow for launcher + intro + menu + world load before giving up.
LAUNCH_TIMEOUT=${LAUNCH_TIMEOUT:-420}
# Reaching gameplay can take far longer than reaching the window: the game
# recompiles shaders after loading a save, which is slow on a driver with no
# precompiled cache for this GPU. Keep this generous and separate from the
# window-appearance timeout.
GAMEPLAY_TIMEOUT=${GAMEPLAY_TIMEOUT:-900}
# How long to hold at steady state while sampling.
SAMPLE_SECONDS=${SAMPLE_SECONDS:-90}
# Hard wall-clock ceiling for the WHOLE run. The per-stage timeouts can compound
# -- 600 s waiting for a window plus 1200 s waiting for gameplay is half an hour
# of clicking at a game that died at startup, which is exactly what happened
# when AGX_OCCUPANCY=512 crashed vkCreateComputePipelines. Nothing here should
# ever take longer than this; if it does, the run is broken, not slow.
RUN_TIMEOUT=${RUN_TIMEOUT:-1500}
RUN_START=$(date +%s)
# Telling gameplay from the MENU is the subtle part. The menu is itself a 3D
# scene -- it is GPU-bound and compute-dominated too -- so a command COUNT does
# not separate them (the menu issues MORE compute commands than gameplay: ~3960
# vs ~1659 per 5 s window). What separates them cleanly is how expensive each
# compute command is, and the frame rate:
#
#   menu      928 us per compute command    ~15-18 fps
#   gameplay 2565 us per compute command    ~6 fps
#
# So gate on per-command compute cost, with fps as corroboration.
# The ORIGINAL gate was cost per compute control stream (menu 928 us, gameplay
# 2565 us). That works at a fixed resolution and breaks the moment the
# resolution changes, because the cost of a control stream scales with pixels:
# at 1280x720 the menu sat at 885 us and sailed past a threshold lowered to 700,
# so the harness sampled the menu and called it gameplay.
#
# Frame rate and compute-milliseconds-per-frame are far better behaved. The menu
# renders 3x faster than gameplay at every resolution measured, and gameplay is
# always far more compute per frame.
GAMEPLAY_MIN_COMP_US=${GAMEPLAY_MIN_COMP_US:-1500}
# WIDENED 2026-09-04: the gate was fps<=15 and >=50 ms of compute per frame,
# which the driver outgrew -- with barrier mask 0x80 gameplay runs at ~14.8 fps
# and 40 ms, and a run measured at 14.72 fps was reported as "never reached
# gameplay" for eight minutes while the harness clicked at a running game. The
# menu is the thing being excluded and it sits at ~52 fps / 8.7 ms, so there is
# ample room; revisit again if gameplay passes ~25 fps.
# SUPERSEDED: both of these move as the driver gets faster, and both had to be
# widened twice -- fps<=15/50ms, then fps<=25/20ms -- each time silently
# mislabelling a good run as "never reached gameplay". Kept only so an old
# invocation still parses.
GAMEPLAY_MAX_FPS=${GAMEPLAY_MAX_FPS:-25}
GAMEPLAY_MIN_MS_PER_FRAME=${GAMEPLAY_MIN_MS_PER_FRAME:-20}

# The gate that actually works: COMPUTE CONTROL STREAMS PER FRAME. This counts
# how much work the frame is structurally made of, and it does not care how
# fast the GPU chews through it -- so unlike frame rate or milliseconds, it
# does not need revisiting every time something gets optimised.
#
# Measured distribution over a full run (runs/largeconst2): menu windows sit at
# 8-23, gameplay at 43-56. Nothing lands in between, so 35 separates them with
# a wide margin on both sides.
GAMEPLAY_MIN_CMDS_PER_FRAME=${GAMEPLAY_MIN_CMDS_PER_FRAME:-35}

mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT"/*.txt

say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/harness.log"; }

elapsed() { echo $(( $(date +%s) - RUN_START )); }

# Give up if the whole run has overrun, or if the game window has vanished after
# having appeared -- a crashed game leaves the harness clicking at a desktop.
should_abort() {
    local e; e=$(elapsed)
    if [ "$e" -gt "$RUN_TIMEOUT" ]; then
        say "ABORT: run exceeded RUN_TIMEOUT=${RUN_TIMEOUT}s"
        return 0
    fi
    if [ "${SAW_WINDOW:-0}" = 1 ] && [ -z "$(game_window)" ]; then
        say "ABORT: the game window has disappeared (crashed?) after ${e}s"
        return 0
    fi
    return 1
}

# Pointer coordinates are SCALED. Xwayland reports the display as 3024x1964
# while KDE's logical geometry is 2016x1310, and xdotool mousemove coordinates
# get multiplied by that 1.5x on the way in: asking for (752,601) puts the
# pointer at (1128,901). A click meant for the middle of the game window
# therefore landed outside it, on the desktop -- after which KDE's focus
# stealing prevention refused to give the window focus back, and every
# subsequent keystroke went nowhere. That is the whole reason input "randomly"
# stopped working mid-session.
#
# Measure the factor rather than hard-coding it.
PSCALE=1.0
calibrate_pointer() {
    xdotool mousemove 600 600 2>/dev/null; sleep 1
    local lx
    lx=$(xdotool getmouselocation --shell 2>/dev/null | sed -n 's/^X=//p')
    if [ -n "$lx" ] && [ "$lx" -gt 0 ] 2>/dev/null; then
        PSCALE=$(awk -v a="$lx" 'BEGIN{printf "%.4f", a/600}')
    fi
    say "pointer scale measured: $PSCALE"
}

# Move to a true screen coordinate, compensating for the scale, and click.
click_screen() {
    local rx ry
    rx=$(awk -v v="$1" -v s="$PSCALE" 'BEGIN{printf "%d", v/s}')
    ry=$(awk -v v="$2" -v s="$PSCALE" 'BEGIN{printf "%d", v/s}')
    xdotool mousemove "$rx" "$ry" 2>/dev/null; sleep 1
    # NOT `xdotool click 1`. A click is a press and a release in the same
    # instant, and the game does not register it -- proven directly: with the
    # pointer parked on CONTINUE, `click 1` did nothing nine times in a row,
    # while mousedown / 1 s / mouseup advanced the menu first try. Motion was
    # being delivered the whole time (hovering visibly moved the highlight), so
    # this is the game sampling input on its own ~6 fps cadence and missing a
    # button that goes down and up between two samples.
    xdotool mousedown 1 2>/dev/null; sleep 1; xdotool mouseup 1 2>/dev/null
}

# The window the game actually draws into. Prefer the title-bearing game window
# over the Wine virtual desktop that parents it: the desktop appears first, is
# blank, and capturing it yields no game pixels.
game_window() {
    local w
    w=$(xdotool search --name "Ghost of Tsushima" 2>/dev/null | head -1)
    [ -z "$w" ] && w=$(xdotool search --name 'Wine Desktop' 2>/dev/null | head -1)
    echo "$w"
}

# Steam's own window is 1924x1203 at (17,374) and the game's is 1920x1200 at
# (552,368): Steam covers essentially the whole game window, and KDE stacks it
# on top and makes it the active window after launch. Every click aimed at the
# game then lands on Steam instead. This is the real reason input "randomly"
# stopped working -- coordinates and focus were both fine, the pointer events
# were simply going to a different application.
#
# Minimising Steam is the reliable fix; raising the game window alone is not,
# because KDE can restack it again.
# NOT `xdotool search --name Steam`: it does not return Steam's main window at
# all. Observed directly -- the search listed five Steam-named windows while the
# one actually stacked on top of the game, and named exactly "Steam", was not
# among them. Enumerating _NET_CLIENT_LIST_STACKING and asking each window its
# name does find it, so use the window manager's own list.
banish_steam() {
    local w n
    for w in $(xprop -root _NET_CLIENT_LIST_STACKING 2>/dev/null |
               tr ',' '\n' | grep -oE '0x[0-9a-f]+'); do
        n=$(xdotool getwindowname "$w" 2>/dev/null)
        case "$n" in
            Steam|"Steam Big Picture Mode"|steamwebhelper)
                case "$(xprop -id "$w" _NET_WM_STATE 2>/dev/null)" in
                    *_NET_WM_STATE_HIDDEN*) ;;
                    *) xdotool windowminimize "$w" 2>/dev/null
                       say "minimised covering window $w ($n)" ;;
                esac ;;
        esac
    done
}

# Give the game window real focus by clicking inside it. windowactivate alone
# is not enough: KDE can refuse the activation request.
focus_game() {
    local g x y w h
    banish_steam
    g=$(game_window)
    [ -z "$g" ] && return 1
    xdotool windowraise "$g" 2>/dev/null
    eval "$(xdotool getwindowgeometry --shell "$g" 2>/dev/null)"
    [ -z "${X:-}" ] && return 1
    # THE critical step, and it must come FIRST -- clicking before focusing
    # puts focus back on the parent and undoes it. The game is a CHILD window of "Wine Desktop"; X focus
    # sits on the parent and the child never gets Wine's internal focus, so the
    # game ignores every key and click. windowfocus on the child fixes it, and
    # the effect is visible: the menu items brighten from their dimmed
    # "unfocused" look. That dimming was misread earlier as a loading state.
    local child
    child=$(xdotool search --name "Ghost of Tsushima" 2>/dev/null | head -1)
    [ -n "$child" ] && { xdotool windowfocus "$child" 2>/dev/null
                         xdotool windowactivate "$child" 2>/dev/null; }
    sleep 2
}

# Same reasoning as click_screen: hold the key long enough to survive a frame
# at 6 fps rather than trusting a single instantaneous press/release pair.
press() {
    focus_game
    xdotool keydown "$1" 2>/dev/null; sleep 1; xdotool keyup "$1" 2>/dev/null
    say "pressed $1"
}

# Click at coordinates relative to the game window's top-left.
wclick() {
    local g
    g=$(game_window)
    [ -z "$g" ] && return 1
    eval "$(xdotool getwindowgeometry --shell "$g" 2>/dev/null)"
    [ -z "${X:-}" ] && return 1
    click_screen $((X + $1)) $((Y + $2))
    say "clicked window-relative $1,$2"
}

# Clicking CONTINUE works where Return does not. Return activates the launcher
# reliably but is ignored by the game's own menu, which appears to want a real
# pointer event on the item.
# CONTINUE's position as a FRACTION of the window, not in pixels: the whole
# point of the resolution experiments is to change the window size, and a
# hard-coded (200,233) would land on NEW GAME at 1280x800.
CONTINUE_FX=${CONTINUE_FX:-0.104}
CONTINUE_FY=${CONTINUE_FY:-0.194}
click_continue() {
    focus_game
    local g
    g=$(game_window); [ -z "$g" ] && return 1
    eval "$(xdotool getwindowgeometry --shell "$g" 2>/dev/null)"
    [ -z "${WIDTH:-}" ] && return 1
    wclick "$(awk -v w="$WIDTH" -v f="$CONTINUE_FX" 'BEGIN{printf "%d", w*f}')" \
           "$(awk -v h="$HEIGHT" -v f="$CONTINUE_FY" 'BEGIN{printf "%d", h*f}')"
}
shot() {
    # "import -window root" fails on Xwayland ("missing an image filename").
    # Capturing the Wine Desktop window by id works and also excludes the
    # desktop behind it.
    local w
    w=$(game_window)
    # timeout, because import CAN hang forever: caught it blocked for 43
    # minutes on a stale window id while the run sat there doing nothing and
    # RUN_TIMEOUT never fired, because RUN_TIMEOUT is only checked between
    # phases and this was inside one. A missed screenshot is worth far less
    # than a lost run.
    [ -n "$w" ] && timeout 20 import -window "$w" "$OUT/$1.png" 2>/dev/null \
        && say "screenshot: $1.png" || say "screenshot $1 skipped (no window or import timed out)"
}

# KEEP_GAME=1 leaves the game running when the harness exits. Killing the
# harness alone does NOT do this: its TERM trap calls kill_muvm, and muvm hosts
# Steam as well as the game, so the whole session goes down with it.
kill_muvm() {
    # Only once this run has launched something -- the cleanup kill at the
    # START of a run must still happen, or a stale session poisons the result.
    [ "${KEEP_GAME:-0}" = 1 ] && [ "${LAUNCHED:-0}" = 1 ] && {
        say "KEEP_GAME=1, leaving the game running"; return 0; }
    local pids
    pids=$(ps -eo pid,args --no-headers | awk '$2=="/usr/bin/muvm"{print $1}')
    # NEVER use pkill -f here: the pattern matches this script's own shell.
    [ -n "$pids" ] && { kill $pids 2>/dev/null; sleep 6; }
    pids=$(ps -eo pid,args --no-headers | awk '$2=="/usr/bin/muvm"{print $1}')
    [ -n "$pids" ] && { kill -9 $pids 2>/dev/null; sleep 3; }
    return 0
}

trap 'say "interrupted, cleaning up"; kill_muvm; exit 130' INT TERM

say "=== run '$NAME' ==="
say "env overrides: $*"
kill_muvm
rm -f "$LOG"

# muvm hosts Steam AND the game, so this one process is the whole session.
say "launching steam + game"
env "$@" HK_GPUTIME=5 ASAHI_LOCAL_MESA=0 \
    "$HOME/.local/bin/steam" steam://rungameid/2215430 \
    > "$OUT/steam-stdout.txt" 2>&1 &
LAUNCHED=1

# --- wait for a window to exist, then run the navigation script -------------
# Wait for the GAME window, not for "Wine Desktop". Proton creates the virtual
# desktop as soon as it starts and the game's own window can appear MINUTES
# later -- five minutes later in one run, during which nav.txt ran to completion
# against an empty blue rectangle and every keystroke went nowhere. Worse, the
# desktop's drawable does not contain the game's window, so screenshots of it
# came back a flat uniform colour and looked like a rendering failure.
say "waiting for the game window (up to ${LAUNCH_TIMEOUT}s)"
WIN=""
for _ in $(seq 1 $((LAUNCH_TIMEOUT / 5))); do
    sleep 5
    WIN=$(game_window)
    [ -n "$WIN" ] && break
    if [ "$(elapsed)" -gt "$RUN_TIMEOUT" ]; then
        say "ABORT: no game window within RUN_TIMEOUT=${RUN_TIMEOUT}s"
        break
    fi
done

if [ -z "$WIN" ]; then
    say "NO GAME WINDOW appeared. Capturing the screen and giving up."
    shot fail-no-window
    kill_muvm
    exit 1
fi
say "window id $WIN"
# Arms the crashed-game abort in should_abort(). Without this the harness
# clicks at an empty desktop until RUN_TIMEOUT.
SAW_WINDOW=1
calibrate_pointer
xdotool windowactivate "$WIN" 2>/dev/null
shot 01-window-appeared

if [ -f "$DIR/nav.txt" ]; then
    say "running navigation script"
    n=0
    while read -r verb arg1 arg2; do
        case "$verb" in
        ''|'#'*) continue ;;
        sleep)   sleep "$arg1" ;;
        key)     press "$arg1" ;;
        click)   wclick "$arg1" "$arg2" ;;
        shot)    shot "nav-$arg1" ;;
        esac
        n=$((n+1))
    done < "$DIR/nav.txt"
    say "navigation done ($n steps)"
else
    say "no nav.txt -- relying on the game reaching gameplay by itself"
fi

# --- wait for steady-state gameplay ----------------------------------------
say "waiting for gameplay (>= ${GAMEPLAY_MIN_CMDS_PER_FRAME} compute streams/frame)"
reached=0
retry=0
for i in $(seq 1 $((GAMEPLAY_TIMEOUT / 10))); do
    sleep 10
    should_abort && break
    if [ -f "$LOG" ] && grep -h 'hk gputime' "$LOG" 2>/dev/null \
        | awk -v mincmds="$GAMEPLAY_MIN_CMDS_PER_FRAME" '
             /s wall/  { fr = $6 }
             /  comp / { ok = (fr > 0 && ($9 / fr) >= mincmds) }
             END       { exit !ok }'; then
        reached=1; break
    fi
    # Still light. The menu Return is unreliable -- one that failed in the
    # harness worked by hand seconds later -- so re-send it periodically
    # rather than lose a 10-minute run to a single dropped keystroke.
    # CONTINUE is the default item, so a spurious Return is harmless.
    if [ $((i % 6)) = 0 ] && [ "$retry" -lt 9 ]; then
        retry=$((retry+1))
        say "still in a light window; retry $retry"
        case $((retry % 3)) in
        1) click_continue ;;
        2) press Return ;;
        0) press space ;;
        esac
    fi
done

if [ "$reached" = 1 ]; then
    say "gameplay detected; sampling for ${SAMPLE_SECONDS}s"
    shot 02-gameplay
    sleep "$SAMPLE_SECONDS"
    shot 03-after-sample
else
    say "NEVER REACHED GAMEPLAY -- sampling anyway so the run is not wasted"
    shot 02-not-gameplay
fi

# --- collect ----------------------------------------------------------------
cp "$LOG" "$OUT/proton.log" 2>/dev/null
grep 'hk gputime' "$LOG" > "$OUT/reports.txt" 2>/dev/null

# Steady-state only: windows whose compute command count says it is gameplay.
awk -v mincmds="$GAMEPLAY_MIN_CMDS_PER_FRAME" '
  /s wall/  { w=$3; f=$6; fps=$8 }
  /  vtx /  { v=$4; nv=$9 }
  /  frag / { fr=$4 }
  /  comp / { c=$4; nc=$9; cus=$11 }
  /GPU busy/ {
     for (i=1;i<=NF;i++) if ($i=="(union)") b=$(i+1)
     gameplay = (f > 0 && (nc / f) >= mincmds)
     if (gameplay) { W+=w; F+=f; V+=v; FR+=fr; C+=c; B+=b; N++
                     k = int(nc/f + 0.5); sn[k]++; sf[k]+=f; st[k]+=w; sms[k]+=c }
     else if (c > 0) { mW+=w; mF+=f; mC+=c; mB+=b; mN++ }
  }
  END {
    # Per-scene breakdown. The camera does not land in the same place every
    # time, and this game varies a lot in cost with what is on screen --
    # baseline2 holds both a 5.8 fps village and a 17 fps lighter area, and
    # averaging the two produces
    # a number that compares to nothing.
    #
    # Compute control streams per frame identifies the scene: it is a property
    # of what is being drawn, stable to a stream or two across runs and across
    # driver versions, and it does not move when the GPU gets faster. So bucket
    # by it and let the reader compare like with like.
    if (N) {
      print "by scene (compute streams/frame identifies the scene):"
      printf "  %-10s %8s %9s %12s\n", "streams/f", "frames", "fps", "compute ms/f"
      for (k in sn) {
        printf "  %-10d %8d %9.2f %12.1f\n", k, sf[k], sf[k]/st[k], sms[k]/sf[k]
      }
      print ""
    }
    if (!N) { print "no steady-state windows found"; exit }
    printf "steady-state windows: %d   wall %.1f s   %d frames   %.2f fps\n", N, W, F, F/W
    printf "  vertex   %9.1f ms  %5.1f%% of wall\n", V, 100*V/(W*1000)
    printf "  fragment %9.1f ms  %5.1f%% of wall\n", FR, 100*FR/(W*1000)
    printf "  compute  %9.1f ms  %5.1f%% of wall\n", C, 100*C/(W*1000)
    printf "  GPU BUSY %9.1f ms  %5.1f%% of wall\n", B, 100*B/(W*1000)
    printf "  compute is %.1f%% of GPU busy time\n", 100*C/B
    if (mN) printf "\n(lighter windows, e.g. menu: %d, %.1f s, %.2f fps, GPU busy %.1f%%, compute %.1f%% of busy)\n", mN, mW, mF/mW, 100*mB/(mW*1000), 100*mC/mB
  }' "$OUT/reports.txt" | tee "$OUT/summary.txt"

kill_muvm
say "done -> $OUT"
