#!/bin/bash
# How many independent dispatches does this GPU actually run at once?
#
# With the weak barrier, consecutive dispatches may overlap -- but "may" is not
# "does", and the hardware clearly has some limit on how many launches it will
# keep in flight. Knowing it decides whether further work should go into
# raising concurrency or into making each dispatch bigger.
#
# Method: fix the work per dispatch at one workgroup and a long serial loop, so
# a single dispatch cannot fill the machine and its duration is basically
# constant. Then sweep the dispatch COUNT. While the hardware can absorb them
# in parallel, total time stays flat; once it saturates, time grows linearly.
# The knee is the answer.
#
# Usage: ./concurrency.sh [loops]
set -u
cd "$(dirname "$0")"
LOOPS=${1:-100000}
echo "dispatches  ms      ms/dispatch"
for d in 1 2 4 8 12 16 24 32 48 64 96 128 192 256; do
    ms=$(CSTEST_CASE=$d,1,$LOOPS timeout 120 ./cstest 2>/dev/null | tail -1 | awk '{print $4}')
    [ -z "$ms" ] && { echo "$d  FAILED"; continue; }
    printf "%10d  %-8s %.4f\n" "$d" "$ms" "$(echo "$ms $d" | awk '{print $1/$2}')"
done
