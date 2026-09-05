# What vkCmdPipelineBarrier2 costs here

`hk_CmdPipelineBarrier2` is "the big hammer": it ends BOTH the compute and the
graphics control stream, with an `XXX: perf` comment. Ending a stream is not a
cache operation -- it is a full GPU drain and a firmware round trip.

The game issues **465 pipeline barriers per frame** while running only ~56
compute streams and ~81 render passes, so most barriers end a stream that is
already ended or empty, which is cheap. Counting all 465 would badly overstate
the prize.

## Measured classification (`runs/dumphot`, gameplay windows)

| | per frame | share |
|---|---|---|
| pipeline barriers total | 465 | 100% |
| compute-only (an in-stream CDM barrier could serve them) | 29.6 | 6.4% |
| compute-only but a graphics stream was open | 0 | 0% |
| require the hammer | 435 | 93.6% |

A barrier is counted compute-only when every src and dst stage mask names only
compute, transfer, draw-indirect or the ends of the pipe, and it carries no
image barrier. Image barriers are declined because they can carry a layout
transition; hk treats layouts as no-ops today, but that is a property of the
image code rather than something the barrier path can see.

93.6% needing the hammer is most likely D3D12 resource barriers on textures
arriving as image barriers, plus vkd3d-proton's broader stage masks. That is a
guess and is labelled as one.

## Is 29.6 per frame worth having?

Against ~56 compute streams per frame, removing 29.6 stream ends would be a
better-than-half reduction, so it is worth measuring rather than dismissing --
but only the ones that end a stream *containing work* actually save a drain.
The report now counts those separately (`ending real work`), because the
difference decides whether this is a real optimisation or a rounding error.

Implemented behind `HK_PERFTEST=csbarrier`: for a compute-only barrier with no
graphics stream open, emit a full in-stream CDM barrier and keep the stream.
That is exactly what the driver emitted after every dispatch before the overlap
work, so it is sufficient for compute->compute ordering by construction.
