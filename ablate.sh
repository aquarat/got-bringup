#!/bin/bash
# Per-fix comparison at matched resolution AND gameplay only.
# The invocation filter alone pins resolution but does NOT exclude the menu --
# the menu is pinned too. Gameplay is identified by compute streams per frame,
# which is a property of what is being drawn.
cd /home/aquarat/Projects/got-bringup
printf "%-14s %5s %8s %10s %10s\n" run wins fps comp_ms/f busy_ms/f
for r in "$@"; do
  awk -v r="$r" '
    function commit(){ if(have&&m&&gp){W+=w;T+=w/f;C+=c;B+=b;N++} }
    /s wall/  {commit(); w=$6;f=$8;c=0;b=0;have=1;m=0;gp=0}
    /  comp / {c=$4; if(w>0 && ($9/w)>=35) gp=1}
    /GPU busy/{b=$6}
    $NF=="8c4c1c4aeb67" && $6==808832 {m=1}
    END{commit(); if(!N){printf "%-14s   none\n", r; exit}
        printf "%-14s %5d %8.2f %10.2f %10.2f\n", r, N, W/T, C/W, B/W}
  ' runs/$r/reports.txt
done
