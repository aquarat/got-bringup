#!/bin/bash
# Run several autorun.sh experiments back to back and tabulate them.
#
#   ./queue.sh "baseline" "notess:HK_PERFTEST=notess" "batch:HK_PERFTEST=batch"
#
# Each argument is <name>[:VAR=VALUE[,VAR=VALUE...]]. Runs are sequential --
# they contend for the whole GPU, so running them in parallel would measure
# nothing but the contention.
set -uo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"

for spec in "$@"; do
    name="${spec%%:*}"
    envs="${spec#*:}"; [ "$envs" = "$spec" ] && envs=""
    echo "=============================================================="
    echo "queue: $name  ${envs:+($envs)}"
    echo "=============================================================="
    IFS=',' read -ra vars <<< "$envs"
    "$DIR/autorun.sh" "$name" "${vars[@]}"
    sleep 20   # let the GPU and page cache settle between runs
done

echo
echo "================== COMPARISON =================="
printf '%-14s %7s %7s %9s %9s %9s\n' run fps busy% comp_ms comp/busy us/cmd
for spec in "$@"; do
    name="${spec%%:*}"
    f="$DIR/runs/$name/summary.txt"
    [ -f "$f" ] || { printf '%-14s  (no summary)\n' "$name"; continue; }
    awk -v n="$name" '
      /steady-state windows/ { for(i=1;i<=NF;i++) if($i=="fps") fps=$(i-1) }
      /compute  /  { c=$2 }
      /GPU BUSY/   { b=$3; bp=$4 }
      /compute is/ { cb=$3 }
      END { printf "%-14s %7s %7s %9s %9s\n", n, fps, bp, c, cb }' "$f"
done
