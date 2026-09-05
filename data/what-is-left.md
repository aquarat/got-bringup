# What is left, and what the frame is bound by

Measured on the current driver (`runs/largeconst2`, 20,081 frames), heavy scene
only -- windows bucketed by compute streams per frame so this compares against
the earlier runs like for like.

## The frame

```
frame           47.7 ms   (20.98 fps)
  GPU busy      32.6 ms   68%
    compute     19.1 ms
    vertex       8.5 ms
    fragment     5.1 ms
  GPU idle      15.1 ms   32%
    one stall/frame   ~9.3 ms
    ~220 small gaps   ~5.1 ms
```

## Is it GPU, CPU or memory bound?

**GPU bound, at the frame level.** Three drivers, same scene, same harness:

| run | frame | GPU busy | idle | vtx | frag | comp |
|---|---|---|---|---|---|---|
| `baseline2` | 171.70 | 149.89 | 21.82 | 8.1 | 6.8 | 135.0 |
| `clean80` | 63.44 | 49.17 | 14.27 | 8.0 | 5.0 | 36.2 |
| `largeconst2` | 47.68 | 32.63 | 15.05 | 8.5 | 5.1 | 19.1 |

Frame time tracks GPU busy time almost exactly -- 117.3 ms of GPU time removed
bought 124.0 ms of frame time, a slope of 1.06 -- while idle stayed put at
~15 ms. So GPU work is still the thing setting the frame rate, and the idle is
a fixed additive cost rather than something that scales.

**Memory bound, within the GPU.** Not ALU bound and no longer occupancy bound:

* 1.65 ns per invocation measured against 0.189 ns if ALU-bound: **11.5% of
  peak ALU**. (It was 1.0-3.5% before this work, so the gap has closed a lot,
  but it is still 8.7x.)
* The current top shader, `8c4c1c4aeb67`, runs at **full occupancy (1024
  threads), 27 GPRs, zero spills, zero scratch** and is still ~42x off peak
  ALU. There is no thread supply problem left to fix on it; what remains is
  the memory system.
* Measured streaming ceiling on this GPU is ~68 GB/s in `tests/memtest.c`, and
  dependent-load latency 428-905 ns.

**Not CPU bound yet, but there is a floor.** The ~9.3 ms stall is unchanged
across drivers whose GPU work differs by 4.5x, so it is independent of GPU
work. If every remaining millisecond of GPU time vanished, the frame would
still be ~10 ms, i.e. about 100 fps. That is the ceiling this stall imposes,
and it is a long way above where we are.

## Cross-subqueue overlap — SUPERSEDED, it was measured and buys nothing

**Everything in this section was written before the change was implemented and
measured. It was, and it is worth nothing on this workload** -- see
`subqueue-overlap.md` and `per-fix-results.md`. The analysis below is kept
because the mechanism description is still accurate; the prize estimate is not.

### (original text follows)

## The two subqueues never overlap

vertex 8.5 + fragment 5.1 + compute 19.1 = 32.7 ms, and the measured union is
32.6 ms. **Nothing overlaps with anything.** On a tile-based deferred renderer
with independent VDM (render) and CDM (compute) subqueues that is pure loss.

The mechanism is in `hk_queue.c`. Every control stream is submitted with

    agx_cmd_header(cs->type == HK_CS_CDM, nr_vdm, nr_cdm)

so each command waits for **all** previous work on **both** subqueues. The UAPI
does not require this: `drm_asahi_cmd_header` has separate `vdm_barrier` and
`cdm_barrier` fields, and `DRM_ASAHI_BARRIER_NONE` (0xFFFF) means "do not wait
on this subqueue at all". The fixup loop in `hk_queue.c` already handles NONE
correctly -- nothing ever passes it.

A compute command only needs `vdm_barrier` if it consumes render output. Most
do not.

There is a second, matching conservatism above it: `hk_cmd_draw.c:776` ends the
compute stream at every draw, under

    /* Don't reorder compute across render passes.
     * TODO: Check if this is necessary if the proper PipelineBarriers are
     * handled... there may be CTS bugs... */

so the driver author already flagged it as possibly unnecessary.

**Prize:** if compute overlapped graphics completely, busy would fall from
32.6 ms to about max(19.1, 13.6) = 19.1 ms, i.e. ~13 ms/frame, taking the frame
to ~35 ms / 29 fps. Realistically less, since not every compute stream is
independent of the render that precedes it. This is the largest single item
left and the only one that is structural rather than incremental.

**Risk:** higher than anything done so far. The CDM barrier work was safe
because Vulkan already guarantees dispatches sharing a control stream are
independent. There is no equivalent free guarantee across subqueues; it needs
real dependency tracking between render and compute streams, and getting it
wrong produces races that render correctly most of the time.

## Everything else, smaller

* **Compute, 19.1 ms.** Needs a fresh per-shader ranking -- the old one's
  number one is now 19x cheaper. Memory-bound, full occupancy, so the levers
  are access patterns and cache behaviour rather than threads or ALU.
* **Vertex, 8.5 ms.** Untouched by all of this work and now 26% of GPU time.
  Includes tessellation emulation (~150 helper dispatches/frame).
* **The ~5.1 ms of drain gaps.** ~220 per frame. `HK_PERFTEST=csbarrier` was
  measured and did nothing, because the barriers it skips are followed by a
  draw that ends the stream anyway. Cross-subqueue overlap above is the real
  fix for this too.
* **The ~9.3 ms stall.** Not addressable from the driver. Worth identifying
  (CPU under FEX vs compositor pacing) only to know the true ceiling.

---

# The current per-shader ranking

