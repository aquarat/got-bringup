# Honeykrisp tessellation: where the time goes, and what to do about it

Analysis 2026-09-03 against `$HOME/Projects/mesa-local`. Read-only; nothing modified.
VERIFIED = read in the code. INFERRED = reasoning about likely cost.

---

## CORRECTION FIRST: AGX appears to HAVE a hardware tessellator

Earlier in this session I stated AGX has no hardware tessellator. **That was wrong.**
`src/asahi/genxml/cmdbuf.xml` carries reverse-engineered packets for one:

    :839  <struct name="VDM State Tessellation" size="4">
            Step function (Constant/Per patch/Per instance), Factor type (FP16/FP32),
            Patch type (Triangles/Quads), Max tess factor (up to 64),
            Counterclockwise winding, Partition mode (Pow2/Integer/Frac odd/Frac even)
    :925  <struct name="VDM Tessellate" size="4">
            + Factor buffer / Patch count / Instance count / Base patch / Indirect
    :745  VDM Block Type has  Tessellate = 5

These correspond to Metal's `drawPatches` path. **They are never emitted.** The only
reference anywhere in the tree is `decode.c:696`, which prints them. Honeykrisp
emulates tessellation entirely in compute instead.

Note the XML's own hedging (`XXX: Order is an (educated) guess`, `XXX: Out of bits!!!`),
so the encoding is not fully understood. And kosmickrisp — which runs on Metal, where
`drawPatches` is a *documented* API — **also** uses the software path
(`kk_cmd_draw.c:1856-1944`, same 5-dispatch structure). That is a signal the previous
authors judged HW tessellation insufficiently conformant or general, not that it is slow.

---

## The dispatch sequence (VERIFIED)

5 dispatches per direct tessellated draw, 6 if indirect. `hk_cmd_draw.c:1586-1675`:

| # | Dispatch | Grid / workgroup | Notes |
|---|---|---|---|
| 0† | `libagx_tess_setup_indirect` :1628 | `agx_1d(1)` / **1 thread** | indirect draws only |
| 1 | VS as compute :1654 | verts x instances / 64 | writes VS outputs |
| 2 | TCS :1659 | patches x patch_size / **patch_size** | writes tcs_buffer + factors |
| 3 | `libagx_tess_*` COUNT :1664 | patches / 64 | writes counts[patch] |
| 4 | `libagx_prefix_sum_tess` :1667 | `agx_1d(1024)` / **1024 = ONE workgroup** | in-place scan, allocates IB |
| 5 | `libagx_tess_*` WITH_COUNTS :1670 | patches / 64 | domain points + index buffer |

The tessellator itself moved out of `src/asahi/` into shared `src/poly/`
(`src/poly/cl/tessellator.h` is the 1609-line MS D3D11 reference tessellator).

---

## Where the cost most plausibly is

### 1. A full cache flush after EVERY dispatch  (VERIFIED mechanism, INFERRED magnitude)

`hk_cmd_dispatch.c:56-68` — every dispatch is unconditionally followed by
`hk_cdm_cache_flush()`. The `enum agx_barrier` argument exists but is **never consulted**
to decide whether to flush; its only use is picking the target control stream
(`hk_cmd_buffer.c:763`). `AGX_BARRIER_NONE` appears nowhere outside its own enum.

And the barrier is maximally conservative — `libagx_dgc.h:372-413`:

    /* ... Until we know what bits mean what exactly, let's just set these
     * after every launch to be safe. */
    cfg.unk_0 = true; cfg.unk_1 = true; cfg.unk_2 = true;
    cfg.usc_cache_inval = true;
    cfg.unk_4 ... cfg.unk_19 = true;    // every single bit

So each tessellated draw costs **5 full pipeline drains + total cache invalidations**,
including one between draw N's last dispatch and draw N+1's first, which are
independent. `usc_cache_inval` also forces TCS output to round-trip to DRAM before the
COUNT dispatch reads it, and again before TESSELLATE.

Foliage draws are typically many and small. 2000 patches / WG 64 = 31 workgroups on a
32-core M1 Max: one workgroup per core, then a full drain. Five times. The GPU is never
filled. **This is the #1 suspect.**

