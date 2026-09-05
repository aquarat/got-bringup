#!/bin/bash
# Instrumented Ghost of Tsushima run.
#
# Answers the question left open by CHECKLIST.md item 2b: at 6 fps, with the
# HOST showing no memory/io/cpu pressure and no swap traffic, what is actually
# limiting the frame rate?
#
# Separates three candidates:
#   1. guest-internal reclaim -> guest PSI rises while host PSI stays flat
#   2. GPU-bound              -> system power stays high while fps is low
#   3. stall / serialisation  -> everything idle, power low, fps still low
#
# PHASES MATTER. The game goes: launcher (no GPU) -> intro video (no GPU) ->
# menu (some GPU) -> gameplay (full GPU). A low power reading during the
# launcher means nothing. Mark the phases so the report compares like with like:
#
#   ./instrument.sh                      # terminal 1: start it, then launch Steam
#   ./instrument.sh mark gameplay        # terminal 2: when you reach actual gameplay
#   ./instrument.sh mark collapsed       # terminal 2: when the frame rate tanks
#
# Marks are optional but make the report far more trustworthy. The key output is
# the WITHIN-RUN comparison: the same machine, same scene, at high fps vs low
# fps. That is much stronger evidence than any absolute wattage.
#
#   ./instrument.sh report DIR           # re-print an earlier run

set -uo pipefail

BASE="$(cd "$(dirname "$0")" && pwd)"
GAMELOG="$HOME/.local/share/Steam/steamapps/compatdata/2215430/pfx/drive_c/users/steamuser/Documents/Ghost of Tsushima DIRECTOR'S CUT/Ghost of Tsushima DIRECTOR'S CUT.log"
INTERVAL=10
GUEST_CSV="$HOME/.got-guest-samples.csv"
GUEST_THREADS="$HOME/.got-guest-threads.txt"
GUEST_FLAG="$HOME/.got-sampling"
CURRENT="$BASE/runs/.current"

# ------------------------------------------------------------------- marks --
if [ "${1:-}" = "mark" ]; then
    d=$(cat "$CURRENT" 2>/dev/null) || { echo "no run in progress" >&2; exit 1; }
    echo "$(date +%s),$(date +%H:%M:%S),${2:-mark}" >> "$d/marks.csv"
    echo "marked '${2:-mark}' at $(date +%H:%M:%S)"
    exit 0
fi

# ---------------------------------------------------------------- reporting --
report() {
    local dir="$1" csv="$1/samples.csv"
    echo
    echo "=================== INSTRUMENTED RUN REPORT ==================="
    echo "dir: $dir"

    if [ -s "$dir/marks.csv" ]; then
        echo
        echo "--- phase marks ---"
        column -t -s, "$dir/marks.csv"
    else
        echo
        echo "--- phase marks: NONE RECORDED ---"
        echo "  Without marks the comparison below may straddle launcher/video/menu,"
        echo "  where low power is EXPECTED and means nothing. Treat with caution."
    fi

    echo
    echo "--- game frame rate / working set (game's own log, once per minute) ---"
    if [ -f "$dir/game.log" ]; then
        grep -ah 'Working set' "$dir/game.log" \
          | sed 's/.*Working set: /WS /; s/ Page file.*Video Budget: / budget /; s/ Video Usage.*Demoted: / demoted /; s/ fps: / fps /'
    fi

    echo
    echo "--- samples (psi_* are 'some avg10'; 0 = nothing stalling) ---"
    [ -f "$csv" ] && column -t -s, "$csv"

    # ---- the analysis that matters: same run, high fps vs low fps ----
    if [ -f "$csv" ]; then
        echo
        echo "--- WITHIN-RUN COMPARISON (the key result) ---"
        awk -F, 'NR>1 && $2!="" && $2+0>0 {
                    fps=$2+0; pw=$13+0; rss=$3+0; ha=$4+0; ga=$5+0;
                    gpm=$7+0; gpi=$9+0; gpc=$10+0;
                    if (best=="" || fps>bf) { bf=fps; bp=pw; br=rss; bha=ha; bga=ga; bgm=gpm; bgi=gpi; bgc=gpc; best=1 }
                    if (worst=="" || fps<wf) { wf=fps; wp=pw; wr=rss; wha=ha; wga=ga; wgm=gpm; wgi=gpi; wgc=gpc; worst=1 }
                 }
                 END{
                   if(best==""){print "  no fps samples captured"; exit}
                   printf "  %-22s %12s %12s\n","","HIGHEST fps","LOWEST fps";
                   printf "  %-22s %12.1f %12.1f\n","fps",bf,wf;
                   printf "  %-22s %12.1f %12.1f   <-- THE DECIDING NUMBER\n","system power (W)",bp,wp;
                   printf "  %-22s %12d %12d\n","game RSS (MB)",br,wr;
                   printf "  %-22s %12d %12d\n","host avail (MB)",bha,wha;
                   printf "  %-22s %12d %12d\n","guest avail (MB)",bga,wga;
                   printf "  %-22s %12.2f %12.2f\n","guest psi mem",bgm,wgm;
                   printf "  %-22s %12.2f %12.2f\n","guest psi io",bgi,wgi;
                   printf "  %-22s %12.2f %12.2f\n","guest psi cpu",bgc,wgc;
                 }' "$csv"
    fi

    if [ -s "$dir/threads.txt" ]; then
        echo
        echo "--- busiest guest threads (last sample) ---"
        tail -14 "$dir/threads.txt"
    fi

    cat <<'EOF'

