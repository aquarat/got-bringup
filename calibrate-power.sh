#!/bin/bash
# Calibrate "Total System Power" against known loads.
#
# WHY THIS EXISTS: Asahi exposes no GPU utilisation counter anywhere --
# not in /sys/kernel/debug/dri/406400000.gpu (only clients/gem_names/name),
# not as drm-engine-* in /proc/<pid>/fdinfo, and MangoHud has no AGX backend.
# The one signal we DO have is macsmc_hwmon "Total System Power".
#
# On its own a wattage is meaningless. This script establishes the scale, so
# that a reading taken during a 6 fps moment can be classified:
#
#   ~idle          -> nothing is working. A stall, not a capacity limit.
#   ~cpu-busy      -> CPU-side bound
#   ~gpu-busy      -> genuinely GPU-bound
#
# Run it once, with the desktop otherwise quiet. Takes about 2 minutes.

set -uo pipefail
POWER=/sys/class/hwmon/hwmon1/power1_input

read_w() { awk -v v="$(cat $POWER 2>/dev/null || echo 0)" 'BEGIN{printf "%.1f", v/1000000}'; }

sample() {           # sample <seconds> -> mean, min, max
    local secs="$1" vals=() v
    for _ in $(seq 1 "$secs"); do vals+=("$(read_w)"); sleep 1; done
    printf '%s\n' "${vals[@]}" | awk '{s+=$1; if(min==""||$1<min)min=$1; if($1>max)max=$1; n++}
                                      END{printf "mean %5.1f W   min %5.1f W   max %5.1f W", s/n, min, max}'
}

echo "=============== POWER CALIBRATION ==============="
echo "Keep the desktop idle. Do not move the mouse more than necessary."
echo

echo "1/4  IDLE baseline (20s)..."
echo "     $(sample 20)"

echo
echo "2/4  CPU load, all cores (20s)..."
NPROC=$(nproc)
for _ in $(seq 1 "$NPROC"); do (while :; do :; done) & done
CPUJOBS=$(jobs -p)
sleep 2
echo "     $(sample 20)"
kill $CPUJOBS 2>/dev/null; wait 2>/dev/null
sleep 3

echo
echo "3/4  GPU load, vkcube offscreen (20s)..."
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1000}
vkcube --c 100000 >/dev/null 2>&1 &
VKPID=$!
sleep 3
if kill -0 $VKPID 2>/dev/null; then
    echo "     $(sample 20)"
    kill $VKPID 2>/dev/null
else
    echo "     vkcube would not start (needs a display); skipped"
    echo "     retry from a graphical terminal, or use: DISPLAY=:0 $0"
fi
sleep 3

echo
echo "4/4  IDLE again, to confirm the machine settled (10s)..."
echo "     $(sample 10)"

cat <<'EOF'

=============== HOW TO USE THIS ===============
Compare against power_w in instrument.sh's samples.csv at a low-fps moment:

  power near IDLE      -> the machine is doing nothing. 6 fps with an idle SoC
                          means a STALL or serialisation, not a capacity limit.
                          Chase blocking: guest reclaim, fence waits, a spinning
                          streaming thread.
  power near CPU-busy  -> CPU-side bound. Profile threads.
  power near GPU-busy  -> genuinely GPU-bound. Reduce GPU work.

CAVEAT: this is TOTAL SYSTEM power, not GPU power. It cannot separate CPU from
GPU on its own -- that is what the CPU-load and GPU-load steps above are for,
and even then a mixed workload sits between them. Treat it as a coarse
"is anything working at all" signal, which is exactly the question item 2b left
open. Do not over-read small differences.
EOF
