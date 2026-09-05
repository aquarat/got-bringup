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

Pinning the resolution was necessary but NOT sufficient -- see the correction
below. Compute milliseconds per frame at a matched scene reproduces to 1-2%;
frame rate does not reproduce reliably at all.

Arms are filtered on two things: exactly 808,832 invocations of
`8c4c1c4aeb67` (which runs one invocation per pixel, so it identifies the
render resolution), AND at least 35 compute streams per frame (which excludes
the menu). Results are then reported per scene, because one number per fix
turned out to be an artifact of the window mix.

## CORRECTION (2026-09-05, after external review)

The first version of this document reported a single number per fix, filtered
only on render resolution. **That filter was wrong and those numbers should not
be used.** Pinning the resolution pinned it in the MENU too, so menu windows
(90+ fps, ~5 ms compute) passed the filter -- between 31% and 53% of the frames
behind each arm's figure. The old gameplay gate that excluded the menu was no
longer being applied.

Worse, "gameplay" is not one scene. Restricting to gameplay still averages over
at least two, and the fixes behave very differently in each. **The per-fix
numbers are therefore properties of the window mix, not of the fixes**, and the
honest presentation is per scene.

## The arms, corrected

Filtered on BOTH matched resolution (808,832 invocations of `8c4c1c4aeb67`) AND
gameplay (>= 35 compute streams per frame, which excludes the menu). Scenes are
separated by streams per frame.

### Heavy scene (54 streams/frame, n = 20-24 windows per arm)

| arm | fps | compute ms/frame |
|---|---|---|
| **all fixes** | **21.5** | **18.60** |
| − constant tables out of scratch | 15.3 | 37.77 |
| − barrier mask `0x80` (back to `0x1f`) | 16.2 | 34.13 |
| − dispatch overlap entirely | 7.7 | 102.81 |
| + subqueue overlap | 22.0 | 18.56 |

### Light scene (43 streams/frame, n = 14 windows per arm)

| arm | fps | compute ms/frame |
|---|---|---|
| **all fixes** | **28.6** | **12.56** |
| − constant tables out of scratch | 17.9 | 33.96 |
| − barrier mask `0x80` | **33.2** | 13.34 |
| − dispatch overlap entirely | 25.3 | 18.16 |
| + subqueue overlap | 31.5 | 12.31 |

## What each fix is worth — it depends on the scene

| fix | heavy scene | light scene |
|---|---|---|
| Dispatch overlap (whole CDM barrier change) | **5.53x** compute | 1.45x compute |
| — of which narrowing `0x1f` to `0x80` | 1.84x compute | **negative** (see below) |
| Constant tables out of scratch | 2.03x compute | 2.70x compute |
| Subqueue overlap | ~1.00x (noise) | ~1.02x (noise) |

Two things worth stating plainly:

* **Dispatch overlap is worth far more than first reported** on the heavy
  scene: 102.81 ms of compute per frame without it against 18.60 with, a 5.5x
  difference, not the 2.17x the contaminated average gave.
* **The mask narrowing does not reproduce in the light scene.** There `0x1f`
  measured *faster* than `0x80` (33.2 against 28.6 fps, 13.34 against 12.56 ms
  -- the two disagree in direction, which is itself a warning). n = 14 windows,
  one run per arm. The heavy-scene result (1.84x) is the stronger of the two but
  the honest position is that this fix is not cleanly measured.

The previously published figures -- +34.5%/2.17x for overlap, +12.6%/1.42x for
the mask, +30.1%/1.88x for constant data, and "4.1x together" -- are withdrawn.
"4.1x" was in any case a product of two separately measured ratios, never
measured directly; no arm exists with both fixes disabled.

## Reproducibility: also overstated

The claim of "0.6% on fps and 0.08% on compute" came from comparing two runs
under the contaminated filter, one of which was later superseded. Measured
properly, from `pintest` against `p-final` (same driver, same settings):

| | heavy scene | light scene |
|---|---|---|
| compute ms/frame | 18.71 vs 18.60 (0.6%) | 12.28 vs 12.56 (2.3%) |
| fps | 21.8 vs 21.5 (1.4%) | 37.7 vs 28.6 (**24%**) |

So: **compute-milliseconds-per-frame at a matched scene reproduces to roughly
1-2%. Frame rate does not reproduce reliably** -- the light scene varies 24%
between identical runs, presumably because it is less GPU-bound and therefore
more exposed to the ~9 ms non-GPU stall. Use compute ms/frame, not fps.

## A tautology that was presented as evidence

The original document argued that the decomposition was "internally consistent"
because 1.52 x 1.42 = 2.16 against a measured 2.17, and called that independent
corroboration. It is not: those ratios were computed from the same three
measurements, so the identity holds by construction and cannot fail. That
argument is withdrawn.

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
