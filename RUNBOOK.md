# Instrumented run: how to do it

Answers CHECKLIST.md item 2b — at 6 fps with an apparently idle machine, what is
actually limiting the frame rate?

## Once, first

    ./calibrate-power.sh

Establishes the idle / CPU-busy / GPU-busy wattage scale, because Asahi exposes
no GPU utilisation counter anywhere and "Total System Power" is the only signal
we have. Measured so far: **idle ~17 W**. Note that `vkcube` is far too light to
represent a real GPU load, so the more trustworthy calibration is the
within-run comparison below.

## Every run

Terminal 1:

    cd ~/Projects/got-bringup && ./instrument.sh

It waits for the VM, so you can start it before or after Steam.

Terminal 2 (or the same one, later) — **mark the phases**:

    ./instrument.sh mark launcher
    ./instrument.sh mark intro_video
    ./instrument.sh mark menu
    ./instrument.sh mark gameplay
    ./instrument.sh mark collapsed     # the moment fps tanks

Marks matter. The game goes launcher (no GPU) -> intro video (no GPU) -> menu
(some GPU) -> gameplay (full GPU). **A low power reading during the launcher or
video means nothing.** Without marks the report cannot tell those apart from a
genuine gameplay stall, and it will say so.

Then play. Ctrl-C when done, or just quit the game — it stops on its own and
prints the report.

## What it collects

| Source | Signal | Answers |
|---|---|---|
| host `/proc/pressure/*` | memory/io/cpu stall | host-side pressure |
| **guest** `/proc/pressure/*` | same, inside the VM | **guest-internal reclaim, invisible from the host** |
| guest `/proc/meminfo` | guest available memory | is the VM itself short? |
| guest `VmRSS` | game working set | pool growth |
| `macsmc_hwmon` power1 | total SoC watts | **is anything working at all?** |
| guest `top -H` | per-thread CPU | single-thread bottleneck |
| `/proc/vmstat` | swap in/out MB/s | real swap traffic |
| game log | fps + working set | ground truth, once per minute |
| MangoHud log | per-frame times | stutter vs sustained low fps |
| `VKD3D_QUEUE_PROFILE` | queue submissions | submission-side stalls |

## The output that matters

The report ends with a **within-run comparison**: the same machine, same scene,
at its highest-fps sample versus its lowest. That is far stronger evidence than
any absolute wattage.

    power HIGH at low fps            -> GPU-bound
    power DROPS with fps             -> SoC went idle: a STALL, not a capacity limit
    guest psi mem HIGH, host psi ~0  -> guest-internal reclaim; raise ASAHI_MEM_MB
    one thread pegged in threads.txt -> single-thread bound

## Current tunables

    ASAHI_VRAM_MB=8192    # applied; PROVEN not to bound the working set
    ASAHI_MEM_MB=16384    # applied; never yet exercised
    ASAHI_LOCAL_MESA=0    # required - local Mesa cannot init (item 7)

Both muvm sizes are fixed at VM creation, so **quit Steam completely** before a
run that changes them.

## Known blind spots

- **No GPU utilisation counter exists.** Not in
  `/sys/kernel/debug/dri/406400000.gpu` (only clients/gem_names/name), not as
  `drm-engine-*` in `/proc/<pid>/fdinfo`, and MangoHud has no AGX backend, so its
  `gpu_load` will read zero. Power is a coarse proxy for the whole SoC and
  cannot separate CPU from GPU.
- The game only logs fps once per minute, so `fps` in samples.csv is the last
  value it published, not an instantaneous reading. MangoHud's per-frame log is
  the finer-grained source.