--- HOW TO READ THIS ---
  Compare HIGHEST-fps against LOWEST-fps, and only if both are GAMEPLAY
  (check the marks; the launcher and intro video use almost no GPU, so a low
  reading there is meaningless).

  power HIGH at low fps            -> GPU-bound. Reduce GPU work.
  power DROPS with fps             -> the SoC went idle. 6 fps with an idle
                                      machine is a STALL, not a capacity limit.
  guest psi mem HIGH, host psi ~0  -> guest-internal reclaim. muvm balloons
                                      pages back to the host, so guest pressure
                                      is invisible from outside. Raise ASAHI_MEM_MB.
  one thread pegged in threads.txt -> single-thread bound after all.

CAVEAT: "Total System Power" is whole-SoC, not GPU. It cannot separate CPU from
GPU. It answers "is anything working at all", which is exactly what item 2b left
open. Run ./calibrate-power.sh once for the idle/CPU/GPU scale.
EOF
    echo "==============================================================="
}

if [ "${1:-}" = "report" ]; then
    report "${2:?usage: instrument.sh report DIR}"; exit 0
fi

# ------------------------------------------------------------ wait for VM --
echo "waiting for Steam's muvm VM (start Steam now if you have not)..."
for _ in $(seq 1 600); do pgrep -f '/usr/bin/muvm' >/dev/null 2>&1 && break; sleep 2; done
pgrep -f '/usr/bin/muvm' >/dev/null 2>&1 || { echo "no VM after 20 min; aborting" >&2; exit 1; }
echo "VM is up"

RUN_START=$(date +%s)
RUNDIR="$BASE/runs/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$RUNDIR"; echo "$RUNDIR" > "$CURRENT"
echo "logging to $RUNDIR"
echo "mark phases from another terminal:  $0 mark gameplay"

: > "$GUEST_CSV"; : > "$GUEST_THREADS"; touch "$GUEST_FLAG"

