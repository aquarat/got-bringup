# Cross-subqueue overlap: it works, and it buys nothing here

AGX has two independent subqueues, VDM (render) and CDM (compute).
`drm_asahi_cmd_header` carries a separate barrier index for each, and
`DRM_ASAHI_BARRIER_NONE` means "do not wait on this subqueue at all".
Honeykrisp made every command wait on all prior work on BOTH, so the two
engines were strictly serialised.

That was measured, not assumed: vertex 8.5 + fragment 5.1 + compute 19.1 ms per
frame summed to 32.7 ms against a GPU-busy union of 32.6 ms. Overlap 0.00 ms.

## What was built

Wait on a command's own subqueue always; take the cross-subqueue wait only
where a dependency exists:

* `pre_gfx` compute feeds the render after it -> that render waits on compute;
* `post_gfx` compute consumes the render before it -> that compute waits;
* `vkCmdPipelineBarrier2`, in the direction its stage masks express -- a
  graphics-to-graphics barrier sets neither side;
* events and the query timestamp flush;
* the first command of each type in a submit, since barriers are
  submit-relative and nothing else can express "wait for the other engine's
  work from previous ioctls".

`merge_control_streams()` ORs the flag when merging, and
`hk_cmd_buffer_end_graphics()` re-arms it if compute fed the render, because
`hk_optimize_empty_vdm()` can drop the stream that transitivity would otherwise
have relied on.

## Result: correct, and worth nothing on this workload

Same session, A/B against `HK_PERFTEST=noxoverlap`:

Two independent A/B pairs, each control run in the same session as its
experiment:

| run | fps | stage sum | union | overlap |
|---|---|---|---|---|
| `noxoverlap` | 22.40 | 30.86 | 30.86 | 0.00 ms |
| `xoverlap` (both directions) | 22.56 | 32.33 | 30.60 | 1.73 ms (5%) |
| `noxoverlap2` | 22.15 | 30.99 | 30.99 | 0.00 ms |
| `xoverlap2` (direction-aware) | 22.21 | 32.87 | 31.11 | 1.76 ms (5%) |

Reproducible in both directions: exactly zero overlap without it, 1.7-1.8 ms
with it, and a frame-rate difference of +0.7% and +0.3% -- noise.

One detail confirms the overlap is genuine rather than a measurement artefact:
fragment time rises systematically whenever it is on (5.01 ms in both controls,
6.41 and 6.56 in the two experiments) while the union does not. That is what
sharing the machine looks like -- the stages stretch because they are now
running at the same time as something else.

Overlap went from exactly zero to real and measurable, which was structurally
impossible before. The frame rate did not move: 22.21 / 22.40 / 22.56 is noise.

Making the barrier tracking direction-aware -- so a graphics-to-graphics
barrier no longer blocks compute -- changed the overlap by 0.03 ms. It was the
right fix for the wrong bottleneck.

## Why the 13 ms estimate was wrong

The estimate assumed that "nothing overlaps" meant "nothing may overlap". The
counters say otherwise:

    compute commands taking a cross-subqueue wait:  5937/6854 = 87%
    render  commands taking a cross-subqueue wait:  6850/9251 = 74%

Those waits are mostly genuine. The driver runs compute FOR almost every
render pass:

    draw_robust_index_1   143.4 dispatches/frame
    tess_tri_0 / tess_tri_1 / prefix_sum_tess_1   ~34 each

Index-robustness clamping and tessellation emulation both produce data the
following draw consumes. So nearly every render command legitimately waits on
compute emitted immediately before it, and only 13% of compute and 26% of
render commands are free to overlap at all.

The serialisation was real, but it was mostly a true dependency wearing the
costume of a conservative barrier.

## Where that leaves it

Kept, `HK_PERFTEST=xoverlap`, **default off**. It is measured correct across
~38,000 CTS tests -- including `synchronization.op.single_queue`, which is
built from 440 graphics->compute and 570 compute->graphics dependency pairs --
and three game runs render correctly. But it is a synchronisation change whose
failure mode is a race that renders correctly most of the time, and it buys no
measurable frame rate here. That is not a trade worth making in a default.

Flip `HK_PERF_XOVERLAP` in `hk_device.c` to enable it; a workload with less
driver-generated pre-draw compute (no tessellation, no index robustness) would
see more from it than this one does.

## What would actually unlock it

Remove the dependency rather than the barrier:

* `draw_robust_index` at 143 dispatches/frame exists because robustness bounds
  are lowered by clamping the index buffer in a pre-pass. `hk_shader.c:342`
  has a TODO about doing bounds in the shader instead (`iadd(amul)`), which
  would delete the pre-pass and the dependency with it.
* Tessellation emulation is inherent to AGX having no hardware tessellator.
