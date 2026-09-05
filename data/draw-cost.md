# Is the vertex phase driver overhead or real work?

Vertex was the last block nobody had examined: 7.8 ms per frame, 25% of GPU
busy time, untouched by any of the optimisation work.

## The workload

This game is draw-call heavy: **6886 draws per frame** across 81.6 render
passes, at 22.4 fps -- about 154,000 draws per second. So 7.8 ms of vertex time
is **1133 ns per draw**, and the question is how much of that is the draw
itself rather than the shading.

## Measuring an empty draw

`tests/drawcost.c`: one triangle, no vertex inputs, no varyings, a fragment
shader writing a constant, into a 64x64 R32_UINT attachment. Sweep the draw
count in a single render pass; the intercept is the render pass, the slope is
the draw.

**The first version of this test was wrong** and worth recording. It drew a
full-screen triangle, so every "trivial" draw rasterised 4096 pixels: at high
draw counts it was measuring fragment throughput, not per-draw cost. The
symptom was a per-draw cost that *fell* from 1.0 us to 0.12 us as the count
rose, which is not how a fixed cost behaves. Making the triangle degenerate
(all three vertices at one point, so the rasteriser produces nothing) isolates
what was wanted:

| draws | total us | us/draw |
|---|---|---|
| 1 | 79.96 | 79.96 (all render pass setup) |
| 256 | 96.38 | 0.377 |
| 1024 | 208.08 | 0.203 |
| 4096 | 277.75 | 0.068 |
| 16384 | 854.38 | 0.052 |

Marginal cost over the last step: **47 ns per draw**.

## Answer: it is real work

    per-draw GPU overhead        47 ns
    x 6886 draws                 0.32 ms
    of 7.8 ms vertex             4.1% overhead, 95.9% genuine work

An empty draw costs 47 ns; this game's draws cost 1133 ns. The difference is
vertex shading and tiling of actual geometry.

So there is no meaningful driver-side win in the vertex phase. The lever is
fewer draws or less geometry, and both belong to the application. This closes
the last unexamined block.
