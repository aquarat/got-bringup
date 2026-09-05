# Fragment: instrumented, but not yet rankable

Fragment was the last stage with no per-shader accounting. Compute had it
(`hk_gputime_note_shader` at the CDM chokepoint); vertex and fragment had
nothing but a stage total.

## What was added

Each draw records its fragment shader on the control stream, marking it
`HK_GPUTIME_MIXED` when more than one runs, and the firmware's `ts_frag`
interval is charged to that shader when exactly one owned the whole stream.
This is the same soundness condition the compute attribution uses, and the same
`block_shader` machinery.

## Result: 29% coverage, which is not enough to rank

```
fragment stage:                7.45 ms/frame
attributed to a single FS:     2.15 ms/frame   = 29% coverage
```

A render pass usually runs many different fragment shaders, so most streams are
MIXED and unattributable. 29% is a biased sample: it over-represents whatever
happens to get a render pass to itself, which is exactly the bias that made an
earlier compute ranking in this project unsound and forced its retraction.

**So do not rank fragment shaders on this data.** What it does establish is
that the instrumentation works and what the coverage is.

For the record, the top attributed shaders were:

| id | spirv | ms/frame | instrs | gprs | occupancy |
|---|---|---|---|---|---|
| 212 | `6b104353a7f6` | 0.86 | 1100 | 103 | 1024 |
| 43 | `5bf779cb4f8d` | 0.29 | 7 | 16 | 1024 |
| 39 | `26c109103d2f` | 0.27 | 65 | 39 | 1024 |
| 85 | `f36dd29a643d` | 0.18 | 204 | 39 | 1024 |

Nothing there is structurally alarming -- full occupancy, no spills, no
scratch, modest register counts.

## To get a sound ranking

The compute equivalent of this problem was solved by `HK_GPUTIME_ISOLATE=1`,
which ends the control stream after every dispatch so each is timed alone. The
graphics equivalent would end the render pass after every draw. That is far
more perturbing than the compute version -- a render pass carries tile setup
and a flush, where a compute stream costs ~2 us to start -- so the absolute
numbers would be worthless and only the ranking would survive. Worth doing if
fragment ever becomes the thing worth optimising; at 7.45 ms of a 44 ms frame
it is currently third behind compute and the non-GPU stall.
