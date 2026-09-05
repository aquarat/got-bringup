#!/bin/bash
# Aggregate the per-shader and per-helper-kernel tables out of a run's reports.
#
#   ./shaders.sh runs/<name>            steady-state (gameplay) windows only
#   ./shaders.sh runs/<name> menu       the lighter (menu) windows instead
#
# WHY NOT JUST READ THE LAST REPORT
# Each report covers 5 s. At 6 fps that is ~30 frames, so a shader that runs
# once every few frames is badly sampled in any single window. Aggregating
# across every gameplay window weights each window by the frames it actually
# delivered, which is also what stops the menu -- rendered at 3x the frame rate
# with a completely different shader mix -- from swamping the numbers.
set -uo pipefail
RUN="${1:?usage: shaders.sh runs/<name> [menu]}"
MODE="${2:-gameplay}"
# Same gate as autorun.sh: frame rate plus compute-per-frame, both of which
# behave sensibly across a resolution change, unlike cost per control stream.
# Same gate as autorun.sh: compute control streams per frame. Frame rate and
# milliseconds both move as the driver improves and had to be rewidened twice;
# the structural work per frame does not. Menu 8-23, gameplay 43-56.
MINCMDS=${GAMEPLAY_MIN_CMDS_PER_FRAME:-35}
F="$RUN/reports.txt"
[ -f "$F" ] || { echo "no $F"; exit 1; }

awk -v mincmds="$MINCMDS" -v mode="$MODE" '
  # A new window begins at the wall-clock line; everything until the next one
  # belongs to it.
  /s wall/ { frames = $6; fps = $8; keep = 0; next }
  /  comp / { gameplay = (frames > 0 && ($9 / frames) >= mincmds)
              keep = (mode == "menu") ? (frames > 0 && !gameplay) : gameplay
              if (keep) { WF += frames; WN++; CMS += $4 }
              next }

  # Header: "top compute shaders by estimated cost (N live, X Gcycle total)".
  # X is computed over EVERY live shader, not just the 20 printed below, so it
  # is the only trustworthy total here.
  keep && /Gcycle total/ {
     # This figure is the total for the WINDOW, not per frame. Accumulate window
     # totals and divide by total frames at the end. Multiplying by the frame
     # count of that same window first would just reproduce the window total,
     # mislabelled as a per-frame figure.
     for (i = 1; i <= NF; i++) if ($(i+1) ~ /^Gcycle/) { TGC += $i; break }
     next
  }

  # Shader row layout:
  #  $3 id  $4 kind  $5 disp/fr  $6 invoc/fr  $7 est%  $8 meas ms/fr  $9 meas%
  #  $10 cyc  $11 inst  $12 gprs  $13 thr  $14 loop  $15 spil  $16 wg  $17 stage
  keep && $3 ~ /^[0-9]+$/ && NF >= 17 {
     id = $3
     kind[id] = $4
     d[id]   += $5 * frames
     inv[id] += $6 * frames
     ms[id]  += $8 * frames          # measured ms/frame, weighted by frames
     cyc[id] = $10; inst[id] = $11; gprs[id] = $12
     thr[id] = $13; loop[id] = $14; spil[id] = $15; wg[id] = $16; stage[id] = $17
     seen[id] = 1
     next
  }

  # Helper kernel rows: name <n> disp/fr <n> invoc/fr <share>
  keep && NF == 8 && $5 == "disp/fr" {
     hd[$3] += $4 * frames
     hi[$3] += $6 * frames
     hseen[$3] = 1
     next
  }

  END {
    if (!WN) { print "no " mode " windows found"; exit 1 }
    printf "%s: %d windows, %d frames\n", mode, WN, WF

    total = 0; tinv = 0
    for (i in seen) { cost[i] = inv[i] * cyc[i]; total += cost[i]; tinv += inv[i] }

    # Reality check on the cost model. Measured compute time over invocations
    # is what an invocation really costs; the model estimate over the same is
    # what it would cost if ALU-bound. The ratio is how far from ALU-bound
    # this workload actually is.
    printf "  compute %.1f ms/frame   %.2f Gcycle/frame (true total, all shaders)\n", \
           CMS/WF, TGC/WF
    printf "  top-20 only: %.2f M invocations/frame   %.2f Gcycle/frame (%.0f%% of the true total)\n", \
           tinv/WF/1e6, total/WF/1e9, TGC ? 100.0*(total/1e9)/TGC : 0
    # 4096 lanes x 1.296 GHz = 5.308e12 lane-cycles/s, so ns/invocation if
    # perfectly ALU-bound is cycles-per-invocation / 5308.
    if (tinv) {
       meas_ns = CMS*1e6/tinv
       alu_ns  = (total/tinv)/5308.0
       printf "  %.2f ns per invocation measured   %.3f ns if ALU-bound (4096 lanes x 1.296 GHz)   -> %.1fx off, %.1f%% of peak ALU\n", \
              meas_ns, alu_ns, meas_ns/alu_ns, 100*alu_ns/meas_ns
    }
    printf "\n"

    printf "%-4s %-7s %8s %12s %11s %7s %6s %8s %6s %5s %5s %5s %5s %s\n", \
           "id","origin","disp/fr","invoc/fr","meas ms/fr","meas%","est%","cyc/inv","instrs","gprs","occ","loops","spill","stage"
    tms = 0
    for (i in seen) tms += ms[i]

    # Rank by MEASURED time. The estimate is anti-correlated with reality at the
    # top of this table -- the largest estimated item measured at 0.6% -- so
    # sorting by it would put the wrong shaders first.
    n = 0
    for (;;) {
      best = ""; bv = -1
      for (i in seen) if (!done[i] && ms[i] > bv) { bv = ms[i]; best = i }
      if (best == "" || ++n > 24) break
      done[best] = 1
      printf "%-4s %-7s %8.1f %12.0f %11.2f %6.1f%% %5.1f%% %8s %6s %5s %5s %5s %5s %s\n", \
        best, kind[best], d[best]/WF, inv[best]/WF, ms[best]/WF, \
        tms ? 100*ms[best]/tms : 0, \
        total ? 100*cost[best]/total : 0, cyc[best], inst[best], gprs[best], \
        thr[best], loop[best], spil[best], stage[best]
    }

    print ""
    ht = 0
    for (k in hseen) ht += hd[k]
    printf "driver helper kernels: %.1f dispatches/frame across %d kernels\n", ht/WF, length(hseen)
    printf "%-26s %9s %12s %7s\n", "kernel","disp/fr","invoc/fr","share"
    n = 0
    for (;;) {
      best = ""; bv = -1
      for (k in hseen) if (!hdone[k] && hd[k] > bv) { bv = hd[k]; best = k }
      if (best == "" || ++n > 15) break
      hdone[best] = 1
      printf "%-26s %9.1f %12.0f %6.1f%%\n", best, hd[best]/WF, hi[best]/WF, \
             ht ? 100*hd[best]/ht : 0
    }
  }' "$F"
