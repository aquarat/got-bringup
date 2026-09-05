#!/bin/bash
# Read honeykrisp's firmware GPU timing out of the Proton log.
#
# Run the game with:
#     HK_GPUTIME=5 ASAHI_LOCAL_MESA=0 steam
# then, while it runs (or after):
#     ./gputime.sh            # last few reports
#     ./gputime.sh -f         # follow live
#     ./gputime.sh -s         # summary of the whole run
#
# WHAT THE NUMBERS MEAN
#   vtx/frag/comp   GPU time in each phase, and how much of wall clock it is.
#                   These OVERLAP on a tiler, so they can sum past 100%.
#   GPU busy        the UNION of every interval. This is the real one.
#
#     union near 100%  -> GPU saturated. Only less work will help.
#     union well under -> the GPU is idle inside the frame. The bottleneck is
#                         submission latency or serialisation, not capacity,
#                         and shader/pipeline optimisation will not fix it.

set -uo pipefail
LOG=${LOG:-$HOME/steam-2215430.log}
[ -f "$LOG" ] || { echo "no log at $LOG (is PROTON_LOG=1 set?)" >&2; exit 1; }

case "${1:-}" in
-f) exec tail -f "$LOG" | grep --line-buffered 'hk gputime' ;;

-s)
    echo "=== $LOG ==="
    grep 'hk gputime' "$LOG" | awk '
      /s wall/          { wall += $3; frames += $6 }
      /  vtx /          { vtx  += $4; nv += $9 }
      /  frag /         { frag += $4; nf += $9 }
      /  comp /         { comp += $4; nc += $9 }
      /GPU busy/        { for (i=1;i<=NF;i++) if ($i=="(union)") busy += $(i+1) }
      END {
        if (wall == 0) { print "no reports found"; exit }
        printf "wall      %10.1f s   %d frames   %.1f fps\n", wall, frames, frames/wall
        printf "vertex    %10.1f ms  %5.1f%%  %8d cmds\n", vtx,  100*vtx/(wall*1000),  nv
        printf "fragment  %10.1f ms  %5.1f%%  %8d cmds\n", frag, 100*frag/(wall*1000), nf
        printf "compute   %10.1f ms  %5.1f%%  %8d cmds\n", comp, 100*comp/(wall*1000), nc
        printf "GPU BUSY  %10.1f ms  %5.1f%%   <-- the number that decides it\n", busy, 100*busy/(wall*1000)
      }'
    ;;

*)  grep 'hk gputime' "$LOG" | tail -"${1:-30}" ;;
esac
