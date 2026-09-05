# Where the frame goes, and whether the frame rate is even GPU-bound

## The display is 120 Hz

`xrandr`: `eDP-1 ... 3024x1964 120.00*+`. So the vblank period is **8.333 ms**
and, with a FIFO (vsync) swapchain, achievable frame rates are quantised:

| vblanks | frame time | fps |
|---|---|---|
| 6 | 50.00 ms | 20.00 |
| 7 | 58.33 ms | 17.14 |
| 8 | 66.67 ms | 15.00 |

This matters more than it looks. 15.00 fps is also 60/4, so a frame rate near
15 does NOT distinguish a 60 Hz panel from a 120 Hz one, and neither number
distinguishes "the GPU cannot go faster" from "the GPU finished early and
waited for a vblank". Those want completely different work.

## Measured frame budget (`runs/clean80`, default driver, 3518 frames)

```
frame time        59.24 ms   =  7.11 vblanks
GPU busy (union)  45.6  ms
GPU idle          13.6  ms
  compute         35.0  ms/frame
  vertex           6.4  ms/frame
  fragment         4.2  ms/frame
```

## The idle is bimodal, which is the informative part

Gaps between merged GPU intervals, one 5 s window:

| gap size | count | total |
|---|---|---|
| < 10 us | 192 | 1.8 ms |
| < 100 us | 17331 | 400.8 ms |
| 100 us - 1 ms | 0 | 0.0 ms |
| >= 1 ms | **80** | **702.8 ms** |

Two thirds of all idle is in **~1 gap per frame of ~8.8 ms**, and nothing at
all falls in between. 8.8 ms is one vblank period. The remaining third is ~220
small gaps per frame totalling ~5.6 ms/frame, which is the pipeline being
drained -- consistent with ~137 control stream ends per frame.

## Hypothesis, and the test

If the big gap is the vsync wait, then the critical path is roughly

    45.6 ms busy + 5.6 ms of drain gaps = 51.2 ms

against a 6-vblank budget of 50.00 ms. That would put the game **1.2 ms away
from 20 fps**, and would mean small GPU savings convert into a large frame
rate step rather than a proportional one.

If instead the big gap is the CPU failing to feed the GPU -- entirely plausible
with the game's x86-64 code running under FEX emulation -- then GPU savings buy
nothing until the CPU side moves, and the work belongs somewhere else entirely.

Test: `MESA_VK_WSI_PRESENT_MODE=immediate` removes the swapchain cap.

## Result: it is NOT the vsync cap

| | fps | frame time | vblanks | gaps >=1 ms | big-gap mean | busy/frame |
|---|---|---|---|---|---|---|
| `clean80` | 16.88 | 59.24 ms | 7.11 | 80 | 8.79 ms | 45.6 ms |
| `immediate` | 16.49 | 60.64 ms | 7.28 | 81 | 8.51 ms | 45.8 ms |

Removing the cap moved nothing: not the frame rate (the difference is inside
the scene variance noted below), not the number of big gaps, not their size.

The override did reach the game -- the wrapper reports "forwarding 5 env var(s)
into muvm" against 4 for the control run, and Mesa logged no "Unsupported
MESA_VK_WSI_PRESENT_MODE value". So this is a real negative result, not a
plumbing failure.

Supporting evidence that it was never vsync: the largest single gap observed is
**13.6 ms**, and a wait for the next vblank cannot exceed one 8.333 ms period.

## So what IS the ~8.5 ms stall?

Not established. Two candidates remain and this test does not separate them:

* the game's own CPU work between frames, running as emulated x86-64 under
  FEX, with the GPU idle while it happens;
* the compositor pacing presentation regardless of the swapchain's present
  mode -- under Xwayland an "immediate" swapchain does not necessarily bypass
  the Wayland frame callback.

What IS established, and is what matters for planning: **there is a stall of
about 8.5 ms per frame, roughly 14% of the frame, that does not respond to GPU
work or to the swapchain present mode.** GPU-side optimisation cannot remove
it, and the ceiling for further driver work should be computed with it present.

Distinguishing the two needs a different experiment -- measuring CPU frame time
directly, or presenting offscreen -- which has not been run.

## Methodological caveat: scene variance

The same driver measured 14.72 fps (`runs/mask80`, 15188 frames) and 16.88 fps
(`runs/clean80`, 3518 frames). The harness loads the same save and runs the
same navigation, but where the camera ends up still varies, and this game's
cost varies a lot with what is on screen.

So run-to-run differences under ~15% are not evidence of anything. The changes
claimed so far are much larger than that (5.94 -> 12.4 -> ~15-17) and each was
also confirmed by the compute-milliseconds-per-frame figure, which is far less
scene-sensitive than fps. Small effects need an A/B in the same session, and
ideally more than one run each.
