# Ghost of Tsushima on Honeykrisp — where this stands

**Result: 5.84 -> 22.4 fps on the heavy scene (3.8x).** Compute time per frame
134.4 -> 17.9 ms (7.5x).

Enabled by default in the local driver; just run `steam` through the wrapper.

## What each fix is worth

Ablation at **pinned render resolution**, same build and session, each arm
removing exactly one change. Full detail and the verification that each arm
actually engaged: **`data/per-fix-results.md`**.

| fix | fps | compute | commit |
|---|---|---|---|
| **Dispatch overlap** (CDM barrier) | **+34.5%** | **2.17x** | `df1874767e7`, `ba5fcc29756` |
| — overlap at mask `0x1f` | +19.4% | 1.52x | `df1874767e7` |
| — narrowing `0x1f` to `0x80` | +12.6% | 1.42x | `ba5fcc29756` |
| **Constant tables out of scratch** | **+30.1%** | **1.88x** | `6a71e0feba7` |
| Subqueue overlap | +0.7% (noise) | worse | `df2ad9e9ab6`, default OFF |

Reference (all fixes): **32.47 fps, 11.94 ms compute/frame, 14.77
ns/invocation**. Removing dispatch overlap: 24.14 fps, 25.93 ms. Removing the
constant-data pass: 24.96 fps, 22.47 ms.

Together the two real fixes are worth about **4.1x on compute time**.

## Measurement: read this before trusting any number here

The game's dynamic resolution moves render resolution over a 4x range to hold
30 fps, so **frame rate does not measure GPU efficiency** unless it is pinned.
Two shader-cache-key bugs also let configuration changes silently no-op. All
three are fixed or documented in `data/measurement-hazards.md`; with resolution
pinned the harness reproduces to 0.6% on fps, against roughly 15% before.

Numbers taken before that work -- anything comparing fps or ms/frame between
runs -- are indicative only. The per-shader comparisons at matched invocation
counts were always sound.

## The frame today

```
frame           44.6 ms   (22.4 fps)
  GPU busy      30.8 ms   69%
    compute     17.9 ms   memory-bound; top shader is clean
    vertex       7.8 ms   95.9% genuine work (6886 draws/frame)
    fragment     5.0 ms
  GPU idle      13.8 ms   31%
    one stall/frame  ~9.3 ms   not GPU, not the swapchain
    ~220 small gaps  ~5.1 ms   control stream drains
```

Every step was checked against a captured frame, not just a counter
(`./verify-frame.sh runs/<name>`).

Everything below is measured on an M1 Max under Fedora Asahi Remix 44, Ghost of
Tsushima DIRECTOR'S CUT (Steam 2215430) via muvm + FEX + Proton + vkd3d-proton,
on the Honeykrisp Vulkan driver reached through the FEX Vulkan thunk.

---

## The finding

This GPU needs roughly **4096 threads in flight** to cover a dependent memory
load, which costs **428 ns** from a 16 KiB working set and **905 ns** from
64 MiB. Throughput improves 7.4x going from 64 to 4096 threads and is flat
after that.

The hottest shader dispatches **214 invocations at a time, 414 times per
frame**. 214 invocations is 3.3 workgroups: it occupies about **10% of the
GPU** and leaves the other 29 cores idle. No such dispatch can fill the
machine, so the only cover available for its memory latency is *other
dispatches*. Honeykrisp issued every dispatch with `AGX_BARRIER_ALL`, which
emits a full CDM barrier after each one and prevents exactly that.

Same work, no barriers between it: one dispatch of 64 groups takes 2.66 ms,
the same work as 64 dispatches of 1 group takes 17.20 ms.

Letting independent dispatches actually overlap is worth **2.5x frame rate**
here. See `data/compute-breakdown.md` for where the remaining time goes.

## How it is implemented

* `HK_PERF_OVERLAP`, **on by default**; `HK_PERFTEST=nooverlap` restores the old
  behaviour.
* `hk_cdm_barrier_masked()` emits a barrier block with only some cache bits.
  Passing `AGX_BARRIER_NONE` instead does not work -- it emits *no barrier block
  at all*, and the command stream needs one for sequencing. That hangs the GPU.