`runs/isolate4`, `HK_GPUTIME_ISOLATE=1`, 2949 frames. Isolate mode gives every
dispatch its own control stream so attribution is complete, but it perturbs:
compute reads 39.3 ms/frame against 19.1 ms in normal operation, so divide the
measured column by ~2.06 for real time. The RANKING is what this is for.

| id | spirv | disp/fr | invoc/fr | ms/fr | share | instrs | gprs | occ | loops | spill:fill |
|---|---|---|---|---|---|---|---|---|---|---|
| 158 | `a30540657d3d` | 109.2 | 91981 | **8.37** | 28.8% | 2196 | 107 | 896 | 0 | 0:0 |
| 302 | `c0408dcabfa6` | 23.4 | 12003 | 4.10 | 14.1% | 12705 | 207 | 512 | 17 | 5:4 |
| 303 | `dc6351ad59d8` | 15.2 | 975 | 2.87 | 9.9% | 10653 | 215 | 448 | 17 | 0:0 |
| 304 | `18a1bc3d2c5b` | 6.9 | 3524 | 1.66 | 5.7% | 13835 | 207 | 512 | 30 | 10:7 |
| 78 | `eccd8233893e` | 1.0 | 2220403 | 1.59 | 5.5% | 1264 | 143 | 704 | 0 | 0:0 |
| 103 | `bd65ca414801` | 0.3 | 50852 | 1.04 | 3.6% | 8327 | 255 | 384 | 12 | **108:71** |

Top four are 58% of compute; the previous champion `6fc0efe726d2` has fallen to
eighth at 3.3%.

Things worth noticing:

* **158 is the new number one and it is clean.** 107 GPRs, occupancy 896 of
  1024, no spills, no scratch, no loops. There is nothing structurally wrong
  with it -- 109 dispatches of ~843 invocations, and it is simply memory-bound.
  This is the shape that says "the easy driver-side wins are done".
* **302/303/304 are loop-heavy** (17-30 hardware loops, 10-14k instructions).
  Static cycle estimates mean nothing for these; 303 runs only 975 invocations
  per frame and still costs 2.87 ms, i.e. ~2.9 us per invocation. If anything
  here is worth reading next, it is 303.
* **103 still spills hard** -- 255 GPRs, 108:71 spills, occupancy pinned at
  384. So extending `agx_spill.c:can_remat()` is NOT entirely moot after all,
  as claimed after the constant-data work; it is just now worth 3.6% of
  compute rather than 38%.
* The `est%` column disagrees violently with `meas%` (78 is 21.5% estimated
  against 5.5% measured). The static model counts ALU cycles and this workload
  is not ALU-bound, so rank by the measured column only.

---

# Robustness lowering: the per-load address arithmetic

## What the hot shaders actually spend instructions on

`a30540657d3d` issues **410 scalar loads and 2 vec4 loads**; `dc6351ad59d8`
issues 1114 scalar, 48 vec4, 10 vec2. Almost every load is a single dword, and
each carries an address computation like:

```
%135 = imadshl_agx %104.w, 1, %113, 2   // offset = base + index*4
%136 = umin %135, 0xfffffffc            // clamp
%137 = iadd %136, 3
%138 = ult %137, %104.z                 // in bounds?
%139..%144 = bcsel x4                   // zero the ADDRESS if out of bounds
%145, %142 = pack_64_2x32_split x2
%146 = iadd                             // the address, at last
```

That is robust buffer access lowering, and vkd3d-proton enables
`robustBufferAccess`, so every dynamically indexed SSBO load gets it.

## Measured cost, and why the offset shape matters

`tests/ssboload.c` and `tests/ssboload2.c`: 64 dynamically indexed loads,
compiled with and without `HK_PERFTEST=norobust`.

| offset shape | robust | norobust | cost per load |
|---|---|---|---|
| bare `amul(idx, 4)` | 904 | 707 | 3.1 instrs |
| `iadd(amul(idx, 4), 16)` | 1161 | 772 | **6.1 instrs** |
| `iadd(amul(idx, 4), 16)` after the fix | **969** | 772 | **3.1 instrs** |

`check_in_bounds()` in `hk_shader.c` had a cheap element-wise path for a bare
`amul` and an explicit `TODO: handle also the iadd(amul) pattern, this is
important`. An array behind a header in the same buffer -- `buffer B { uint
hdr[4]; uint data[]; }` -- produces exactly that shape, and it is everywhere in
this game.

Handling it in elements costs one `ult` per load, because `bound` is uniform so
the divide and the saturating subtract hoist into the preamble.

## Correctness

`tests/robtest.c`: 256 in-bounds and 768 out-of-bounds reads through the header
pattern. In-bounds return their values, out-of-bounds return zero. The
pre-change driver passes it too, so the test is not vacuous.

## Frame-time impact: not measurable with this harness

The game A/B was inconclusive and it is worth recording WHY, because it bounds
what any future experiment here can claim.

`runs/norobust` landed in scenes at 37-47 compute streams per frame; its
control `runs/robctl` sat mostly at 44 and 55-57. Even restricting both to the
44-stream bucket, the two disagree wildly -- 22.56 fps against 29.07 -- so at
that granularity the bucket does not identify the same content.

**The harness cannot resolve frame-time effects of a few percent.** The scene
bucketing is good enough for the large steps measured earlier (129 -> 53 -> 18
ms of compute) and not for anything smaller.

What IS scene-independent is comparing a NAMED SHADER between runs: same SPIR-V
hash, same invocation count, different statistics. That is what exposed the
shader-cache bug, and it is the right instrument for a change like this one.