# ------------------------------------------------- guest-side sampling loop --
# One long-lived process inside the SAME VM (muvm attaches to the running VM,
# it does not create a second one), writing to the shared home directory.
muvm -- /bin/sh -c '
while [ -f '"$GUEST_FLAG"' ]; do
    T=$(date +%s)
    MA=$(awk "/MemAvailable/{print int(\$2/1024)}" /proc/meminfo)
    MF=$(awk "/^MemFree/{print int(\$2/1024)}" /proc/meminfo)
    PM=$(awk "/^some/{print \$2}" /proc/pressure/memory 2>/dev/null | cut -d= -f2)
    PI=$(awk "/^some/{print \$2}" /proc/pressure/io 2>/dev/null | cut -d= -f2)
    PC=$(awk "/^some/{print \$2}" /proc/pressure/cpu 2>/dev/null | cut -d= -f2)
    RSS=0
    # NB: pgrep -f self-matches this shell (the pattern is in our own cmdline),
    # so match on /proc/*/comm instead.
    P=""
    for d in /proc/[0-9]*; do
        c=$(cat "$d/comm" 2>/dev/null)
        case "$c" in GhostOfTsushima*) P=${d#/proc/}; break;; esac
    done
    if [ -n "$P" ]; then
        RSS=$(awk "/VmRSS/{print int(\$2/1024)}" /proc/$P/status 2>/dev/null)
        # per-thread CPU: settles the single-thread-bottleneck question
        { echo "== $(date +%H:%M:%S) threads of pid $P (top by cpu) =="
          top -H -b -n1 -p "$P" 2>/dev/null | tail -n +7 | sort -k9 -nr | head -12
        } > '"$GUEST_THREADS"'.tmp 2>/dev/null
        mv '"$GUEST_THREADS"'.tmp '"$GUEST_THREADS"' 2>/dev/null
    fi
    echo "$T,${MA:-0},${MF:-0},${PM:-0},${PI:-0},${PC:-0},${RSS:-0}" >> '"$GUEST_CSV"'
    sleep '"$INTERVAL"'
done' >/dev/null 2>&1 &
GUESTJOB=$!

cleanup() {
    rm -f "$GUEST_FLAG"; sleep 1; kill "$GUESTJOB" 2>/dev/null
    cp "$GAMELOG"      "$RUNDIR/game.log"      2>/dev/null
    cp "$GUEST_CSV"    "$RUNDIR/guest-raw.csv" 2>/dev/null
    cp "$GUEST_THREADS" "$RUNDIR/threads.txt"  2>/dev/null
    rm -f "$CURRENT"
    report "$RUNDIR"
    echo; echo "re-print later with:  $0 report $RUNDIR"
}
trap cleanup EXIT INT TERM

# -------------------------------------------------- host-side sampling loop --
CSV="$RUNDIR/samples.csv"
echo "time,fps,game_rss_mb,host_avail_mb,guest_avail_mb,host_psi_mem,guest_psi_mem,host_psi_io,guest_psi_io,guest_psi_cpu,swapin_mbs,swapout_mbs,power_w,vm_cpu" > "$CSV"
prev_in=$(awk '/pswpin/{print $2}' /proc/vmstat); prev_out=$(awk '/pswpout/{print $2}' /proc/vmstat)

echo "sampling every ${INTERVAL}s. Play. Ctrl-C when done (or just quit the game)."
while pgrep -f '/usr/bin/muvm' >/dev/null 2>&1; do
    sleep "$INTERVAL"
    NOW=$(date +%H:%M:%S)
    host_avail=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
    hpm=$(awk '/^some/{print $2}' /proc/pressure/memory | cut -d= -f2)
    hpi=$(awk '/^some/{print $2}' /proc/pressure/io | cut -d= -f2)
    cur_in=$(awk '/pswpin/{print $2}' /proc/vmstat); cur_out=$(awk '/pswpout/{print $2}' /proc/vmstat)
    sin=$(awk -v a="$prev_in" -v b="$cur_in" -v i="$INTERVAL" 'BEGIN{printf "%.1f",(b-a)*4/1024/i}')
    sout=$(awk -v a="$prev_out" -v b="$cur_out" -v i="$INTERVAL" 'BEGIN{printf "%.1f",(b-a)*4/1024/i}')
    prev_in=$cur_in; prev_out=$cur_out
    pw=$(awk -v v="$(cat /sys/class/hwmon/hwmon1/power1_input 2>/dev/null || echo 0)" 'BEGIN{printf "%.1f", v/1000000}')
    vmcpu=$(ps -eo pcpu,comm --no-headers 2>/dev/null | awk '$2=="VM:fedrat"{print $1; exit}')

    gline=$(tail -1 "$GUEST_CSV" 2>/dev/null)
    g_avail=$(echo "$gline" | cut -d, -f2); g_pm=$(echo "$gline" | cut -d, -f4)
    g_pi=$(echo "$gline" | cut -d, -f5);    g_pc=$(echo "$gline" | cut -d, -f6)
    g_rss=$(echo "$gline" | cut -d, -f7)
    # Only trust the game log once the game has rewritten it for THIS run,
    # otherwise we report the previous session's final fps as if it were live.
    fps=""
    if [ "$(stat -c %Y "$GAMELOG" 2>/dev/null || echo 0)" -ge "$RUN_START" ]; then
        fps=$(grep -ah 'Working set' "$GAMELOG" 2>/dev/null | tail -1 | sed 's/.*fps: //')
    fi

    echo "$NOW,${fps:-},${g_rss:-},${host_avail:-},${g_avail:-},${hpm:-},${g_pm:-},${hpi:-},${g_pi:-},${g_pc:-},$sin,$sout,$pw,${vmcpu:-}" >> "$CSV"
    printf '\r%s fps=%-5s rss=%-6s hostav=%-6s guestav=%-6s gpsi=%-5s pow=%-5sW  ' \
        "$NOW" "${fps:-?}" "${g_rss:-?}" "${host_avail:-?}" "${g_avail:-?}" "${g_pm:-?}" "$pw"
done
echo; echo "VM exited."