* The full barrier is deferred, not deleted. It is settled at the control stream
  boundary, before every driver-internal helper kernel, and before every
  indirect dispatch. That last one matters: an indirect dispatch reading an
  unflushed grid does not draw a wrong pixel, it launches billions of
  workgroups.
* Default mask **`0x80`**, overridable with `AGX_CDM_BARRIER_MASK`.
* Also new and NOT yet measured on the game: `HK_PERFTEST=csbarrier` serves a
  compute-only `vkCmdPipelineBarrier2` with an in-stream CDM barrier instead of
  ending the control stream. Off by default until the counters say how many
  barriers would qualify.

## What the barrier bits do (measured, not documented)

`agx_cdm_barrier()` (`libagx_dgc.h:372`) sets all 23 bits of the barrier word
after every launch, under a comment admitting the bits are not understood and
this is "to be safe". Full per-bit cost and coherency data:
**`data/barrier-bit-cost.md`**. The short version, from
`tests/cstest.c CSTEST_CASE=64,1,100000` (64 one-workgroup dispatches, no
dependency between them -- a case that measures overlap and nothing else):

| mask | ms | |
|---|---|---|
| `0x0` no bits | 2.892 | the floor |
| **`0x80` bit 7 (current default)** | **2.891** | free |
| bits 17, 18, 19 | 2.891 | free |
| bits 0, 1, 2 | 4.2 - 7.4 | cheap |
| bits 4, 5, 6, 10-16 | 37.2 | serialise |
| bit 3 | 48.5 | serialises |
| bits 8, 9 | 183 | very expensive |
| `0x1f` (previous default) | 37.2 | serialises |
| all 21 bits (original) | 188.4 | serialises, and dearly |

Coherency (`tests/coherence.c`, one dispatch writes / next reads, no Vulkan
barrier) requires **bit 7 AND one of bits 3/5/6** -- one free bit and one
expensive one. `0x88`, `0xa0`, `0xc0` and `0x8f` pass; `0x80`, `0x08`, `0x18`,
`0x1f` and `0x0` fail.

### Two claims here were previously wrong

Both were inferred from the game rather than measured, and both are now
disproved:

* ~~"bit 7 is the data-coherency bit and the one that serialises"~~. Bit 7 is
  free and serialises nothing. The expensive bits are 3-6 and 8-16.
* ~~"bits 3 and 4 are descriptor state, required between any two dispatches;
  omitting them hangs the GPU"~~. The default omits both and the game runs.
  The hang that produced that belief came from the earlier attempts, which
  emitted no barrier block at all or failed to settle the deferred flush.

Cost separates into two things that were previously conflated: a **fixed
per-barrier cost** (~0.3 us at `0x1f`, ~3.3 us at the full mask, visible with
empty dispatches) and a **loss of overlap** (visible only with dispatches too
small to fill the GPU). The full mask pays both. `0x1f` removed most of the
first and none of the second -- which is why the 2.08x it delivered was NOT the
concurrency win it was described as. `0x80` removes both.

## Conformance

Same driver, mask `0x80` default against `HK_PERFTEST=nooverlap`, failure sets
compared case by case (not just counted):

| suite | passed | nooverlap fails | 0x80 fails |
|---|---|---|---|
| `dEQP-VK.memory_model.*` | 2218 | 30 | the same 30, identical set |
| `dEQP-VK.synchronization.op.single_queue.*` | 2353 | 0 | 0 |
| `dEQP-VK.compute.*` | 10736 | 0 | 0 |

15,307 tests, no regression. The 30 `memory_model.message_passing.*` failures
are pre-existing; the two failure sets were diffed programmatically and are
identical, so none of them is caused by this change. `memory_model` and
`synchronization` are precisely the suites that would catch a missing
cross-dispatch flush.

Re-run with:

    cd $HOME/Projects
    ./cts-run.sh <label> 'dEQP-VK.memory_model.*'

## Why this is safe

Not "it renders and CTS passes", which is what this section used to say. The
actual argument:

