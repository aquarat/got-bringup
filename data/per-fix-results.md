# What each fix is worth

Measured 2026-09-05, ablation against the shipping driver: every arm removes
exactly one change, so the delta from the reference IS that change's
contribution. Same driver build, same session, same scene, matched render
resolution.

## Why these numbers can be trusted when earlier ones could not

Three hazards had to be closed first (`measurement-hazards.md`):

1. **Dynamic resolution.** The game targets 30 fps and moves render resolution
   between 960x600 and 1920x1200 to hold it, so a driver improvement can be
   absorbed as extra pixels at constant frame rate. Pinned by setting
   `UpscaleQuality` to a fixed level and `DynamicResolutionTargetFPS` to 1 in
   the game's Wine registry.
2. **AGX_MESA_DEBUG absent from the shader cache key** -- one nopreamble run
   poisoned every later run.
3. **HK_PERFTEST absent from the shader cache key** -- the first attempt at
   this very ablation produced identical shader statistics with and without
   `noconstdata`, and reported the pass as worth 1.5% when it had not been
   disabled at all.

With resolution pinned the harness became precise enough to be worth using:
two independent runs of the same driver agree to **0.6% on fps and 0.08% on
compute time**, against roughly 15% before.

Every arm below is filtered to windows at exactly 808,832 invocations of
`8c4c1c4aeb67`, which runs one invocation per pixel and so identifies the
render resolution exactly. 42-47 windows per arm.

## The arms

| arm | fps | compute ms/frame | GPU busy ms/frame | ns/invocation |
|---|---|---|---|---|
| **all fixes (reference)** | **32.47** | **11.94** | **18.63** | **14.77** |
| − constant tables out of scratch | 24.96 | 22.47 | 28.81 | 27.78 |
| − barrier mask `0x80` (back to `0x1f`) | 28.83 | 17.01 | 23.72 | 21.03 |
| − dispatch overlap entirely | 24.14 | 25.93 | 30.64 | 32.06 |
| + subqueue overlap | 32.70 | 12.38 | 19.43 | 15.30 |

## What each fix is worth

| fix | fps | compute | commit |
|---|---|---|---|
| **Dispatch overlap** (whole CDM barrier change) | **+34.5%** | **2.17x** | `df1874767e7`, `ba5fcc29756` |
| — of which: overlap at mask `0x1f` | +19.4% | 1.52x | `df1874767e7` |
| — of which: narrowing `0x1f` to `0x80` | +12.6% | 1.42x | `ba5fcc29756` |
| **Constant tables out of scratch** | **+30.1%** | **1.88x** | `6a71e0feba7` |
| Subqueue overlap | +0.7% (noise) | 0.96x (worse) | `df2ad9e9ab6`, default OFF |

Together the two real fixes are worth about **4.1x on compute time**. They do
not add linearly in frame rate because compute is only part of the frame.

### The decomposition is internally consistent

    no overlap        25.93 ms
      -> overlap 0x1f 17.01 ms    1.52x
      -> mask 0x80    11.94 ms    1.42x
                                  -----
      total                       2.17x     (1.52 x 1.42 = 2.16)

Three separately measured runs agreeing arithmetically to 0.5% is not
something noise produces. It is the best evidence available that these arms
measure what they claim.

### Verification that each arm actually engaged

A timing difference means nothing if the configuration did not change, which
is exactly how the first attempt failed.

* `noconstdata`: shader `6fc0efe726d2` goes from 1974 instructions, 119 GPRs,
  832 occupancy, 0 spills, 0 scratch **to** 2540 instructions, 255 GPRs, 384
  occupancy, 194:78 spills, 3104 bytes of scratch. Confirmed different code.
* `mask1f`: wrapper reports 5 forwarded environment variables against 4.
* `xoverlap`: measured overlap 1.52 ms against 0.00 ms, and 14% of compute and
  28% of render commands submitted without a cross-subqueue wait against 0%.

## Subqueue overlap: a correction

This was reported three times and got it wrong twice.

1. First measurement, unpinned resolution: "works but buys nothing" -- correct
   conclusion, luck rather than method.
2. Then, prompted to look at whether dynamic resolution was hiding gains: two
   A/B pairs showed 13-27% more pixels at identical frame rate, and it was
   reported as a real gain the frame rate had masked. **Wrong.**
3. Now, at pinned resolution: +0.7% fps, 3.7% *worse* compute. The extra
   pixels were the resolution controller landing somewhere different between
   runs. Correlation, not causation -- a caveat stated at the time and then
   leaned away from.

**Default OFF is correct**, and the reasoning in `subqueue-overlap.md` stands:
87% of compute and 74% of render commands take a cross-subqueue wait that is
genuine, because `libagx_draw_robust_index` writes VDM words into the graphics
control stream for nearly every render pass.

## Fixes not in the table

* **`iadd(amul)` bounds checking** (`895002f0e6b`): halves robustness lowering
  cost for that offset shape, 6.1 to 3.1 instructions per load, measured
  locally. Changes nothing in this game -- 42 shaders byte-identical -- because
  its shaders use a different offset shape. Real improvement, wrong workload.
* **Fragment attribution** (`f0d9a97406f`): instrumentation, not an
  optimisation. 29% coverage, too low to rank on.
* **Cache key fixes** (`c72c9e345aa`, `551b3415ffe`): correctness of
  measurement, not performance. Without them the table above would be wrong.