### 2. The prefix sum is a single workgroup  (VERIFIED)

`hk_cmd_draw.c:1667` dispatches `agx_1d(1024)` against `KERNEL(1024)` — one threadgroup,
one core, looping `nr_patches/1024` with two local barriers per iteration
(`src/poly/geometry.h:542-570`). 1/32 of the GPU, strictly serial, mid-pipeline, once
per draw. It also does the index-buffer heap allocation and writes the draw descriptor
from a single elected thread.

### 3. TCS runs at ~3/32 lane occupancy  (VERIFIED code, INFERRED impact)

`hk_cmd_draw.c:1659-1661` sets workgroup size == output patch size. With
`subgroupSize = 32` and a typical 3- or 4-control-point patch, that is **3 of 32 lanes
active**, one threadgroup slot burned per patch. `poly_nir_lower_tess.c:135-145`
confirms the design assumption ("a patch fits in a subgroup").

### 4. Everything round-trips through DRAM  (VERIFIED)

`poly_nir_lower_tess.c:161-190` lowers every TCS output to `nir_store_global` and every
TES input to `nir_load_global_constant`. Per draw the GPU writes and re-reads VS outputs,
TCS outputs, counts (twice), domain points, and the index buffer. AMD keeps all of this
in LDS / parameter cache. This is the fundamental emulation tax, amplified by item 1's
cache invalidations.

### 5. Per-patch global atomics  (VERIFIED)

`src/poly/geometry.h:120-137` — a bare `atomic_fetch_add` on one `heap->bottom` word per
patch, no subgroup aggregation. N patches = N serialised atomics on one cacheline.

### 6. CPU-side per-draw work — matters more here because of FEX  (VERIFIED)

- `hk_cmd_draw.c:3067-3071` sets `root_dirty = true` on every tessellated draw, forcing a
  ~1.8 KB root descriptor re-upload, only because the tess_params address changed.
- 2x `hk_upload_usc_words()` rebuilt per draw; 3x `hk_dispatch_precomp` each doing a pool
  alloc + upload.
- `hk_pool_alloc_internal` (`hk_cmd_buffer.c:193-203`) falls off a cliff above 128 KB and
  calls `agx_bo_create()`. A draw with ~7k patches and a 32-dword TCS stride exceeds that.

### 7. One ioctl per control stream  (VERIFIED)

`hk_queue.c:253-256`: `max_commands_per_submit()` returns **1** unless `HK_PERFTEST=batch`.
Tessellation adds a CDM stream per render pass, roughly doubling submit count. Under FEX
that syscall thunking is expensive. Batching is disabled over a 7ppm CTS flake (:249-252).

---

## Ranked optimisation candidates

### 1. Use the hardware tessellator  (SPECULATIVE, very high ceiling, weeks)
Deletes dispatches 3, 4 and 5 outright, plus 3 of 5 cache flushes, all domain-point and
index-buffer heap traffic, every per-patch atomic, and the serial prefix sum. Needs a new
`agx_vdm_tessellate()`, changes to `hk_launch_tess()`, and a post-tessellation VS ABI in
`agx_compile.c` (nothing in the compiler knows about it today). High risk: several bit
positions are guesses; no isolines or point mode (Patch type is 1 bit), so the software
path must remain as fallback. Bring-up needs a Metal trace + `ASAHI_MESA_DEBUG=trace`.

### 2. Stop flushing after every dispatch  (VERIFIED problem, best non-HW win)
**2a**, ~50 lines: honour the existing `agx_barrier` argument; skip the flush for
`AGX_BARRIER_NONE`; drop the barrier after each draw's last dispatch, since draw N+1's VS
does not depend on draw N's tessellate output.
**2b**, ~300-500 lines: batch by stage across a render pass —
`[VS x N][barrier][TCS x N][barrier][COUNT x N][barrier][PREFIX x N][barrier][TESS x N]`.
5N dispatches + 5N barriers becomes 5N + 4, and every phase has N draws of parallelism to
fill the GPU. **The single biggest win available without hardware tessellation.**
Risk: the "set every bit to be safe" comment exists for a reason, and the CTS batch flake
at `hk_queue.c:249` may be the same missing-flush bug.

