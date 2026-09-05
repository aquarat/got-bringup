# Conformance suite for the tessellation optimisation work

Every change to the tessellation path is gated on this. Reuses the existing
harness (`~/Projects/cts-run.sh`, VK-GL-CTS 1.4.3 @ 979a4f5, unpatched).

## Case counts (measured on this machine)

    dEQP-VK.tessellation.*        1,100
    dEQP-VK.geometry.*              198
    dEQP-VK.draw.*               30,481
    dEQP-VK.transform_feedback.* 133,651

## Three tiers

### Tier 1 — per-change gate. Run on EVERY build. ~1,300 cases.

    dEQP-VK.tessellation.*
    dEQP-VK.geometry.*

Direct target plus the geometry path, which shares `src/poly/` (the tessellator
moved out of src/asahi into src/poly, shared with kosmickrisp) and the same heap
allocator and prefix-sum code. A tessellation change that breaks geometry shaders
is a real risk, not a theoretical one.

**Pass condition: zero regressions vs the Tier-1 baseline on the branch point.**
Not "zero failures" — record the baseline first, then diff. Absolute counts are
meaningless without it.

### Tier 2 — before merging anything. ~32,000 cases.

    Tier 1 + dEQP-VK.draw.*

`draw.*` covers indexed/indirect draws, which the tessellation path emits
(`agx_draw_indexed_indirect` from `hk_launch_tess`), and instancing, which
interacts with patch unrolling.

### Tier 3 — full regression before deploying. ~296,000 cases.

The existing baseline in `~/Projects/cts-baseline.txt` (4 groups, ~5.5 min,
152,578 pass / 0 fail). Do not redefine it — diff against it.

`transform_feedback.*` (133k) is deliberately NOT in tiers 1-2: it is large and
mostly orthogonal. Worth one run before any change ships, since XFEED captures
geometry-stage output.

## How to run

    cd ~/Projects
    OUTDIR=~/Projects/cts-results/<label> \
      MESA=local ./cts-run.sh <label> 'dEQP-VK.tessellation.*'

`cts-run.sh` hardcodes `MESA=local` -> `~/Projects/mesa/install`. For a worktree
build, set `VK_DRIVER_FILES` and `LD_LIBRARY_PATH` explicitly instead:

    export VK_DRIVER_FILES=~/Projects/<worktree>/install/share/vulkan/icd.d/asahi_icd.aarch64.json
    export LD_LIBRARY_PATH=~/Projects/<worktree>/install/lib64
    cd ~/Projects/VK-GL-CTS/build/external/vulkancts/modules/vulkan
    ./deqp-vk --deqp-surface-type=fbo --deqp-log-images=disable \
              --deqp-log-shader-sources=disable \
              --deqp-case='dEQP-VK.tessellation.*' \
              --deqp-log-filename=out.qpa

**ALWAYS pin `VK_DRIVER_FILES`.** Per cts-howto.md this has already caused two
false conclusions — unpinned, deqp-vk enumerates lavapipe, which supports
extensions Honeykrisp does not.

`--deqp-surface-type=headless` does not exist in this build. Use `fbo`.

## Correctness risks specific to this work

Tessellation is unusually easy to break *subtly*. Watch for:

- **Primitive ordering.** Vulkan requires primitive order to follow patch order
  for rasterization and blending. `poly_draw` relies on `counts[patch-1]` being
  an ordered exclusive prefix. Any change to the scan MUST preserve ordering --
  plain atomic allocation is NOT a valid substitute.
- **Barrier elision** is the highest-risk item. The existing barrier sets every
  bit "to be safe" because the semantics are un-RE'd. A missing flush produces
  intermittent corruption, not a clean failure -- exactly the signature of the
  7ppm CTS batch flake noted at `hk_queue.c:249`. Run Tier 1 **at least 3 times**
  on any barrier change; a single clean run proves nothing at that rate.
- **Ragged workgroups.** Patch-packing must handle patch sizes that do not
  divide the subgroup size, and the final partial workgroup.
- **Isolines and point mode** are handled by the software path only.

## Also required, beyond CTS

CTS does not cover performance or the actual game. For each change also record:

    HK_PERFTEST=notess           A/B upper bound
    ASAHI_MESA_DEBUG=perf        "%u dispatches, %u flushes" per control stream
    ~/Projects/got-bringup/instrument.sh    fps + power during real gameplay

A change that passes Tier 1 and does not move `flushes` or fps has not earned
its risk.

---

## IMPORTANT CORRECTION: CTS does not test the configuration the game runs in

Discovered while validating the barrier change. `dEQP-VK.tessellation.*` nominally
has 1,100 cases but **only 388 actually execute**; 712 report NotSupported.

The cause is `src/asahi/vulkan/00-hk-defaults.conf`:

    <device driver="hk">
        <engine engine_name_match="DXVK|vkd3d">
           <option name="hk_enable_vertex_pipeline_stores_atomics" value="true" />
        </engine>
    </device>

`vertexPipelineStoresAndAtomics` is **off by default** and switched on only for
engines matching `DXVK|vkd3d` (`hk_physical_device.c:267`). The rationale in the
file:

    Needed for FL11_1. While nominally conformant, CTS hits piles of timeouts
    (== flakes & fails & other fun times), especially on min-spec M1. Therefore
    the feature is hidden. I don't expect native apps to use this.

**Consequence: Ghost of Tsushima runs with the feature ON (it comes through
vkd3d-proton), while deqp-vk runs with it OFF.** The tessellation code path the
game exercises is therefore NOT the path CTS validates by default. That is a
real coverage gap for this work, not a technicality.

### Enabling it for CTS

Put this in `~/.drirc` (it applies only to deqp-vk, so the desktop and games are
unaffected):

    <driconf>
      <device driver="hk">
        <application name="deqp-vk" executable="deqp-vk">
          <option name="hk_enable_vertex_pipeline_stores_atomics" value="true" />
        </application>
      </device>
    </driconf>

Then re-count: `./deqp-vk --deqp-runmode=stdout-caselist` will not change, but the
executed (non-NotSupported) count should rise well above 388.

**Expect flakes.** The upstream comment says this configuration produces timeouts
on min-spec M1. This is an M1 Max, which may fare better, but treat new failures
as suspect-until-reproduced and always compare against a baseline run with the
SAME drirc in place.

### Revised Tier 1

Tier 1 should be run **twice**: once at defaults (matches upstream expectations,
comparable with the existing 296k baseline) and once with the option forced on
(matches what the game actually does). A change that regresses only the second
configuration still matters — that is the one that ships to the game.
