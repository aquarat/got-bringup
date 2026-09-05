# CDM barrier: measured per-bit cost and coherency role

Machine: M1 Max (G13C C0), 32 cores, Honeykrisp local build, 2026-09-04.

## Cost per bit

`tests/cstest.c`, `CSTEST_CASE=64,1,100000`: 64 dispatches of ONE workgroup
each, no data dependency between them, each doing a long serial loop. One
workgroup cannot fill the GPU, so this case measures exactly one thing --
whether consecutive dispatches are allowed to run at the same time.

| mask | ms | reading |
|---|---|---|
| `0x0` (no bits) | 2.892 | full overlap; the floor |
| bit 7 (`0x80`) | 2.891 | free |
| bit 17 (`0x20000`) | 2.891 | free |
| bit 18 (`0x40000`) | 2.891 | free |
| bit 19 (`0x80000`) | 2.894 | free |
| bit 20 (`0x100000`) | 3.470 | nearly free |
| bit 1 (`0x2`) | 4.229 | cheap |
| bit 2 (`0x4`) | 5.030 | cheap |
| bit 0 (`0x1`) | 7.443 | cheap |
| bit 4 (`0x10`) | 37.176 | serialises |
| bit 5 (`0x20`) | 37.172 | serialises |
| bit 6 (`0x40`) | 37.177 | serialises |
| bits 10-16 | 37.17-37.18 | serialise |
| bit 3 (`0x8`) | 48.498 | serialises, worst single bit |
| bit 8 (`0x100`) | 182.927 | very expensive |
| bit 9 (`0x200`) | 183.284 | very expensive |
| all 21 bits | 188.447 | the pre-change default |

Combinations are NOT additive: `0x1f` (bits 0-4) costs 37.18 ms, less than bit
3 alone at 48.50 ms. Whatever these bits select, it is not a set of independent
waits.

## Where the cost actually goes

Same masks, three shapes:

| shape | `0x0` | `0x80` | `0x1f` | all |
|---|---|---|---|---|
| 64 tiny dispatches, long serial work | 2.89 | 2.89 | 37.2 | 188.4 |
| 64 dispatches that each already fill the GPU | 43.1 | 39.6 | 45.7 | 183.5 |
| 64 empty dispatches | 0.042 | 0.038 | 0.064 | 0.251 |

Two separate costs, and they must not be confused:

* a **fixed per-barrier cost** -- visible in the empty case, ~0.3 us for
  `0x1f` and ~3.3 us for the full mask;
* a **loss of overlap** -- visible only in the first row, where `0x1f` is as
  serialised as the full mask.

The full mask pays both. `0x1f` removes most of the first and none of the
second. `0x80` removes both.

## Coherency

`tests/coherence.c`, 200-300 trials per mask: one dispatch writes a buffer, the
next reads it, no Vulkan barrier between them.

| mask | result |
|---|---|
| `0x88` (3+7) | PASS |
| `0xa0` (5+7) | PASS |
| `0xc0` (6+7) | PASS |
| `0x8f` (0,1,2,3,7) | PASS |
| `0x80` (7 alone) | FAIL |
| `0x08` (3 alone) | FAIL |
| `0x18` (3+4) | FAIL |
| `0x81`, `0x82`, `0x84` | FAIL |
| `0x1f` | FAIL |
| `0x0` | FAIL |

Coherency needs bit 7 AND one of bits 3/5/6 -- one bit from the free group and
one from the expensive group. Bit 7 alone is not sufficient and neither is bit
3 alone, so the pair is doing two halves of one job (plausibly "flush writes"
and "wait/invalidate", but that is inference, not measurement).

## Corrections to earlier claims

Two statements in earlier revisions of STATE.md were wrong, and both were
written from the game rather than from a measurement:

* "bit 7 is the data-coherency bit and the one that serialises" -- bit 7 is
  free (2.891 ms against a 2.892 ms floor) and does not serialise anything.
  The expensive bits are 3, 4, 5, 6 and 8-16.
* "bits 3 and 4 are descriptor state, required between any two dispatches;
  omitting them hangs the GPU" -- `0x80` omits both and the game runs for
  minutes without incident. The hang that produced this belief came from the
  earlier attempts that emitted no barrier block at all, or that failed to
  settle the deferred full flush.

`tests/statetest.c` was written to try to reproduce a descriptor-state hazard
locally -- 64 independent dispatches each with its own buffer, own push
constants, alternating pipelines. It passes at every mask down to `0x0`, so it
does not reproduce whatever the original hang was either.

---

# How many dispatches actually overlap

`tests/concurrency.sh` -- one workgroup and a long serial loop per dispatch, so
a single dispatch cannot fill the GPU and its duration is roughly constant.
Sweep the dispatch COUNT: while the hardware absorbs them in parallel the total
stays flat, and once it saturates the total grows. Minimum of 5 runs per point,
because the desktop compositor now shares this driver and the mean is noise.

| dispatches | ms | ms/dispatch |
|---|---|---|
| 1 | 5.386 | 5.3860 |
| 2 | 3.954 | 1.9770 |
| 4 | 4.033 | 1.0083 |
| 8 | 4.116 | 0.5145 |
| 16 | 4.293 | 0.2683 |
| 32 | 3.963 | 0.1238 |
| **64** | **4.181** | 0.0653 |
| 128 | 8.812 | 0.0688 |
| 256 | 10.978 | 0.0429 |

Flat at ~4 ms all the way to 64, then doubling at 128 and rising again at 256.

**The GPU runs about 64 independent single-workgroup dispatches at once.** 64
dispatches cost the same wall time as one.

This is the ceiling the weak barrier unlocked, and it says the dispatch-overlap
avenue is now essentially spent for this workload: the game's ~414 small
dispatches per frame are already free to use that width. Further gains have to
come from doing less work per dispatch, not from more of them in flight.