### 3. Fuse COUNT into the TCS epilogue  (your idea, 5 -> 4, medium)
Feasible: the COUNT path is purely analytic, needing only tess factors plus
partitioning/points_mode, ending in a closed form at `src/poly/cl/tessellator.h:823-1072`.
A TCS workgroup is exactly one patch, so `invocation_id == 0` can run it after the
existing end-of-shader barrier. Risk: inlines a few hundred instructions into every TCS
variant (register pressure, spilling), and only 1 lane of an already tiny workgroup runs it.

### 4. Parallelise the prefix sum  (low risk, small-medium)
Decoupled-lookback single-pass scan. **Do not** substitute plain atomic allocation —
Vulkan requires primitive order to follow patch order, and `poly_draw`
(`src/poly/cl/tessellator.h:131-149`) relies on `counts[patch-1]` being an ordered
exclusive offset. If #3 lands this shrinks to a ~20-line 1-thread kernel.

### 5. Pack multiple patches per TCS workgroup  (medium)
`K = 32 / output_patch_size` patches per workgroup; update `load_primitive_id` /
`load_invocation_id` in `poly_nir_lower_tess.c:135-145` and `poly_tcs_unrolled_id`.
Barriers are already `SCOPE_SUBGROUP`, and a subgroup barrier over K packed patches is a
superset of what is needed, so it stays correct. Takes TCS from ~3/32 to ~30/32 lanes.

### 6. Subgroup-aggregate the heap atomics  (~30 lines, low risk)
One `sub_group_scan_exclusive_add` + one atomic per subgroup instead of per lane.
Benefits GS too.

### 7. Cheap CPU-side wins  (small, low risk)
Give tess params a stable per-command-buffer slot so `root_dirty` stays clear; cache the
VS/TCS USC words. The comment at `hk_cmd_draw.c:3085-3092` already flags this area as
"a mess ported over from the GL driver".

---

## Measurement knobs (VERIFIED names)

    HK_PERFTEST=notess        skip tessellated draws  (hk_device.c:42, used hk_cmd_draw.c:3554)
    HK_PERFTEST=batch         64 control streams per ioctl instead of 1  (hk_queue.c:253)
    HK_PERFTEST=nobarrier     ignore vkCmdPipelineBarrier -- NOT the CDM flush
    ASAHI_MESA_DEBUG=perf     per stream: "%u API calls, %u dispatches, %u flushes, %u merged"
                              (hk_queue.c:828); "Tessellation"/"Indirect tessellation" per draw
                              (hk_cmd_draw.c:1602,1609)
    ASAHI_MESA_DEBUG=sync     serialise submits -- separates GPU from CPU time
    ASAHI_MESA_DEBUG=trace    full command-stream decode
    ASAHI_MESA_DEBUG=nomerge  disable CDM stream merging

No driconf option affects tessellation.

### Order to measure in

1. `HK_PERFTEST=notess`. 6 -> 25+ fps means tessellation is the bottleneck and all of the
   above applies. 6 -> 8 means stop and look elsewhere.
2. `ASAHI_MESA_DEBUG=perf` for one frame: gives dispatches and flushes per control stream.
   Multiply by frame time for a per-dispatch budget. Directly tests suspect #1.
3. `HK_PERFTEST=batch` alone: isolates ioctl/FEX submit cost from GPU cost.
4. Bounding experiments (incorrect but informative): comment out `hk_cdm_cache_flush` at
   `hk_cmd_dispatch.c:66`; or over-spawn the prefix-sum grid at `hk_cmd_draw.c:1667`.

## Unknown

- The split of GPU time across the 5 dispatches. No per-dispatch timing exists;
  `hk_cs.timestamp` is per control stream.
- GoT's actual patch size, patches per draw, and tessellated draws per frame. Decides
  whether #5 is a 10% or a 3x effect, and whether #2b (many small draws) or #4
  (few large draws) matters more.
- Whether the draws are direct or indirect — vkd3d may use ExecuteIndirect, adding a 6th
  single-threaded dispatch plus another flush. `perf_debug "Indirect tessellation"` answers it.
