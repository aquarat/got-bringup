# The same driver on a second GPU: M1 Ultra (G13D)

Everything else in this repository was measured on an **M1 Max, G13C, 32 cores,
one die**, driving a desktop. This page is the same driver measured on an
**M1 Ultra, G13D C0, 48 active cores across 8 clusters, two dies over
UltraFusion**, in a headless Mac Studio (`apple,t6002` / `j375d`) with no
compositor and nothing else using the GPU.

Two reasons it is worth having:

* Every constant in this repository was a single-part measurement. Until now
  there was no way to tell which numbers describe *this architecture* and which
  describe *that chip*. Most of them turn out to be architectural.
* The workload is not the game. It is `llama.cpp` serving a 27B model through
  the Vulkan backend — pure compute, no draws, no FEX, no pressure-vessel.
  That is a much cleaner test of the dispatch-overlap change than a game is,
  and it gives the change a second application to justify it.

Driver: the pinned fork commit `d105715f01c` (`mesa-source.env`), built with
`./build-driver.sh`, reached with `VK_DRIVER_FILES` only. The system driver
(`mesa-vulkan-drivers-26.2.2-6.fc44`) was never replaced.

Measured 2026-09-20.

---

## Headline: the overlap fix is worth 11% of token generation

`llama-server` running `unsloth/Qwen3.6-27B-GGUF:UD-Q4_K_XL` (16.40 GiB,
`qwen35` — a hybrid SSM/attention architecture, 64 blocks, full attention every
4th), 4096-token context, one slot. Fixed prompt, 140 tokens generated,
`temperature 0`.

| driver | ms/token | tok/s |
|---|---|---|
| stock Mesa 26.2.2 | 121.07 | 8.26 |
| patched, `HK_PERFTEST=nooverlap` | 121.51 / 120.58 | 8.23 / 8.29 |
| **patched, default mask `0x80`** | **108.82 / 109.18** | **9.19 / 9.16** |
| patched, `HK_PERFTEST=csbarrier` | 108.93 / 108.36 | 9.18 / 9.23 |

**121.05 -> 109.00 ms/token, a 11.1% reduction**, reproducible to better than
0.5% between runs.

