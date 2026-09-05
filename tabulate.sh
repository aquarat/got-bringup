#!/bin/bash
# Emit one CSV row per gputime report window: run,fps,vtx_ms,frag_ms,comp_ms,comp_cmds,comp_us,busy_ms,wall_s
for r in "$@"; do
  awk -v n="$r" '
    /s wall/   { w=$3; f=$6; have=1 }
    /  vtx /   { v=$4 }
    /  frag /  { fr=$4 }
    /  comp /  { c=$4; nc=$9; cus=$11 }
    /GPU busy/ { for(i=1;i<=NF;i++) if($i=="(union)") b=$(i+1)
                 if (have && w>0) printf "%s,%.2f,%.1f,%.1f,%.1f,%d,%.1f,%.1f,%.2f\n", n, f/w, v, fr, c, nc, cus, b, w
                 have=0 }
  ' "runs/$r/reports.txt" 2>/dev/null
done