**Two dispatches can only share a compute control stream if nothing separated
them.** `vkCmdPipelineBarrier2`, `vkCmdSetEvent2`/`vkCmdWaitEvents2`, every
draw, and every query operation all end the compute stream outright
(`hk_cmd_buffer.c`, `hk_event.c`, `hk_cmd_draw.c:776`, `hk_query_pool.c`).
Vulkan therefore guarantees that dispatches sharing a stream are independent,
and independent dispatches need no coherency between them.

**The one exception is handled explicitly.** The driver issues compute for far
more than `vkCmdDispatch`: geometry/tessellation emulation, decompression, and
the libagx helper kernels. Those have real dependencies the application never
expressed. Every one of them still passes `AGX_BARRIER_ALL` -- only the
`vkCmdDispatch` path in `dispatch()` uses `AGX_BARRIER_NONE` -- and the
deferred flush is settled before them, before any indirect dispatch, and at the
stream boundary.

Residual risk, stated honestly: this rests on hk ending the stream at every one
of those points, which is true today and is not enforced by anything. If a
future change adds a synchronisation primitive that does not end the compute
stream, the weak barrier becomes unsound and nothing will fail loudly. That is
the thing to check before trusting this after a rebase.

If something renders wrong, `HK_PERFTEST=nooverlap` restores the old behaviour,
and `AGX_CDM_BARRIER_MASK=0x1fffff` restores the original barrier.

---

## Other fixes made along the way

| commit | what |
|---|---|
| `df1874767e7` | dispatch overlap (the win) |
| `df2425584d7` | `load_agx` was marked unconditionally divergent, making every AGX SSBO load falsely divergent and blocking `nir_opt_uniform_subgroup`. Real codegen win, no frame-time effect here |
| `05187f07881` | **`vkCmdWriteTimestamp2` under-reported compute GPU time by 240x.** 2 GiB of writes reported as 47 us. Breaks any app sizing work from GPU frame time |
| `5224915045d` | `AGX_OCCUPANCY` — trade spills for occupancy. Works; measured a **loss** on this game |
| `d8da82b7a11` | `merge_control_streams` was silently defeating per-dispatch timing |
| `338be749457`, `53c2d923cc5`, `7a86f2e0399` | the per-shader GPU-time profiler and its integrity counters |

## Eliminated by measurement — do not re-litigate

| hypothesis | how it died |
|---|---|
| ALU throughput / instruction count | a 53% cut in estimated cycles changed frame time by 0% |
| Pixels / resolution | 60% fewer pixels, 4% less time |
| Dispatch count | `norobust` removed 10% of dispatches, no change |
| Cache flush *duration* | measured at ~2 us; the cost was serialisation, not the flush |
| Register occupancy | `AGX_OCCUPANCY=576` was 3.7% *slower* |
| D3D feature level 11.0 | forcing `VKD3D_FEATURE_LEVEL=12_0` changed nothing; hk qualifies for 12_0 anyway |
| Broken timestamps gating dynamic resolution | fixed them; resolution stayed pinned |

## Tools

| tool | what it answers |
|---|---|
| `HK_GPUTIME=<s>` | firmware GPU timing: vertex/fragment/compute, union busy, per-shader ranking, pipeline-barrier classification |
| `HK_GPUTIME_ISOLATE=1` | one dispatch per control stream, for sound per-shader attribution |
| `AGX_DUMP_SHADER=<blake3>[,...]` | dump NIR + AGX disassembly for named shaders only |
| `AGX_CDM_BARRIER_MASK=<n>` | override the weak barrier mask |
| `HK_PERFTEST=nooverlap` | restore a full barrier after every dispatch |
| `HK_PERFTEST=csbarrier` | serve compute-only pipeline barriers in-stream instead of ending the stream (untested on the game) |
| `HK_DEBUG_DRIRC=1` | what app/engine name and driconf options the driver actually received |
| `tests/drawcost.c` | per-draw GPU overhead (use degenerate triangles, or you measure fragment throughput) |
| `tests/preambletest.c` | what a shader preamble costs, against workgroup count |
| `tests/bigconst.c` | does `nir_opt_large_constants` fire, and are its results right |
| `tests/coherence.c` | which bits give data coherency between dependent dispatches |
| `tests/statetest.c` | descriptor/uniform/pipeline state hazard across independent dispatches |
| `tests/cstest.c` | real `vkCmdDispatch` harness for driver A/B in seconds |
| `tests/memtest.c` | dependent-load latency, bandwidth, latency-hiding curve |
| `tests/disptest.c`, `tstest.c`, `waittest.c`, `drirctest.c` | per-dispatch cost, timestamp correctness, interval semantics, driconf |
| `./autorun.sh <name> [VAR=VAL]` | unattended measurement run, with `RUN_TIMEOUT` and crash detection |
| `./verify-frame.sh runs/<name>` | did it actually render, or is it fast and wrong |
| `./shaders.sh runs/<name>` | per-shader ranking from a run |
| `data/barrier-bit-cost.md` | measured per-bit barrier cost and coherency roles |
| `data/compute-breakdown.md` | where compute time goes, and why |