The `nooverlap` arm is the control that makes this attributable. The patched
driver is also a newer Mesa (26.3.0-devel against the distro's 26.2.2), so the
comparison that matters is not patched-against-stock but patched-against-itself
with the fix disabled. That arm lands on 121.5 / 120.6 against stock's 121.07 —
i.e. **the newer Mesa contributes nothing, and the entire gain is the weak CDM
barrier.**

### `csbarrier` was predicted to be the big lever here, and is not

The reasoning was: `HK_PERFTEST=csbarrier` was measured worthless on the game
*because* "the barriers it skips are followed by a draw that ends the stream
anyway" (`data/pipeline-barriers.md`), and llama.cpp issues no draws at all.
Its `ggml_vk_sync_buffers` emits one global memory barrier with compute stage
flags and no image barrier — which is exactly this repository's own definition
of a compute-only barrier. The game was 6.4% compute-only; llama is ~100%.

It makes no measurable difference: 108.65 ms/token against the default's
109.00, a 0.35 ms gap that is smaller than the spread within either arm. The
overlap fix alone captures the win. `csbarrier` remains default-off, now with
a second workload behind that decision rather than one.

---

## Per-bit barrier cost, G13D against G13C

`tests/cstest.c CSTEST_CASE=64,1,100000`, minimum of 3 runs. The G13C column is
`data/barrier-bit-cost.md`.

| mask | G13D ms | G13C ms | |
|---|---|---|---|
| `0x0` no bits | 4.147 | 2.892 | the floor |
| bit 7 `0x80` | **3.136** | 2.891 | free |
| bit 17 `0x20000` | 3.168 | 2.891 | free |
| bit 18 `0x40000` | 3.481 | 2.891 | free |
| bit 19 `0x80000` | 4.043 | 2.894 | free |
| bit 20 `0x100000` | 4.444 | 3.470 | nearly free |
| bit 2 `0x4` | 4.058 | 5.030 | cheap |
| bit 1 `0x2` | 4.195 | 4.229 | cheap |
| bit 0 `0x1` | 4.748 | 7.443 | cheap |
| bit 6 `0x40` | 37.563 | 37.177 | serialises |
| bit 5 `0x20` | 37.596 | 37.172 | serialises |
| bit 3 `0x8` | 37.614 | **48.498** | serialises |
| bit 4 `0x10` | 37.618 | 37.176 | serialises |
| bits 10, 12, 16 | 37.62-37.64 | 37.17-37.18 | serialise |
| bit 9 `0x200` | 183.338 | 183.284 | very expensive |
| bit 8 `0x100` | 183.400 | 182.927 | very expensive |
| `0x1f` | 37.646 | 37.18 | serialises |
| all bits | 183.435 | 188.447 | serialises, and dearly |

**The structure is architectural, not per-part.** The same three cost tiers, the
same bits in each, the same non-additivity (`0x1f` costs less than bit 3 would
if these were independent waits). A driver tuned on one part is tuned for the
other.

One G13C anomaly does **not** reproduce: bit 3 was the worst single bit there
(48.5 ms against 37.2 for its neighbours) and here it sits with them at 37.6.
That 30% excess appears to be a G13C property, or was noise on a contended
machine. Nothing in the driver depends on it.

Note `0x80` measuring 3.136 against a `0x0` floor of 4.147 — the "free" bit
appears slightly *faster* than no bits at all. That is within this test's
run-to-run spread and should not be read as bit 7 saving time. What it does
confirm is the conclusion `barrier-bit-cost.md` already reached: there is
nothing left to win by narrowing the mask further.

## Coherency is identical

`tests/coherence.c`, 200 trials per mask, 8192 elements:

| mask | G13D | G13C |
|---|---|---|
| `0x88` (3+7) | PASS | PASS |
| `0xa0` (5+7) | PASS | PASS |
| `0xc0` (6+7) | PASS | PASS |
| `0x8f` (0,1,2,3,7) | PASS | PASS |
| `0x80` (7 alone) | FAIL | FAIL |
| `0x08` (3 alone) | FAIL | FAIL |
| `0x1f` | FAIL | FAIL |
| `0x0` | FAIL | FAIL |

Bit 7 AND one of bits 3/5/6, on both parts, every trial. Whatever these bits
select, it is a property of the architecture.

---

## Concurrency ceiling: ~96-100 dispatches, and it tracks core count

`tests/concurrency.sh` equivalent on the patched driver at the default mask,
minimum of 5 runs per point. One workgroup and a long serial loop per dispatch,
so a single dispatch cannot fill the machine.

| dispatches | G13D ms | ms/dispatch |
|---|---|---|
| 32 | 3.347 | 0.1046 |
| 64 | 4.116 | 0.0643 |
| 128 | 6.492 | 0.0507 |
| 192 | 6.203 | 0.0323 |
| 256 | 8.957 | 0.0350 |
| 320 | 11.820 | 0.0369 |
| 384 | 11.842 | 0.0308 |
| 512 | 17.502 | 0.0342 |
| 768 | 23.243 | 0.0303 |
| 1024 | 31.814 | 0.0311 |

Against a single-dispatch floor of ~3.2 ms, the implied batch count
(`total / floor`) gives a ceiling converging on **~96-100 concurrent
single-workgroup dispatches**: 2.0 batches at 128, 2.8 at 256, 5.5 at 512,
9.9 at 1024.

G13C measured ~64 on 32 cores. G13D measures ~96-100 on 48 cores. **Both are
about two dispatches per core**, so this is a scaling rule rather than a
constant, and `barrier-bit-cost.md`'s "about 64" should be read as "about 2N".

## Per-dispatch overhead: 4.43 us -> 0.06 us

`CSTEST_CASE=<d>,1,1` — trivial work, so this is launch plus barrier and
nothing else. Minimum of 3 runs.

| dispatches | full mask ms | `0x80` ms | `0x0` ms |
|---|---|---|---|
| 256 | 1.528 | 0.335 | 0.226 |
| 1024 | 5.445 | 0.432 | 0.366 |
| 4096 | 19.070 | 0.617 | 0.646 |

Marginal cost per dispatch, from the 1024 -> 4096 slope with fixed submit cost
eliminated:

| mask | us per dispatch |
|---|---|
| full | **4.43** |
| `0x80` | **0.060** |
| `0x0` | 0.091 |

**A ~73x cut in per-dispatch overhead**, and `0x80` is at the `0x0` floor.
G13C's figures were ~3.9 us at the full mask and ~0.66 us at `0x0`; the full
barrier is somewhat more expensive on the Ultra, which is consistent with a
cache flush having to settle across two dies but is not evidence for it — the
gap is small and no experiment here isolates the cause.

This is the number that explains the llama result. At roughly a thousand
dispatches per token, 4.4 us each is ~4.4 ms; the measured saving is ~12 ms, so
per-dispatch overhead is a large part of it but not all — the rest is the
serialisation itself, on the many small ops (single-row RMS norms, elementwise
adds, rope, the SSM conv and scan in 48 of the 64 blocks) that individually
occupy a fraction of a percent of the GPU.

---

## What the stock driver does, for the record

The same sweeps before any patched driver existed, on
`mesa-vulkan-drivers-26.2.2-6.fc44`:

| dispatches | ms | ms/dispatch |
|---|---|---|
| 1 | 4.425 | 4.4250 |
| 4 | 11.810 | 2.9525 |
| 16 | 46.215 | 2.8884 |
| 64 | 183.466 | 2.8667 |
| 256 | 732.518 | 2.8614 |

Perfectly linear, ms/dispatch flat across a 64x range. **Concurrency on the
stock driver is exactly 1.** Stock G13D (183.47 ms at 64 dispatches) and stock
G13C (188.4 ms) are within 3% of each other: when only one workgroup runs at a
time, 48 cores and 32 cores perform identically, and the Ultra's extra silicon
is unreachable.

A single dispatch, sweeping workgroups, saturates at **~512 workgroups =
16,384 threads** (flat from 1 to 512, 7.5 ms at 1024, 17.3 ms at 2048). That
is the bar a dispatch must clear to fill this GPU on its own. llama's large
matmuls clear it comfortably; nothing else in the graph comes close, which is
why the fix pays.

---

## Three bugs this exposed

### 1. `build-driver.sh` told you to measure the wrong driver

Its closing line said:

    VK_DRIVER_FILES=$INSTALL/share/vulkan/icd.d/asahi_icd.aarch64.json

Because the build is configured `--prefix=/usr`, meson writes the *eventual*
install path into that manifest:

    "library_path": "/usr/lib64/libvulkan_asahi.so"

which is the distro driver. Following the script's own instructions loads stock
Honeykrisp, reports `Mesa 26.2.2`, and works — so every measurement taken
through it silently describes the unpatched driver and nothing fails.
`packaging/honeykrisp-got.spec.in` already rewrites this manifest for exactly
this reason; the build script did not. It now writes `asahi_icd.local.json`
next to meson's and points you at that.

### 2. `cstest` and `concurrency.sh` were unusable on any unfixed driver

`cstest`'s only clock was `vkCmdWriteTimestamp2`, and the bug that
`05187f07881` fixes makes it report **0.055 ms for 183.46 ms of work — a 3336x
under-report** on G13D. `data/measurement-hazards.md` records 240x on G13C; it
is an order of magnitude worse here.

`concurrency.sh` reads that column. Run against a stock driver it produced a
plausible-looking table of pure fiction. `cstest` now measures submit-to-idle
on the CPU as well and prints both columns: the wall column is always
trustworthy, and the gap between them measures the bug. On the patched driver
the two agree to 8%.

### 3. `cts-run.sh MESA=local` pointed at a directory nothing creates

It named `$HOME/Projects/mesa/install`, while `build-driver.sh` installs to
`$HOME/Projects/mesa-local/install`. It also named meson's manifest rather than
the corrected one, so had the path existed it would have run the entire suite
against the stock driver under a label saying "local". Both fixed, with the
checkout and output directories made relocatable at the same time.

---

## Conformance on G13D

The 15,307-test result in `STATE.md` is a G13C measurement, so it was re-run
here. VK-GL-CTS at `979a4f5a6` (`vulkan-cts-1.4.3.3-288`) — the same revision
`CONFORMANCE.md` pins, so the counts are directly comparable to G13C, not only
between arms. `./cts-run.sh` with `MESA=local` for the patched arms and
`MESA=system` for stock.

| suite | passed | stock fails | `nooverlap` fails | `0x80` fails |
|---|---|---|---|---|
| `dEQP-VK.memory_model.*` | 2218 | 30 | 30 | 30, identical set |
| `dEQP-VK.synchronization.op.single_queue.*` | 2353 | — | 0 | 0 |
| `dEQP-VK.compute.*` | 10736 | — | 0 | 0 |

**15,307 tests, no regression, and the same pass counts as G13C to the case.**

The failure sets were diffed by case name, not counted. The 30
`memory_model` failures are the same 30 in all three drivers — stock, patched
with the fix off, patched with it on — so none is caused by this change. They
are one family, three variants each of:

    dEQP-VK.memory_model.message_passing.ext.u32.coherent.
        {atomic_atomic,atomic_fence,fence_atomic,fence_fence}.
        {atomicrmw,atomicwrite}.{device,queuefamily}.payload_local.image.guard_local

— always `payload_local` with an **image** payload and a local guard. That is
consistent with the 30 pre-existing `message_passing` failures `STATE.md`
records on G13C, and it is an image-path problem independent of dispatch
barriers.

That `deqp-vk` actually loaded the patched binary was checked directly with
`VK_LOADER_DEBUG=driver` rather than inferred from the label, given bug 1
above: it resolves to `mesa-local/install/lib64/libvulkan_asahi.so`.

What this adds to the safety argument in `STATE.md`: that argument is about
Vulkan semantics — dispatches sharing a control stream are independent by
construction — and it does not depend on the part. But it rested on one GPU's
CTS run. It now rests on two, with different core counts and a different
die topology, and `memory_model` and `synchronization` are exactly the suites
that would catch a missing cross-dispatch flush.

---

## What this does not establish

* **Nothing here re-validates the game.** The game numbers remain G13C
  measurements and no part of this page bears on them.
* **The two-die hypothesis is not tested.** The full barrier costs more per
  dispatch here than on G13C (4.43 vs ~3.9 us) and that is *consistent* with a
  flush settling across UltraFusion, but a 13% gap on a different part at a
  different core count is not evidence. An experiment that pinned work to one
  die would be.
* **The llama figure is one model, one prompt, one context length.** Decode on
  a 16.4 GiB hybrid SSM model at 4096 context. Prompt processing was not
  characterised — the 28-token prompt in this harness is dominated by
  first-graph cost and measures nothing useful. A model with a different
  dispatch mix will not necessarily see 11%.
* **`~145 GB/s` is not a bandwidth ceiling claim.** It is what the model's
  16.40 GiB of weights divided by the token rate implies, and it is quoted
  below only to contradict a number, not to establish one.

## One number elsewhere in this repository is wrong

`data/what-is-left.md` states:

> Measured streaming ceiling on this GPU is ~68 GB/s in `tests/memtest.c`

llama exceeds that on this part. 16.40 GiB of weights read per token at
8.26 tok/s is **~145 GB/s** sustained, 2.1x the stated ceiling, on a part whose
specified memory bandwidth is 800 GB/s.

The cause is in `memchase.comp` mode 1: `data[(base + i*64u) & pc.mask]` with
`base = t*16u`. Across all threads and iterations that touches **one uint per
64 bytes** — four useful bytes out of every cache line the memory system moves.
It is a sparse gather, not a streaming test, and calling its result a streaming
ceiling overstates how memory-bound the remaining compute is. On this part it
is worse still: the 64 MiB working set fits inside the Ultra's system-level
cache, so it would not reach DRAM at all.

This matters because that figure is load-bearing in the argument that what
remains is memory-bound with no driver lever left. A corrected streaming test —
contiguous `vec4` loads, working set well beyond SLC — is the thing to write
before that argument is relied on again.

---

## Reproducing

    ./build-driver.sh
    export VK_DRIVER_FILES=$HOME/Projects/mesa-local/install/share/vulkan/icd.d/asahi_icd.local.json
    vulkaninfo --summary | grep driverInfo     # must say 26.3.0-devel, NOT 26.2.2

    cd tests
    glslangValidator -V cstest.comp -o cstest.spv && cc -O2 -o cstest cstest.c -lvulkan
    CSTEST_CASE=64,1,100000 ./cstest                        # default mask
    HK_PERFTEST=nooverlap CSTEST_CASE=64,1,100000 ./cstest  # the control
    ./concurrency.sh

For llama, a systemd drop-in is enough and leaves the system driver alone:

    # ~/.config/systemd/user/llama-server.service.d/99-hkgot.conf
    [Service]
    Environment=VK_DRIVER_FILES=/home/<user>/Projects/mesa-local/install/share/vulkan/icd.d/asahi_icd.local.json

Add `Environment=HK_PERFTEST=nooverlap` to that file to get the control arm
without changing anything else. `rm` the file to revert entirely.