- The real cost of one `agx_cdm_barrier` — unknown-bit semantics are un-RE'd.
- Whether `VDM Tessellate` works as documented. Only a Metal trace would settle it.
- Whether AGX truly wastes 29/32 lanes on a 3-thread threadgroup. A microbenchmark
  comparing 3-thread vs 32-thread workgroups at equal total threads settles it in minutes.

---

# RESULT: TESSELLATION IS NOT THE BOTTLENECK. Hypothesis dead.

Measured on the deployed optimised build (`git-9de016a538`), gameplay:

    HK_PERFTEST unset      ~6 fps
    HK_PERFTEST=notess     ~6 fps        <-- no change
    visual difference      "doesn't look vastly different"

`notess` skips tessellated draws **entirely** (`hk_cmd_draw.c:3554`). If removing
every tessellated draw neither changes the frame rate nor materially changes the
image, then Ghost of Tsushima barely uses tessellation on this stack. The
premise was wrong from the start.

**What this invalidates:** the whole chain of reasoning that led here — Legion
Go 2 has a hardware tessellator, AGX emulates tessellation in compute, therefore
the ~10x gap is emulated tessellation. Plausible, internally consistent, and
wrong. The Legion Go comparison was suggestive, not evidence, and I treated it
as more than it was.

**What the three optimisations are still worth.** They are real inefficiencies,
correctly identified and now fixed, and they are validated:
  - barrier elision: -7.5% CDM flushes, zero CTS diff over 3 runs + 4 groups
  - parallel prefix sum: -9.6% to -16.5% on 16k-64k patch draws, CRC-verified
    across 27 configs
  - TCS patch packing: 3/32 -> 30/32 lane occupancy, zero CTS diff over 7 groups
They just do not touch what limits THIS game. Anything tessellation-heavy will
benefit; this title is not that.

Also worth noting: item 6 (subgroup-aggregate the heap atomics) was rejected
because the AGX compiler already does it. So of five candidates, one was
already done, three are real-but-irrelevant-here, and the big one (hardware
tessellator) is now clearly not worth the weeks it would cost for this title.

## What the evidence actually says now

Measured at 6 fps during gameplay, with the deployed build:

    power    41.9 W        idle 17.0, vkcube 23.0
    psi mem  avg10=0.00    psi io 0.00    psi cpu 0.05
    host mem 26.0/31.6 GB used, 5.6 GB available
    VM cpu   160% of 800%  (2 of 8 cores)
    loadavg  2.18

The SoC burns 42 W to produce 6 fps. Not a stall, not memory, not CPU (two cores
busy, no CPU pressure). **The GPU is saturated.** It is doing roughly 10x the
work it should for this scene, and tessellation is not the reason.

## Next candidates — none tested, ordered by fit with the evidence

1. **The game is told it is not a tiler.**
   `d3d12_device_CheckFeatureSupport: Assuming device does not support tile
   based rendering.` On a TBDR GPU that is potentially enormous: the engine will
   order render passes for an immediate-mode desktop GPU, defeating tile memory
   and forcing full framebuffer traffic to DRAM per pass. Fits the evidence
   better than anything else: GPU busy, bandwidth-bound rather than
   shading-bound, and largely insensitive to quality settings AND resolution --
   which is exactly what has been observed.
2. **Depth format emulation.** `Mapping VK_FORMAT_D24_UNORM_S8_UINT to
   VK_FORMAT_D32_SFLOAT_S8_UINT` = 1.5x depth bandwidth on a bandwidth-bound
   part.
3. **The 4 KiB -> 64 KiB alignment rejection** (7,204 per session): memory bloat
   and poor locality. Driver exports `VK_MESA_image_alignment_control` yet vkd3d
   takes the hard-reject path.
4. **EXT_dgc skipped**, so `ExecuteIndirect` loses its GPU-driven path.

## The measurement to take next

    ASAHI_MESA_DEBUG=perf,stats     which driver slow path is actually hit
    ASAHI_MESA_DEBUG=sync           separates GPU time from CPU time

Do this BEFORE writing any more code. The lesson from today is that a coherent
story is not evidence, and 6 hours of implementation went to a hypothesis that
one 5-minute `notess` run would have falsified at the outset.
