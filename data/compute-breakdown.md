# Where the compute time goes

Compute is ~80% of GPU busy time in this game, so "optimise compute" is the
whole problem. This is the attribution.

## How the ranking was obtained, and why the obvious method is wrong

The profiler can only charge a firmware interval to a shader when the control
stream it timed contained just that one shader. In normal operation a stream
holds ~31 dispatches from several shaders, so most time lands in the
unattributable bucket and the ranking is biased towards whichever shaders
happen to run alone.

`HK_GPUTIME_ISOLATE=1` ends the control stream after every dispatch, so every
dispatch is timed individually and attribution is complete. It perturbs --
~56 streams per frame become ~1780, and frame time roughly doubles -- but a
stream costs ~2 us to start against the ~2400 us these streams take, so the
RANKING is sound even though the absolute frame time is not.

Numbers below are from `runs/isolate3` (29 windows, 685 frames), which is the
only full-coverage measurement. Top 24 shaders account for 112 of 137.7 ms of
compute per frame (82%).

## The ranking

| id | disp/frame | invoc/frame | ms/frame | share | instrs | gprs | occupancy | spill+fill |
|---|---|---|---|---|---|---|---|---|
| 159 `dbe39607fa5d` | 413.6 | 88568 | **42.70** | 38.0% | 2708 | 255 | 384 | 272 |
| 160 `6fc0efe726d2` | 16.8 | 1102440 | **18.78** | 16.7% | 2540 | 255 | 384 | 272 |
| 285 `c0408dcabfa6` | 105.1 | 53825 | 18.16 | 16.1% | 12705 | 207 | 512 | 9 |
| 286 `dc6351ad59d8` | 56.5 | 3618 | 11.07 | 9.8% | 10653 | 215 | 448 | 0 |
| 287 `18a1bc3d2c5b` | 27.3 | 13980 | 6.63 | 5.9% | 13835 | 207 | 512 | 17 |

Five shaders are 97.3 of 137.7 ms -- **71% of all compute time**. Two of them,
159 and 160, have identical register and spill statistics and are almost
certainly two variants of one shader.

Driver helper kernels are 411 dispatches/frame across 13 kernels
(`draw_robust_index_1` 145/frame, `copy_uint4` 96/frame) but a small share of
time.

## Shader 159: the cost is spill traffic, not arithmetic

Everything below is per dispatch, and shader 159 runs 414 of them per frame.

```
invocations per dispatch : 214  (3.35 workgroups of 64)
measured                 : 103.2 us
ALU estimate             :   1.49 us   (1936 cycles @ 1.296 GHz)
unexplained              : 101.7 us
  spread over 272 spill+fill instructions -> 374 ns each
```

Measured dependent-load latency on this GPU is 428 ns from a 16 KiB working set
and 905 ns from 64 MiB. 374 ns sits just under the small-working-set figure,
which is what spill memory is: small, hot, and partly overlappable.

This shader has **no hardware loops** (`loops` = 0), so it is straight-line
code and the static spill count IS the dynamic spill count. That is what makes
the arithmetic above a real accounting rather than a coincidence.

It is still a fit, not a proof. The test that would settle it is to compile the
same shader with fewer spills and see the time fall.

## Why it spills, and why that is not the register allocator's fault

`agx_max_registers_for_occupancy()` gives a 64-thread workgroup the full
register file, so `max_possible_regs` is 256 half-registers -- the
architectural maximum (`AGX_NUM_REGS`). The shader uses 255 and spills anyway.
It genuinely needs more than 128 32-bit values live at once.

So the allocator is not being throttled; there is nothing to un-throttle. Note
that this also explains why `AGX_OCCUPANCY=576` measured 3.7% SLOWER: it buys
occupancy by forcing more spilling, which is the wrong side of this trade.

The occupancy table (`agx_performance.c`) is:

| half-registers | threads |
|---|---|
| 104 | 1024 |
| 128 | 832 |
| 160 | 640 |
| 208 | 512 |
| 256 | **384** |

At 255 registers these shaders sit in the worst tier, 384 threads against a
possible 1024. So the register pressure costs twice: it spills, AND it leaves
only a third of the machine's threads available to hide the spills.

## Two independent levers

1. **More dispatches in flight.** A 214-invocation dispatch occupies 3.3 of 32
   cores -- 10% of the GPU. Nothing else can cover its memory latency, so the
   only cover available is other dispatches. This is what the weak CDM barrier
   is for, and it is why mask `0x80` (real overlap) beat `0x1f` (cheap barriers
   but still serialised).

2. **Fewer live values.** Rematerialisation in `agx_spill.c:can_remat()`
   currently handles exactly two opcodes, `MOV_IMM` and `GET_SR`. Extending it
   to cheap ALU whose sources are still live is the standard fix for a
   spill-bound shader and would attack both the spill traffic and the
   occupancy tier at once.

---

# The hot shader, read

`6fc0efe726d2` (16-38% of compute depending on scene) was dumped with
`AGX_DUMP_SHADER` after fixing two bugs in that tool -- see CHECKLIST item 42.

## What it is