## What is left

Honestly: not much that is driver-side. Each remaining block has been measured
and found to be either genuine work or outside the driver.

### Nothing large and safe remains

| block | ms/frame | what it is | driver lever |
|---|---|---|---|
| compute | 17.9 | memory-bound; top shader has full occupancy, 107 GPRs, no spills, no scratch, no loops | none obvious |
| vertex | 7.8 | 95.9% genuine shading and tiling for 6886 draws/frame; an empty draw costs 47 ns | none |
| fragment | 5.0 | never investigated in detail | unknown |
| the ~9.3 ms stall | 9.3 | not GPU, not the swapchain; most likely the game's CPU work under FEX | none |
| ~5.1 ms of drain gaps | 5.1 | control stream boundaries | `csbarrier` measured: no effect |

### The candidates that remain, smallest risk first

1. **Fragment, 5.0 ms.** The only block never examined in detail. Cheap to
    look at and might hold something.
2. **`bd65ca414801`**, still 255 GPRs / 108:71 spills / occupancy 384, worth
    3.6% of compute. `agx_spill.c:can_remat()` handles only `MOV_IMM` and
    `GET_SR`.
3. **Identify the ~9.3 ms stall.** Not fixable here, but it bounds everything
    else, and if it is the Vulkan thunk rather than the game then it is
    someone's to fix.
4. **Driver helper kernels**, 468 dispatches/frame, bounded at ~1.4 ms.

### What would need application changes

6886 draws per frame and the geometry behind them. Both belong to the game.

## Closed -- do not re-open

| avenue | why it is closed |
|---|---|
| **Narrowing the barrier mask further** | `0x80` measures 2.891 ms against a 2.892 ms floor at `0x0`. There is nothing left. |
| **More dispatch concurrency** | The GPU absorbs ~64 independent dispatches at once (`data/barrier-bit-cost.md`); the game's ~414 small dispatches per frame already have that width available. |
| **Per-draw overhead in the vertex phase** | An empty draw costs 47 ns (`tests/drawcost.c`); 6886 of them are 0.32 ms of the 7.8 ms vertex phase. The other 95.9% is real work. |
| **The shader preamble** | Costs ~nothing even at one workgroup and only helps at scale (`tests/preambletest.c`). Disabling it is a 44% compute regression. |
| **Enabling `nir_opt_large_constants` alone** | Done, and it needed the GPU-side plumbing too -- `hk_upload_shader()` never uploaded the data. Also had to move ahead of `agx_preprocess_nir`, which lowers >256-byte arrays to scratch first. |
| **Compute-only barriers in-stream** (`HK_PERFTEST=csbarrier`) | Implemented and measured: 16.95 vs 16.88 fps, compute 35.2 vs 35.0 ms, compute streams per frame 51.8 vs 51.7. No effect -- the barriers it skips are followed by a draw that ends the stream anyway. Flag kept, default off. |
| **Vsync / present mode** | `MESA_VK_WSI_PRESENT_MODE=immediate` moved neither frame rate nor gap structure, and single gaps reach 13.6 ms against an 8.333 ms vblank. |
| everything in the table above | see "Eliminated by measurement" |

Full detail, including every wrong turn and why it was wrong, is in
`CHECKLIST.md`.