```
workgroup_size: 8, 8, 1        num_ssbos: 4
uses_wide_subgroup_intrinsics  uses_texture_gather
scratch: 2560                  <- declared by the SHADER, before any spilling
```

Final compiler stats:

```
2540 instrs, 2014 alu, 680 ic, 18038 code size, 255 gprs, 512 uniforms,
3104 scratch, 384 threads, 2 loops, 194:78 spills:fills, 2104 preamble inst
```

## The finding: a constant table written into per-thread scratch

The shader opens by storing a compile-time-constant lookup table into scratch,
one 16-byte vector at a time:

```
32       %0 = load_const (0x0000005b = 91)
32       %1 = load_const (0x00000097 = 151)
32       %2 = load_const (0x000000a0 = 160)
32       %3 = load_const (0x00000089 = 137)
32x4     %4 = vec4 %1, %2, %3, %0
              @store_scratch (%4, %5 (0x0))
```

160 such stores -- 160 x 16 = 2560 bytes, exactly the declared scratch -- then
44 dynamically-indexed `load_scratch` reads. They survive to the final code:
**514 `stack_store` and 244 `stack_load` instructions** in the AIR, at
consecutive offsets `#0, #16, #32, #48 ...`.

Scratch is PER-INVOCATION private memory. So every one of the ~1.38 million
invocations per frame builds its own private copy of the same constant table
before reading from it.

Note the shader has 44 `if_fcmp` and 2 loops, so the STATIC instruction counts
above are not the dynamic ones. The size of the prize is not yet measured; what
is established is that the work is unnecessary, not how much of it runs.

## Why the driver leaves it there

`nir_opt_large_constants` is exactly the pass for this: it finds function-temp
arrays that are constant-initialised and never written afterwards, and moves
them into `nir->constant_data` so the array is read from shared constant memory
instead of being rebuilt per thread. RADV, ANV, iris, radeonsi and freedreno
all run it.

Honeykrisp does not. `src/asahi/vulkan/hk_shader.c:841`:

```c
// NIR_PASS(_, nir, nir_opt_large_constants, NULL, 32);
```

Commented out, with no explanation, since the commit that added the driver
(`5bc82848163 hk: add Vulkan driver for Apple GPUs`).

## It is not a one-line fix

The CPU half of the plumbing exists -- `hk_shader.c:1225` captures
`nir->constant_data` into `shader->data_ptr`, and it is serialised into the
pipeline cache. But `hk_upload_shader()` uploads only the binary. Nothing ever
places that data in GPU memory or binds an address to it, so enabling the pass
today would generate loads from a buffer that does not exist.

The backend's existing `agx_rodata` path is not the answer either: it lands
constants in UNIFORM REGISTERS (`agx_usc_immediates`, `hk_shader.c:1857`), a
scarce resource this shader already uses 512 of, and uniform registers cannot
be indexed dynamically.

What is needed is a real constant-data buffer: append `nir->constant_data` to
the shader BO, reserve a uniform slot for its base address, fill that slot via
USC at bind time, and lower `nir_var_mem_constant` with
`nir_lower_explicit_io` against it. Tractable, but a driver feature rather than
uncommenting a line.

---

# Result: the constant table is gone

Implemented in `6a71e0feba7`. `runs/largeconst`, against `runs/clean80` as the
control (same driver otherwise, same save, same navigation).

## The shader that started this

`6fc0efe726d2`, same 17.0 dispatches and 1,114,112 invocations per frame in
both runs, so this is like-for-like on identical work:

| | before | after |
|---|---|---|
| measured ms/frame | 4.24 | **0.22** |
| scratch | 3104 | **0** |
| spills:fills | 194:78 | **0:0** |
| GPRs | 255 | **119** |
| occupancy (threads) | 384 | **832** |
| instructions | 2540 | 1974 |
| cycles/invocation | 2014 | 1648 |

**19x less measured GPU time.** And note what happened to the register
pressure: 255 GPRs down to 119, which moves the shader from the worst
occupancy tier to more than double the threads in flight. Addressing the
constant table also fixed the spilling, so the rematerialisation work that was
item 2 on the list is largely moot for this shader -- the spills were the
table's addressing, not an inherent register shortage.

## Whole frame

| | `clean80` | `largeconst` |
|---|---|---|
| compute ms/frame | 35.0 | **24.8** |
| GPU busy ms/frame | 45.6 | **40.2** |
| fps | 16.88 | 17.96 |
| compute as share of GPU busy | 76.7% | 61.7% |

Compute down **29%**. Take the fps with the usual caution about scene variance;
the compute figure is the one to trust, and it is a much larger move than the
noise. Note also that `largeconst` ran with a cold shader cache -- the pipeline
cache UUID changed with the driver build -- so it was recompiling throughout,
which if anything depressed it.

Across all profiled shaders, scratch fell from 3232 bytes in 3 shaders to 116
bytes in 1.

Compute is no longer the overwhelming majority of GPU time: it was 76.7% of
busy and is now 61.7%, with fragment work rising to 13.5% of wall. The next
investigation should re-rank from scratch rather than assuming compute.
