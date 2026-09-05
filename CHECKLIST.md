# Rendering-chain gaps: Ghost of Tsushima on Honeykrisp

Working checklist. Each item is something the stack is currently *faking, forcing
or working around* rather than implementing. Ordered by value.

Recorded 2026-09-03. M1 Max (G13C C0), Fedora Asahi Remix 44,
Proton Experimental 11.0-100, vkd3d-proton 3.1 (7ad4937e28ea1a0),
Mesa 26.1.8 system / 26.3.0-devel local.

Legend: [ ] not started  [~] worked around  [x] done

---

## 1. [x] `VK_KHR_shader_atomic_int64` — CLOSED: impossible on this hardware

**Status: DO NOT ATTEMPT A NATIVE IMPLEMENTATION.** M1 Max (Apple7 / AGX G13)
has no 64-bit atomic instructions at all. The `VKD3D_SHADER_MODEL=6_6` override
is not a stopgap — on G13 it is the only option.

**Apple's own Metal Feature Set Tables** (developer.apple.com, May 2026 rev):

    64-bit atomics                          Metal 3 & 4      Apple9
    7. GPU devices in the Apple8 family support 64-bit atomic minimum and maximum
       using ulong, on both buffers and textures, only on macOS. The full set of
       64-bit atomic operations is supported on all platforms starting with Apple9.

    Apple7 = M1 family = G13 (incl. G13C / M1 Max)  -> NONE
    Apple8 = M2        = G14                        -> umin/umax only, macOS only
    Apple9 = M3        = G15                        -> full set

Honeykrisp supports only G13 and G14 (`src/asahi/lib/agx_device.c:662-670`) —
precisely the two families without a usable set.

**Mesa already says so explicitly** — `src/asahi/compiler/agx_pack.c:261-282`:

    agx_pack_atomic_source(...)
       pack_assert_msg(I, index.size == AGX_SIZE_32, "no 64-bit atomics yet");
    agx_pack_atomic_dest(...)
       pack_assert_msg(I, index.size == AGX_SIZE_32, "no 64-bit atomics yet");

Corroborating ISA evidence:
- The atomic opcodes carry no width field at all: `agx_opcodes.py:150-162,311,317`
  (`op("atomic")` / `op("local_atomic")` take `[ATOMIC_OPC, SCOREBOARD]` only,
  unlike loads/stores which take `[FORMAT, MASK, ...]`).
- No 64-bit format exists in the ISA: `agx_compile.h:219-235` has I8/I16/I32/F16,
  no I64. 64-bit values are always register pairs (`agx_lower_64bit.c`).
- `cmpxchg` takes an `agx_vec2` — a pair of 32-bit registers, not a 64-bit datum
  (`agx_compile.c:951-1010`).

**The kosmickrisp lead disproved the hypothesis rather than supporting it.**
`src/kosmickrisp/vulkan/kk_physical_device.c:79` also sets
`.KHR_shader_atomic_int64 = false`, and its MSL backend emits only 32-bit-shaped
atomics. kk *does* family-gate other features, so it could have enabled this —
it does not, on any Apple GPU.

**Emulation is possible but NOT conformant.** Two 32-bit atomics cannot make one
64-bit atomic: carry propagation is observably non-atomic and the required
"old value" return cannot be produced. Mesa's reusable pass
(`nir_lower_atomics.c`) emits same-bit-size CAS and therefore *requires* a native
64-bit compare-exchange — Nouveau is its only user precisely because Maxwell+ has
one. AGX has neither. The only correct scheme is a hashed spinlock table, which
on AGX risks deadlock (neither Metal nor Vulkan guarantees independent forward
progress across subgroups), breaks for externally-shared memory, and would cost
an estimated 10-30x native. Intel ships `shaderSharedInt64Atomics = false` rather
than emulate. Scope if ever attempted: ~400-800 line NIR pass + ~150-300 lines of
driver plumbing + substantial CTS work; multi-week, high-risk, likely rejected
upstream for advertising a feature the hardware cannot honour.

**What this means practically:** the game runs correctly with SM 6.6 forced,
which is direct evidence its SM 6.6 usage is dynamic resources /
`ResourceDescriptorHeap`, not 64-bit atomics. The override is therefore sound
*for this title*. A title that genuinely issues 64-bit atomics will miscompile
silently, and nothing can be done about that on G13.

    [x] Determine whether AGX has 64-bit atomic instructions -- NO (Apple7)
    [ ] OPTIONAL, low priority: hwtestbed probe of the applegpu annotation that
        bits 45-46 with op=cmpxchg may encode umin64/umax64. Bit 45 is Mesa's
        G13X cross-cluster coherency bit and setting it on G13G "wedges the GPU"
        (agx_pack.c:821), so the annotation probably reflects M2, not M1. Even a
        positive result gives only umin64/umax64 - not enough for the Vulkan
        feature bit, though it IS the exact Nanite InterlockedMax pattern.
    [ ] If SM 6.6 titles become a priority beyond GoT, the tractable path is
        per-pattern emulation in vkd3d-proton, not a driver-level lie.

Sources: Apple Metal Feature Set Tables; dougallj/applegpu; philipturner's
ue5-nanite-macos AtomicsWorkaround (independently concludes M1 Max lacks them).

## 2. [~] The frame-rate collapse is MEMORY, not GPU settings — and not CPU

**Correction to an earlier working theory.** "Single-thread CPU bottleneck" was
the wrong read. The evidence points at memory pressure and texture streaming.

**The decisive data**, from the one real gameplay session (18:19-18:26, system
Mesa, Preset High). `[Render] Working set ... fps:` samples:

    18:20:17   WS  2911MB   fps 50.3
    18:21:17   WS  8487MB   fps 30.1
    18:22:17   WS 12264MB   fps 56.6
    18:23:17   WS 12778MB   fps 10.0   <-- COLLAPSE
    18:24:17   WS 12785MB   fps  6.4
    18:26:17   WS 12818MB   fps 10.6

The first settings change was logged at **18:23:47** — thirty seconds AFTER the
collapse. And dropping High -> Lowest (AO off, SSR off, shadows/textures low)
moved frame rate from **10.0 to 10.6**. That is why quality settings do nothing:
by the time you change them the machine is already thrashing.

**The mechanism.** The game was told `Video Budget: 15808MB`. That figure is
muvm's `--vram` default of 50% of system RAM — it is not real VRAM, it is shared
system memory. The game believes it and streams textures until the working set
reaches ~12.8 GB inside a 24.1 GB guest, on a 30 GB host. Per STATE.md finding 5,
guest page cache is host ANONYMOUS memory, so it is counted twice and is
reclaimable only by swapping; the host falls into direct reclaim against zram and
frame rate dies. Same failure mode as the previously-recorded 61-second stall,
just slower.

Note `Video Usage: 0MB` on every sample — vkd3d reports no video memory usage,
so the game has no feedback signal and never backs off.

**Action taken:** `~/.local/bin/steam` now passes `--vram=8192` to muvm, tunable
via `ASAHI_VRAM_MB` (`ASAHI_VRAM_MB=0` restores muvm's default). Backup at
`~/.local/bin/steam.bak-preVRAM` and `evidence/steam-wrapper.bak-preVRAM`.

    [ ] UNTESTED. Re-run the same ~7-minute session and plot working set vs fps.
        Success = working set plateaus well below 12 GB, fps does not collapse.
    [ ] If 8192 is too aggressive (texture pop-in, streaming stalls), try 10240.
        If not enough, try 6144.
    [ ] Only after this is settled is there any point profiling threads.

**On the earlier CPU reading.** Host CPU was 215% of 800% available, load average
1.84 — NOT multi-core saturation. That measurement stands; the inference drawn
from it does not. A memory-stalled process also fails to saturate cores.
Per-thread profiling (`perf record -g` / `top -H` on the FEX pid) is still the
way to settle CPU questions, but it should happen AFTER the memory problem is
fixed, or it will just measure stall time.

**Measurement hygiene:** `PROTON_LOG=1` is now OFF in `user_settings.py`. It
mattered less than feared for the gameplay run (~4,000 unwind lines over 449 s)
but is catastrophic when something is throwing: during the mesa-local crash run
Streamline threw **194 C++ exceptions/sec**, which `+seh,+unwind` turned into
~22,000 log lines/sec and 1.83 MB/s of synchronous writes from the game's main
thread. Never benchmark with it on.

**Other slow paths found in the logs, not yet acted on:**
- `Not all relevant pipeline stages are supported by EXT_dgc. Skipping.` —
  `ExecuteIndirect` loses its GPU-driven path, which GoT uses for foliage/culling.
  The driver *does* export `VK_EXT_device_generated_commands`; vkd3d rejects it.
- `Could not allocate an out of band queue for queue family 0.` — out-of-band
  work serialises onto the in-band queue.
- `Assuming device does not support tile based rendering.` — the game is told
  "not a tiler" on a TBDR GPU, so it will not use tiler-friendly pass ordering.
- UPLOAD heap is `DEVICE_LOCAL | HOST_COHERENT` (write-combined, uncached);
  every CPU memcpy into it is emulated store-by-store by FEX.
- `VKD3D_CONFIG=''` — no tuning applied at all, and GoT is absent from
  vkd3d's application-workaround table.
- 7,204 alignment rejects in one session (~3-7 per frame at 10 fps) on two
  texture-streaming threads, not a load-time burst. See item 4.

---

## 3. [~] Wine reports no PCI ID -> "Wine Adapter"

**Status:** worked around with `dxgi.customVendorId = 0000`.

Honeykrisp has no PCI identity (inside muvm the GPU is a `virtio_mmio` platform
device; there is no PCI bus), so Vulkan reports `vendorID = 0x10005`
(VK_VENDOR_ID_MESA) and no `VK_EXT_pci_bus_info`. Wine's `win32u` cannot build a
PCI hardware ID and registers `DriverDesc = "Wine Adapter"` /
`PCI\VEN_0000&DEV_0000&SUBSYS_00000000&REV_00`. Any game that cross-checks its
DXGI adapter against `EnumDisplayDevices` fails its driver pre-flight.

The workaround forces DXGI to report vendor 0 so both strings agree. It is
coupled to a Wine implementation detail and will break if Wine changes.

    [ ] Proper fix is upstream in WINE, not Mesa: win32u should fall back to
        the Vulkan deviceName when no PCI info exists. Mesa reporting
        VK_VENDOR_ID_MESA is spec-mandated and must not change.
    [ ] Alternative local-only fix: honeykrisp already wires up the
        `force_vk_vendor` driconf option (hk_physical_device.c:699,
        hk_drirc_gen.py). Untested — see README "Fix B".

Full detail: `README.md`.

---

## 4. [ ] D3D12 4 KiB small-resource alignment unsupported

**Status:** not addressed. Fires constantly but is not currently fatal.

    d3d12_resource_validate_texture_alignment:
      Invalid resource alignment 0x1000 (required 0x10000)      x1776 in one session

The game requests 4 KiB texture alignment (legal in D3D12 for small textures);
vkd3d enforces 64 KiB. Every such resource is rounded up 16x.

Not a per-frame cost, but it inflates memory — and per STATE.md finding 5,
memory pressure and zram reclaim are the real cost in this stack, not GPU
throughput. Worth quantifying before deciding it is benign.

    [ ] Measure actual memory overhead attributable to the rounding
    [ ] Determine whether AGX's 16 KiB page minimum makes 4 KiB impossible
        (cf. STATE.md finding 1: 3D sparse cannot serve D3D12 for this reason)

---

## 5. [~] Panel exposes only 3024x1964 -> virtual desktop required

**Status:** worked around with a Wine virtual desktop at 1920x1200.

`appledrm.show_notch=1` makes the panel advertise **only** 3024x1964 (six
refresh rates, no other resolution, `Custom modes: None`). KDE cannot offer
anything else — this is why display settings appear broken. Aspect 1.5397
appears in no game's mode table; GoT's tops out at 3024x1890 / aspect 1.60.

    [Software\\Wine\\Explorer]           "Desktop"="Default"
    [Software\\Wine\\Explorer\\Desktops] "Default"="1920x1200"

in `.../compatdata/2215430/pfx/user.reg`. Confirmed working — the game logs
`rect(0 1920 0 1200)`, `aspect = 1.60`.

    [x] DECIDED: appledrm.show_notch=1 STAYS. Do not propose reverting it.
        The Wine virtual desktop at 1920x1200 already makes the 3024x1964
        panel mode a non-issue for the game, and the notch is wanted on the
        desktop. This is a settled preference, not an open question.
    [ ] Quantify the virtual desktop's composition overhead (extra blit)

---

## 6. [~] RESOLVED: mesa-local cannot create a Vulkan instance inside pressure-vessel

**Status:** cause identified. Worked around by running system Mesa
(`ASAHI_LOCAL_MESA=0 steam`). The real fix is unfinished — see below.

**What happens:** with `mesa-local` pinned, DXVK never initialises:

    info:  DXVK: v3.0.2-58-g9cd6755b1d8416a
    info:  Vulkan: Found vkGetInstanceProcAddr in winevulkan.dll
    err:   DxvkInstance::createInstance: Failed to create Vulkan instance
    err:   Failed to initialize DXVK.

The game then throws a C++ exception (code 0x20474343, the GCC exception magic)
out of DXGI.DLL, unwinds through `sl.interposer.dll`, and dies. Its log stops
dead at `[DXGI] Initializing...` and the process becomes a zombie its parent
never reaps — which is why Steam sits there claiming the game is still running.
Evidence: `evidence/gamelog-mesalocal-dxgi-crash.log`.

**Isolation (four runs):**

| Run | Mesa | virtual desktop | MangoHud | Result |
|-----|------|-----------------|----------|--------|
| A | local | no | no | CRASH |
| B | system | yes | no | OK |
| C | system | yes | no | ran |
| D | local | yes | yes | CRASH |

`mesa-local` is the only variable present in both crashes and absent from both
successes. The extras differ between A and D, so neither explains both.

**Why it is NOT a broken build:** the same `libvulkan_asahi.so` works fine under
plain `vulkaninfo` inside the muvm guest, reporting
`Mesa 26.3.0-devel (git-ad47d86bfd)`, `Apple M1 Max (G13C C0)`. Its dependency
set is byte-identical to the system driver's and every dependency resolves:

    diff <(ldd .../mesa-local/install/lib64/libvulkan_asahi.so | awk '{print $1}' | sort) \
         <(ldd /usr/lib64/libvulkan_asahi.so                   | awk '{print $1}' | sort)
    -> IDENTICAL

**The actual problem — containerisation.** The game does not run in the guest
directly; it runs inside the **pressure-vessel** container (Steam Linux Runtime
4.0, app 4183110). pressure-vessel imports the *host* graphics stack into the
container by enumerating ICDs from the standard Vulkan search paths and mapping
each driver plus its host dependency chain into `/usr/lib/pressure-vessel/overrides`.

`$HOME/Projects/mesa-local/install/share/vulkan/icd.d/asahi_icd.aarch64.json`
is not in any standard search path, so it is never imported. `VK_DRIVER_FILES`
points the in-container loader at that JSON anyway; the JSON is readable (home is
bind-mounted) but the `.so` it names must then resolve its dependencies against
the *container* libraries rather than the imported host ones. Since
`VK_DRIVER_FILES` names ONLY the local ICD, a failure to load it leaves zero
drivers, and `vkCreateInstance` fails outright — exactly what DXVK reports.

    [ ] CONFIRM the above with VK_LOADER_DEBUG=error,warn (run in progress at
        time of writing - read the loader's own rejection reason, do not assume)
    [ ] Then fix properly. Candidates, untested, in rough order of preference:
        - Install/symlink the local ICD into a standard search path so
          steam-runtime-tools imports it like any other host driver, e.g.
          ~/.local/share/vulkan/icd.d/  (XDG_DATA_HOME is on the loader's path)
        - PRESSURE_VESSEL_FILESYSTEMS_RO=$HOME/Projects/mesa-local to
          expose the tree, if visibility rather than import is the issue
        - Verify which of the two actually matters before adopting either

**Consequence for "make Steam use our drivers by default":** it already is the
default - the wrapper pins mesa-local unless `ASAHI_LOCAL_MESA=0`. That default
is currently BROKEN for any game using Steam Linux Runtime 4.0. Do not "fix" it
by changing the default; fix the import path so the default works.

## Current working configuration

`~/.local/share/Steam/steamapps/common/Proton - Experimental/user_settings.py`
(delete the file to revert everything):

    DXVK_CONFIG        = dxgi.customVendorId = 0000; dxgi.customDeviceId = 0000
    VKD3D_SHADER_MODEL = 6_6
    MANGOHUD           = 1
    MANGOHUD_CONFIG    = fps,frametime,frame_timing,cpu_stats,cpu_load_change,
                         core_load,cpu_mhz,gpu_stats,gpu_load_change,vram,ram,
                         swap,resolution,vulkan_driver,engine_version,...
    PROTON_LOG         = 1

Launch with `ASAHI_LOCAL_MESA=0 steam` (system Mesa). REQUIRED until item 6
is fixed - the local build cannot create a Vulkan instance inside pressure-vessel.

Caveat: `user_settings.py` applies to EVERY Proton Experimental title. Once the
config is stable, move it to per-game Steam launch options for app 2215430.

MangoHud caveat: it is the host aarch64 layer, reached through the FEX Vulkan
thunk. `core_load` and `fps` should be reliable; `gpu_load` may read zero, as
MangoHud has no AGX backend and falls back to DRM fdinfo which Asahi may not
populate. Absence of a GPU load figure is not evidence of an idle GPU.

---

## 7. [!] BLOCKER: the local Mesa build never reaches games at all

**This supersedes the pressure-vessel theory in item 6, which was WRONG.**
Recorded after direct testing. This has implications for STATE.md section 1.

**What actually happens.** With `VK_DRIVER_FILES` pointed at the local build,
**Steam itself** fails to initialise Vulkan — before any game starts:

    CVulkanTopology: failed create vulkan instance: -9   (VK_ERROR_INCOMPATIBLE_DRIVER)
    Vulkan missing requested extension 'VK_KHR_surface'.
    Vulkan missing requested extension 'VK_KHR_xlib_surface'.
    BInit - Unable to initialize Vulkan!

Running an x86 `vulkaninfo` under FEX in the guest gives the precise reason:

    ERROR: $HOME/Projects/mesa-local/install/lib64/libvulkan_asahi.so:
           cannot open shared object file: No such file or directory
    ERROR: loader_icd_scan: Failed loading library associated with ICD JSON ...
    ERROR: vkCreateInstance: Found no drivers!

The file **is** present and readable from inside FEX (verified with `ls`), and it
is a valid aarch64 ELF. It still cannot be loaded.

**Why.** `/usr/share/fex-emu/ThunksDB.json` thunks the Vulkan **loader**, not
individual drivers, and only at a fixed set of paths:

    "Vulkan": { "Library": "libvulkan-guest.so",
                "Overlay": [ "@PREFIX_LIB@/libvulkan.so",
                             "@PREFIX_LIB@/libvulkan.so.1",
                             "@HOME@/.local/share/Steam/ubuntu12_32/steam-runtime/pinned_libs_64/libvulkan.so.1" ] }

A driver at an arbitrary path outside the system library locations does not
survive the guest/host thunk boundary. Corroborating oddity: the x86 rootfs's
`asahi_icd.x86_64.json` resolves to `/usr/lib64/libvulkan_asahi.so`, which from
inside FEX is an **aarch64** ELF — i.e. the thunk works by path, and only for
paths it already knows. An x86 `vulkaninfo` through that path reports
**`Mesa 26.0.3`**, which is neither the host's 26.1.8 nor the local 26.3.0-devel.

**Implication for STATE.md section 1 — please check this.** STATE.md states the
local build is "DEPLOYED" and that "Vulkan for Steam/games uses a locally-built
Mesa". The evidence here says games have **never** used it: every attempt to
pin it kills Vulkan outright, and every successful game run this session was on
system Mesa via `ASAHI_LOCAL_MESA=0`. The CTS and shader-db work was validated
host-side, natively, which is unaffected — but no game has run on the local
build.

**Consequence:** the request to point the desktop launchers at the local build
cannot be honoured yet. They remain pinned to `ASAHI_LOCAL_MESA=0`, because the
alternative is a Steam client that cannot start.

    [ ] Decide how to expose the local build to FEX. Options, none tested:
        - Install the local build over the system driver path. Simple and would
          certainly work, but violates the deliberate safety property in
          STATE.md (desktop keeps system Mesa so a bad build cannot kill the
          compositor). Would need a rollback plan.
        - Add the local prefix to ThunksDB.json's Vulkan Overlay list, or ship a
          FEX AppConfig entry. Untested; ThunksDB overlays name the *loader*,
          not ICDs, so this may not be the right lever at all.
        - Bind-mount the local .so over /usr/lib64/libvulkan_asahi.so inside the
          muvm guest only. Scoped to games, leaves the host desktop untouched.
          Probably the most promising - muvm controls the guest mount namespace.
        - Rebuild Mesa inside the FEX x86 rootfs. Heaviest, almost certainly
          not worth it.
    [ ] Whichever is chosen, verify with:
          muvm -- FEXBash -c 'vulkaninfo --summary' | grep driverInfo
        and require it to print the local build's version string.

### 7a. Guest-scoped bind-mount attempt — BLOCKED, muvm `-x` is broken

The plan was to bind-mount the local driver over the system path inside the
muvm guest's mount namespace only, leaving the host desktop on system Mesa.
`muvm --help` documents exactly this:

    -x, --execute-pre=COMMAND  Command to run inside the VM before guest server
                               starts, while still running as root.
                               Can be used for e.g. setting up additional mounts.

**It does not work in muvm-0.6.0-3.fc44.** Any use of `-x` aborts the launch:
neither the hook nor the main command runs, and there is no error message.

    # baseline, works:
    muvm -- /bin/sh -c 'echo ok > ~/mainran.txt'          -> file created
    # with -x, nothing runs (tried all three forms):
    muvm -x  "touch ~/x.txt"        -- /bin/sh -c '...'   -> neither file created
    muvm -x= "touch ~/x.txt"        -- /bin/sh -c '...'   -> neither file created
    muvm -x  "/usr/bin/touch ~/x.txt" -- /bin/sh -c '...' -> neither file created

Absolute paths were tried in case PATH is unset that early. Same result. This
looks like a genuine muvm bug worth reporting upstream — the flag is documented,
accepted by the argument parser, and then silently discards the whole launch.

    [ ] Report upstream / check muvm git for a fix or correct syntax
    [ ] If -x is fixed, the one-liner is:
          muvm -x "mount --bind $HOME/Projects/mesa-local/install/lib64/libvulkan_asahi.so \
                                /usr/lib64/libvulkan_asahi.so" -- ...
        and it needs no change to the ICD JSON, because
        /usr/share/vulkan/icd.d/asahi_icd.aarch64.json already points at that path.
    [ ] Fallback if muvm stays broken: a wrapper that enters a private mount
        namespace (unshare -m) before exec'ing muvm - untested, and it is not
        obvious the guest inherits the host's mount namespace at all.

**Unresolved contradiction, worth understanding before the next attempt.**
Under FEX (x86 view), the system ICD loads and reports **`Mesa 26.0.3`** — which
is neither the host's 26.1.8 nor the local 26.3.0-devel. Yet `file` on
`/usr/lib64/libvulkan_asahi.so` from inside FEXBash reports an **aarch64** ELF.
Those two facts cannot both be simple. Either FEX's rootfs overlay is serving a
different (x86, Mesa 26.0.3) driver to the loader than `file`/`ls` resolve, or
the Vulkan thunk is substituting a driver from elsewhere. **Until this is
understood, any further attempt to inject a custom driver is guesswork** — the
next step is to find out which library the x86 loader actually maps, e.g. by
inspecting /proc/<pid>/maps of a running x86 Vulkan process inside the guest.

This also means the "which Mesa is a game actually using?" question is currently
UNANSWERED even for the working system-Mesa configuration.

### 7b. CORRECTION + the actual mechanism (settled by /proc/<pid>/maps)

The contradiction in 7a is resolved, and part of item 7 was wrong.

**What the game actually loads** (from the maps of the live game process):

    /run/pressure-vessel/interpreter-root/var/pressure-vessel/gfx/main/usr/lib64/libvulkan_asahi.so
    /run/pressure-vessel/interpreter-root/var/pressure-vessel/gfx/main/usr/lib64/libvulkan.so.1.4.341

`libvulkan.so.1.4.341` is the HOST loader (host has vulkan-loader-1.4.341.0).
So pressure-vessel **does** import the host graphics stack into
`/var/pressure-vessel/gfx/main/`, and the full chain is:

    x86 game -> FEX Vulkan thunk (libvulkan-guest.so, per ThunksDB)
             -> host aarch64 loader, imported by pressure-vessel
             -> host aarch64 libvulkan_asahi.so, imported by pressure-vessel

**CORRECTION to item 7:** the worry that games might not be using host Mesa at
all was WRONG — they are, via this import. The `Mesa 26.0.3` reading in 7a came
from a bare `FEXBash -c vulkaninfo` OUTSIDE pressure-vessel, which is a different
code path from the game and is not what the game uses. Do not read 7a's
"Mesa 26.0.3" as evidence about game behaviour.

**What remains true from item 7:** the *local* build has still never run a game.
Every attempt to pin it fails.

**The remaining hypothesis, now much better founded and cheap to test.**
`steam-runtime-system-info` honours `VK_DRIVER_FILES` and enumerates the local
ICD with `issues: None` — so pressure-vessel WOULD import it. The likely blocker
is simply that `$HOME/Projects/mesa-local` is not exposed to the
container's mount namespace, so the import source path is unreachable.

`PRESSURE_VESSEL_FILESYSTEMS_RO` has already been added to the wrapper's FORWARD
list, so the test is one launch:

    PRESSURE_VESSEL_FILESYSTEMS_RO=$HOME/Projects/mesa-local steam -applaunch 2215430
    # (with ASAHI_LOCAL_MESA unset, so the wrapper pins the local ICD)

Then confirm from inside the guest:

    grep -oE '/[^ ]*libvulkan_asahi[^ ]*' /proc/<game pid>/maps

Success = the path resolves to the local build, not gfx/main's imported copy.

**Note:** Steam's own client Vulkan init fails under local Mesa
(`BInit - Unable to initialize Vulkan!`) because the Steam client runs OUTSIDE
pressure-vessel and gets no such import. That appears to be NON-FATAL — Steam
still started and still launched the game in that run. Do not treat Steam's
Vulkan complaint as the blocker; the game is a separate process with a separate
driver path.

### 2a. The VRAM cap FAILED. Measured result, and what replaced it.

`--vram=8192` was applied and confirmed reaching the game
(`Dedicated Video Memory: 8160 MB`, down from 15808). It did **not** bound the
working set:

    WS   912MB  budget 8192MB  demoted 0MB  fps  0.0
    WS  6914MB  budget 8192MB  demoted 0MB  fps 65.2
    WS  8810MB  budget 8192MB  demoted 0MB  fps 52.3
    WS 12703MB  budget 8192MB  demoted 0MB  fps 43.0   <-- 12.7 GB on an 8 GB budget

    budget 15808MB -> working set 12.8 GB
    budget  8192MB -> working set 12.7 GB      i.e. no effect

Host at that moment: **28.0 of 31.6 GB used, 2.3 GB free, 8.9 GB in zram**,
load average 4.08. User-observed frame rate ~6 Hz.

**Two things this proves:**

1. The game does NOT size its texture pool from the video budget. It reports
   `Total RAM = 24.1 GB` (muvm's default `--mem`, 80% of host) and almost
   certainly sizes against that instead.
2. `Video Usage: 0MB` and `Demoted: 0MB` on *every* sample. vkd3d-proton reports
   zero video-memory usage through `QueryVideoMemoryInfo`, so the game's
   streaming system never sees pressure and never evicts. The budget is inert
   because nothing is ever measured against it. **This is a vkd3d-proton gap
   worth investigating separately** — with working usage reporting, the VRAM cap
   would probably have worked as intended.

**Replacement action:** the wrapper now also passes `--mem=16384`
(`ASAHI_MEM_MB`, `=0` for muvm's default). Rationale is structural rather than
behavioural: whatever the game *wants*, a 16 GB guest cannot drive the host past
~16 GB + overhead, leaving ~15 GB of the 31.6 GB host free. That removes the
host-side reclaim regardless of whether the game cooperates.

    [ ] UNTESTED. Watch for: working set plateau below ~13 GB, host `free -m`
        staying out of zram, and fps not collapsing.
    [ ] RISK: if the game genuinely needs >16 GB it will OOM or thrash inside the
        guest instead. If that happens try ASAHI_MEM_MB=20480.
    [ ] Independent of muvm sizing: lower in-game Texture Quality. That bounds
        the pool at source and is the one lever known to affect it directly.
        Set it BEFORE loading a save - changing it after the collapse does
        nothing (measured: High -> Lowest moved 10.0 -> 10.6 fps).

### 2b. The memory explanation is INCOMPLETE. Measured, 19:41.

The `--vram=8192` run was watched to the collapse. What happened refutes the
simple "memory pressure" story in item 2:

    WS 12703MB  demoted 0MB  fps 43.0
    WS 12707MB  demoted 0MB  fps  6.2     <-- working set FLAT, fps collapsed anyway
    WS 12707MB  demoted 0MB  fps  6.2     <-- and it never recovers

At the 6.2 fps sample, the HOST was healthy:

    free -m      : 21092 used, 8776 free, 10523 available  (of 31616)
    PSI memory   : some avg10=0.00  full avg10=0.00
    PSI io       : some avg10=0.00
    PSI cpu      : some avg10=0.00
    swap traffic : 0.0 MB/s in, 0.0 MB/s out
    VM cpu       : 134% of 800% available
    fans         : quiet (user observation)
    MangoHud     : "very low CPU usage" (user observation)

So at 6 fps: no memory pressure, no swap I/O, no CPU pressure, no IO pressure,
low CPU, quiet fans. **Nothing is working hard and nothing is stalling that the
host can see.** The collapse also persisted after host memory recovered — it is
not a transient reclaim event.

**What this rules out:** host memory pressure as the *sustained* cause, host
swap thrash, host CPU saturation. Memory growth still CORRELATES with the onset
(collapse happens as WS approaches ~12.7 GB in both runs) but does not explain
why frame rate stays down once pressure clears.

**Leading hypotheses, none tested:**
1. **Guest-internal reclaim.** The guest VM has its own memory management and
   muvm balloons pages back to the host. Guest-side pressure would NOT appear in
   host PSI. This is the most likely candidate and the cheapest to test.
2. **The game's streaming system stuck in a retry loop** after the memory event,
   never recovering. Consistent with `Demoted: 0MB` — it never evicts, so if it
   is over-committed it may spin.
3. **GPU-bound.** Cannot currently be ruled out: quiet fans are NOT proof on an
   M1 Max, which is efficient at moderate load.

**MEASUREMENT GAP — fix this first.** We cannot currently see:
- Guest PSI. Read `/proc/pressure/{memory,io,cpu}` INSIDE the guest, not on the
  host. Host PSI was misleading here.
- GPU utilisation. Asahi exposes no `drm-engine-*` in `/proc/<pid>/fdinfo`
  (checked, nothing found) and no gpu-top tool is installed. MangoHud's
  `gpu_load` has no AGX backend. **We have been flying blind on GPU load for
  this entire investigation.**

    [ ] Next run: sample guest /proc/pressure/* and guest free -m every 10s
        alongside the fps log. That distinguishes hypothesis 1 from 3 directly.
    [ ] Find any way to read AGX GPU busy time. Options: asahi debugfs, a
        Vulkan timestamp-query harness, or ASAHI_MESA_DEBUG=perf,stats.
    [ ] Do NOT ship another memory fix until one of the above says which it is.
        Two memory-shaped fixes have now been tried; the first provably did
        nothing and the second is unvalidated.

**Status of the two applied fixes:**
- `--vram=8192` — reaches the game, provably does NOT bound the working set.
  Harmless but useless on current evidence. Consider reverting to reduce noise.
- `--mem=16384` — applied but NEVER EXERCISED (added after this run started).
  Completely untested.

### 2c. RETRACTION: there is no "collapse". Gameplay is simply ~6 fps.

**Items 2, 2a and 2b are built on a false premise and should be read only as a
record of what was ruled out.** User clarification: *"The game doesn't collapse,
it runs, it just runs slowly, at a low frame rate of 6 Hz."*

Re-read the same timeline against the game's actual phases — launcher (no GPU),
intro video (no GPU), menu (some GPU), gameplay (full GPU):

    WS  2911MB  fps 50.3    launcher / intro
    WS  8487MB  fps 30.1    loading
    WS 12264MB  fps 56.6    menu
    WS 12778MB  fps 10.0    <-- GAMEPLAY BEGINS
    WS 12707MB  fps  6.2    gameplay
    WS  3511MB  fps 44.7    (next run: menu only, user quit before gameplay)

The "collapse from 56 to 10" is a phase transition, not a degradation. The high
frame rates were menus and video. **Gameplay has never run faster than ~6 fps.**

**What this invalidates:**
- The memory-pressure story. Working-set growth to ~12.7 GB is the world
  loading, and it *coincides* with entering gameplay. Correlation, not cause.
- "It never recovers" — there was nothing to recover from.
- `--vram=8192`: harmless, but was solving a non-problem.
- `--mem=16384`: still worth keeping. It demonstrably stopped the host being
  driven into zram (host_psi_mem 0.00, swap 0.0 MB/s throughout the last run,
  versus 28/31.6 GB used and 8.9 GB swapped before). Good hygiene, not the fix.

**What it points at instead.** A steady 6 fps in gameplay with menus at 50+ is a
straightforward "the renderer is ~10x too slow for this workload" problem, not a
stall that develops. Candidates, in the order I would test them:

1. **Tessellation.** GoT is dense foliage/grass. AGX has no hardware tessellator;
   honeykrisp emulates it with extra compute dispatches, and STATE.md already
   carries an open thread about fusing the tess COUNT pass (5 dispatches -> 4).
   **`HK_PERFTEST=notess` gives a hard upper bound in one run.** Cheapest, most
   informative test available and it uses tooling that already exists.
2. **GPU-bound generally.** Settle with the power reading during *gameplay*
   (~50 W = GPU busy, ~20 W = not). Every power sample so far was menu or
   loading, so this is still unmeasured.
3. **A driver slow path.** `ASAHI_MESA_DEBUG=perf,stats` reports which one.
4. Submission-side cost. 22 MB of `VKD3D_QUEUE_PROFILE` trace already captured
   and not yet analysed.

**Note on "settings don't change fps":** if this were plain shading load,
quality settings would move it. That they do not is evidence the bottleneck is
something the settings do not touch — which fits (1) or (3) better than (2).

    [ ] A/B: one gameplay run normal, one with HK_PERFTEST=notess. Same save,
        same spot. This single comparison is worth more than everything above.
    [ ] Capture power DURING gameplay, not menus.
    [ ] ASAHI_MESA_DEBUG=perf,stats on a gameplay run.

---

## 8. [!] THE ACTUAL BOTTLENECK: ~966 dispatches/frame, each with a full cache flush

Measured with `ASAHI_MESA_DEBUG=perf,stats` during real gameplay on the deployed
build, 324 s of log at ~6 fps (~1945 frames):

    CDM submissions      94,146       API calls  1,281,649
    dispatches        1,879,635       flushes    1,888,055     merged  301,276
    control streams     197,639       submits       39,689

Normalised per frame:

    compute dispatches / frame   966
    cache flushes      / frame   971      <-- 1.004 flushes PER DISPATCH
    CDM streams        / frame    48.4
    control streams    / frame   101.6    <-- 101.6 ioctls/frame = ~610/s

**Every dispatch still drains the pipeline and invalidates all caches.**
`agx_cdm_barrier` (`libagx_dgc.h:372-413`) sets every unknown bit plus
`usc_cache_inval`, because the semantics are un-RE'd. 966 of those per frame.

If a drain costs even ~100 us, 966 of them is ~97 ms/frame == ~10 fps. That is
the right order of magnitude for what we observe. This is the leading candidate
for the entire 10x deficit.

**Why our barrier change did not help.** It only skipped the flush after the
LAST tessellation dispatch when no GS follows. This game barely tessellates
(proven by `HK_PERFTEST=notess`), so the change had almost nothing to act on.
The measured -7.5% was on a tessellation CTS group, which is not representative
of this workload. The plumbing is right; the scope was wrong.

**The 101.6 control streams per frame is a second, independent cost.**
`max_commands_per_submit()` returns 1 unless `HK_PERFTEST=batch`
(`hk_queue.c:253`), so that is ~610 ioctls/second, each thunked through FEX,
where syscalls are unusually expensive. Batching is disabled upstream over a
7ppm CTS flake (`hk_queue.c:249-252`).

### Two cheap bounding experiments, in order

1. **`HK_PERFTEST=batch`** — already exists, no build needed. Batches up to 64
   control streams per ioctl. Bounds the submit/ioctl cost directly. If fps
   moves materially, the ioctl path matters and the 7ppm flake is worth
   revisiting.

2. **A debug flag that skips ALL CDM flushes.** The plumbing already exists
   from the barrier work (`hk_dispatch_with_usc_launch` takes `enum agx_barrier`,
   `HK_PERFTEST=forcebarrier` restores the old behaviour). Adding the inverse —
   skip every flush — is a few lines. It will very likely render incorrectly,
   but that does not matter: it puts a HARD UPPER BOUND on what removing the
   barrier tax could ever be worth. If fps barely moves, the whole barrier
   theory dies cheaply and we look elsewhere. If it jumps to 30+, then the real
   work is item 2b (batch dispatches by stage) plus reverse-engineering which
   barrier bits are actually needed.

**Do experiment 2 before writing any more optimisation code.** Today's lesson,
twice over: measure the bound before building the fix.

---

## RESULT: both bounding experiments came back negative

Run by the user on the deployed local driver, same save, same spot.

| Run | fps |
|---|---|
| baseline | ~6 |
| `HK_PERFTEST=batch` (64 control streams per ioctl) | ~6, no change |
| `HK_PERFTEST=noflush` (skip every CDM cache flush) | ~6, no change |

Two hypotheses retired for the price of one build:

- **The per-dispatch barrier tax is not the bottleneck.** 971 cache flushes per
  frame sounded damning; removing all of them buys nothing. Item 2b — batching
  dispatches by stage to eliminate those flushes — would therefore have been
  wasted work, which is exactly what the bound was for.
- **The ioctl rate is not the bottleneck either.** 610 submits/second through
  FEX sounded expensive; collapsing them 64:1 buys nothing.

Both were measured, not argued. That is now four hypotheses disproven by
testing (pressure-vessel ICD visibility, glibc mismatch, tessellation,
subgroup atomic aggregation) plus these two.

### What this leaves

The GPU draws 41.9 W (idle 17 W) to produce 6 fps, all PSI ~0, 2 of 8 cores
busy. So work IS being done, on the GPU, and neither the barriers around that
work nor the cost of submitting it explains the frame time. That points at the
work itself, or at bubbles between the pieces of it — and **we could not tell
those apart**, because nothing on Asahi reports GPU utilisation. Total System
Power is a whole-SoC proxy that cannot separate CPU from GPU, never mind
vertex from fragment from compute.

## The instrument that closes that gap: `HK_GPUTIME`

The firmware can timestamp its own work. `drm_asahi_cmd_render` carries
`ts_vtx` and `ts_frag` start/end pairs; `drm_asahi_cmd_compute` carries `ts`.
The gallium driver uses these for `AGX_DBG_STATS`. Honeykrisp wired up only
`ts_frag.end`, purely to service `vkCmdWriteTimestamp` — **every other slot was
unused**, which is why nobody has ever measured where GPU time goes on this
driver.

`src/asahi/vulkan/hk_gputime.c` claims the unused slots. `HK_GPUTIME=<seconds>`
reports per interval:

    [hk gputime] 5.00 s wall
    [hk gputime]   vtx      ... ms  ...% of wall   ... cmds   ... us each
    [hk gputime]   frag     ...
    [hk gputime]   comp     ...
    [hk gputime]   GPU busy (union) ... ms = ...% of wall

**The union is the number that decides the next month of work.** Vertex,
fragment and compute overlap on a tiler by design, so their individual sums can
exceed wall time and mean nothing alone. The union cannot:

- **Union near 100%** → the GPU is genuinely saturated. The frame costs what it
  costs, and only *less work* helps: shader optimisation, the D24→D32 depth
  emulation (1.5× bandwidth), lower settings. Optimising submission is pointless.
- **Union well under** → the GPU is idle inside the frame. The work is not the
  problem; the gaps between it are. Chase dependency stalls, WSI/present
  pacing, and the `Assuming device does not support tile based rendering`
  finding. Shader optimisation would be pointless here — the exact opposite
  conclusion.

These two readings demand opposite work, which is why this had to be measured
before anything else gets built. Same discipline that just retired two
hypotheses in one run.

Validated against vkcube before deployment: 245 render passes and 0.5% busy
when vsynced, 16279 and 27% in immediate mode, ~17 us vertex and ~17 us
fragment per pass in both — the counts and the busy fraction both scale with
load, and the per-pass costs stay put, as they must.

### How to run it

    HK_GPUTIME=5 ASAHI_LOCAL_MESA=0 steam

Get to steady-state gameplay, let it sit a minute, then:

    ./gputime.sh -s      # whole-run summary
    ./gputime.sh -f      # follow live

---

# CRITICAL: the game has never used our driver

`HK_GPUTIME=5` produced no output. The variable was forwarded correctly — it is
visible on the live muvm command line:

    /usr/bin/muvm -e MESA_SHADER_CACHE_MAX_SIZE=12G -e HK_GPUTIME=5 ...

The driver simply is not ours. The Proton log says:

    info:  Found device: Apple M1 Max (G13C C0) (Honeykrisp 26.0.3)
    info:  Found device: llvmpipe (LLVM 22.1.0, 256 bits) (llvmpipe 26.0.3)

Neither 26.0.3 is a host version. The host has distro Mesa **26.1.8** and our
build **26.3.0-devel** (`driverVersion 26.2.99`). Both entries being 26.0.3
means the whole imported graphics stack comes from one foreign Mesa tree.

It does. Extracting `/usr/share/fex-emu/RootFS/default.erofs`:

    rootfs/usr/lib64/libvulkan_asahi.so   ELF 64-bit LSB shared object, x86-64
    version string:                       26.0.3

**The game runs an x86-64 Honeykrisp 26.0.3 out of the FEX rootfs, emulated.**
pressure-vessel's graphics provider is the guest's `/`, which under FEXBash is
that rootfs — so it imported the x86-64 Mesa, not the host's aarch64 one.
`/var/pressure-vessel/gfx/main/usr/lib64/libvulkan_asahi.so`, which CHECKLIST
item 6 recorded as "the imported copy of the HOST driver", is nothing of the
kind.

## What this invalidates

- **`deploy-system-driver.sh` never delivered anything to the game.** It does
  what it says — `/usr/lib64/libvulkan_asahi.so` really is our build, and the
  desktop really does use it — but the game never looks there. The verification
  step only ever confirmed the host path, never what the game loaded.
- **`HK_PERFTEST=noflush` was a no-op.** The flag does not exist in 26.0.3, so
  that result is void, not negative.
- **`notess` and `batch` do exist upstream in 26.0.3**, so those two results are
  probably real — but they measured stock Mesa, not our branch.
- **Every optimisation on `local-deploy` has never run in this game.** The three
  tessellation commits, the compiler fixes, the lighter CDM barrier: none of it
  was ever exercised. The 6 fps is a stock-26.0.3 number.

The `[hk] HK_PERFTEST active` banner added this session is what exposed this,
one run after being added. A flag that fails to arrive looks exactly like a flag
that arrived and did nothing — and for weeks, it did.

## Why the thunk is not catching it

FEX is meant to prevent exactly this. `/usr/share/fex-emu/ThunksDB.json`:

    Vulkan  Library: libvulkan-guest.so
            Overlay: @PREFIX_LIB@/libvulkan.so
                     @PREFIX_LIB@/libvulkan.so.1
                     @HOME@/.local/share/Steam/ubuntu12_32/steam-runtime/pinned_libs_64/libvulkan.so.1

Both thunk halves are installed (`GuestThunks/libvulkan-guest.so`,
`lib64/fex-emu/HostThunks/libvulkan-host.so`). But inside the pressure-vessel
container the game loads

    /usr/lib/pressure-vessel/overrides/lib/x86_64-linux-gnu/libvulkan.so.1
      -> /var/pressure-vessel/gfx/main/usr/lib64/libvulkan.so.1.4.341

and **neither path is in the overlay list**. The second entry is the *old scout*
runtime path; Steam Linux Runtime 4.0 does not use it. So no substitution
happens, the guest keeps its own x86-64 loader, and the entire Vulkan
userspace — driver, compiler, command-buffer building — runs under emulation.

FEX exposes `FEX_THUNKCONFIG` to point at an alternative ThunksDB, which is the
obvious lever.

---

## Option 1 (FEX Vulkan thunk) — investigated in depth, currently blocked

The goal: make FEX substitute `libvulkan.so.1` with its thunk so the game's
Vulkan runs as **native aarch64** against our driver, instead of an emulated
x86-64 Mesa. That would deliver our optimisations *and* remove emulation from
the whole Vulkan call path. It does not work yet. What was established:

### The architecture is not what the earlier notes assumed

`muvm` boots an **aarch64** guest; FEX emulates x86-64 *inside* it. So two Mesa
stacks exist in the guest:

| Client | Mesa it gets | Source |
|---|---|---|
| native aarch64 (guest) | **our build** | host `/usr/lib64`, shared into the guest |
| FEX x86-64 (the game) | Honeykrisp 26.0.3 | the FEX rootfs erofs |

Verified directly — native aarch64 `vulkaninfo` inside muvm:

    [hk gputime] firmware GPU timing active, reporting every 3.00 s
    driverInfo = Mesa 26.3.0-devel (git-46b2d15388)

**Our driver works correctly under muvm's virtio-gpu path, and the gputime
instrument initialises there.** The only gap is that FEX-emulated clients use
the rootfs copy. That is a delivery problem, not a driver problem.

### Why the thunk never fires

Four separate obstacles, found in order. The first three are fixed; the fourth
is not.

1. **muvm replaces `/usr/share/fex-emu` in the guest.** The guest sees exactly
   one file there — `Config.json`, containing
   `{"Config":{"RootFS":"/run/fex-emu/rootfs"}}`. `ThunksDB.json` and the whole
   `GuestThunks/` directory are ENOENT inside the guest, so FEX has no thunk
   database and no guest thunk libraries to load. Fixed by copying both to
   `got-bringup/fex/` (shared filesystem, visible in the guest) and pointing
   `FEX_THUNKCONFIG` / `FEX_THUNKGUESTLIBS` at them. Confirmed by strace: FEX
   then opens our `ThunksDB.json` and `GuestThunks/libVDSO-guest.so`.

2. **The config path was wrong.** strace shows FEX reads
   `~/.fex-emu/Config.json` and `~/.fex-emu/AppConfig/<app>.json` — *not*
   `~/.config/fex-emu/`, despite FEX creating that directory.

3. **The overlay path did not match.** The stock DB overlays
   `@PREFIX_LIB@/libvulkan.so.1`, but `LD_DEBUG=libs` shows the guest loader
   resolving **`/lib64/libvulkan.so.1`**. FEX matches overlay entries as
   literal strings, so the stock entry can never match on this Fedora rootfs.
   Added `/lib64/...` and the pressure-vessel container paths to our DB.

4. **The `ThunksDB` enable is silently ignored.** With all of the above fixed,
   FEX still never opens `libvulkan-guest.so` or any host thunk. Eight config
   shapes were tested in-guest (`int 1`, `true`, `"1"`, top-level, nested under
   `Config`, both, lowercase key, `libvulkan` key), each against a freshly
   written config, all returning Mesa 26.0.3. `FEX_SILENTLOG=0` produces no
   thunk diagnostic — not the "Requested thunking via guest library that does
   not exist" or "Failed to initialize thunk library" messages that exist in
   the binary. The subsystem initialises (it loads our DB and the unconditional
   VDSO thunk) but the Vulkan entry never activates.

`fex-emu-2604-1.fc44`. No thunk documentation ships in the package, and nothing
in the Fedora gaming stack enables thunks for anything — the only AppConfig
that mentions them (`steamwebhelper.json`) *disables* GL. It is likely that
overlay thunking is inert in this build; confirming that needs the FEX source.

### Where that leaves us

Option 2 is now the tractable path: **cross-build Honeykrisp for x86-64 and put
it in the rootfs.** The FEX rootfs itself supplies the x86-64 sysroot (it has
libdrm, wayland, xcb), so a Vulkan-only Mesa cross-build against it is
plausible without hunting x86-64 devel packages. Delivery would be a writable
copy of the extracted rootfs with our `libvulkan_asahi.so` swapped in, selected
via `FEX_ROOTFS`. This delivers the GPU-side optimisations and the gputime
instrument to the game, but leaves the Vulkan driver running emulated.

Artifacts kept in `got-bringup/fex/`: the extended `ThunksDB.json` and a copy of
`GuestThunks/`, both still useful if the enable mechanism is ever cracked.

---

# RESOLVED: the Vulkan thunk works. It was never a config-shape problem.

Cloned `FEX-Emu/FEX` at tag `FEX-2604` to match `fex-emu-2604-1.fc44`. The answer
is in `Source/Tools/LinuxEmulation/LinuxSyscalls/FileManagement.cpp`:

```cpp
void FileManager::LoadThunkDatabase(..., bool Global) {
  auto ThunkDBPath = FEXCore::Config::GetConfigDirectory(Global) + "ThunksDB.json";
```

**The thunk database is loaded from the config directory and nowhere else.**
`FEX_THUNKCONFIG` is only appended to `ConfigPaths`, which supplies the
*enable map* — never the database. So:

- global config dir = `/usr/share/fex-emu/` → **muvm blanks this inside the
  guest**, leaving only its own `Config.json`. ENOENT.
- user config dir = `~/.fex-emu/` → did not exist.

The DB was therefore **empty**, and the enable loop:

```cpp
auto DBObject = ThunkDB.find(LibraryName);
if (DBObject != ThunkDB.end()) { DBObject->second.Enabled = LibraryEnabled; }
```

found nothing named `Vulkan` to enable. Every one of the eight config shapes was
being parsed correctly and then silently discarded against an empty map — which
is exactly why there was no diagnostic. No vendoring or patching required.

**Also: my `/lib64` theory was wrong.** `@PREFIX_LIB@` is expanded over four
prefixes (`/usr/lib64`, `/usr/local/lib64`, `/lib64`, and
`/usr/lib/pressure-vessel/overrides/lib64`), so the stock entry always covered
`/lib64/libvulkan.so.1`. The literal paths I added were never needed.

## The fix

    ~/.fex-emu/ThunksDB.json     copy of the system DB (+ container paths, below)
    ~/.fex-emu/Config.json       {"ThunksDB":{"Vulkan":1}}
    FEX_THUNKGUESTLIBS=.../got-bringup/fex/GuestThunks

The third is needed for the same reason as the first: muvm blanks
`/usr/share/fex-emu`, so FEX cannot find its own `GuestThunks` either. Without
it FEX aborts with "Requested thunking via guest library that does not exist".

The steam wrapper now sets `FEX_THUNKGUESTLIBS` automatically and forwards it
into muvm. Kill switch: `ASAHI_VULKAN_THUNK=0`.

## Verified working

x86-64 `vkcube` under FEXBash, 1500 frames, exit 0 — using the **native aarch64
driver**, our build:

    Selected GPU 0: Apple M1 Max (G13C C0), type: IntegratedGpu
    [hk gputime] firmware GPU timing active (timebase 1000000000 Hz)
    [hk gputime] 5.08 s wall
    [hk gputime]   vtx  14.2 ms  603 cmds  23.5 us each
    [hk gputime]   frag 11.0 ms  603 cmds  18.3 us each
    [hk gputime]   comp 13.2 ms  603 cmds  21.8 us each
    [hk gputime]   GPU busy (union) 38.4 ms = 0.8% of wall

An emulated x86-64 client is now driving our aarch64 Honeykrisp, and the GPU
timing instrument reports from inside it. Both problems — wrong driver, and
Vulkan running under emulation — are addressed by the same change.

### Known rough edges

- `vulkaninfo --summary` dies with **SIGILL (rc 132)** under the thunk. Some
  entry point it probes is unimplemented. `vkcube` is unaffected, so this looks
  like a diagnostic-tool path rather than a rendering one, but it is a warning
  that thunk coverage is not complete.
- `vkcube --present_mode 0` (immediate) **SIGSEGVs (rc 139)**; FIFO is fine.
  Worth remembering if the game exposes a present-mode setting.
- Vulkan thunking is enabled **globally** by `~/.fex-emu/Config.json`, which
  includes the Steam client itself. If the client misbehaves, scope it to an
  AppConfig instead of the global config.

### The open question for the game

pressure-vessel sets `VK_DRIVER_FILES` to container ICD JSONs that point at
**x86-64** drivers. With the thunk active the host-side loader is **aarch64** and
cannot load those. If the game reports no Vulkan device, that is the cause, and
the fix is to point `VK_DRIVER_FILES` at an aarch64 ICD reachable from inside
the container namespace. The container paths were added to the overlay list
already, since FEX derives its prefixes from the Fedora rootfs and so never
generates the Debian-multiarch layout the container actually uses.

---

# FIRST MEASUREMENT ON OUR DRIVER — and it overturns the working theory

The thunk landed. Ghost of Tsushima ran for 627 s on our aarch64 Honeykrisp,
natively, and reported through the instrument. (`[hk gputime]` exists only in
our build, so its presence in the Proton log is itself the proof of delivery.)

| | GPU time | share of GPU work | commands |
|---|---|---|---|
| vertex   | 8.9 s | 4.6% | 185,077 |
| fragment | 14.5 s | 7.5% | 185,070 |
| **compute** | **168.6 s** | **87.8%** | 155,169 |
| **union busy** | **191.9 s** | — | **30.6% of wall** |

## Two findings, both against the prevailing theory

**1. The GPU is not saturated. It is idle roughly 70% of the time.**
Every optimisation ranked so far assumed a GPU-bound frame — the whole
tessellation programme, the barrier work, the "reduce GPU work" framing. The
union says otherwise. Making shaders cheaper cannot recover time the GPU is
already not spending. The 41.9 W reading that started this line of thinking was
a whole-SoC number and simply could not distinguish "busy" from "awake".

**2. Of the GPU work that does exist, compute is 88% of it.**
Actual rasterisation is 3.7% of wall clock. Mean command durations:

    compute   1.086 ms      <- 155,169 of them
    fragment  0.078 ms
    vertex    0.048 ms

Compute commands are 14-22x longer than render commands. Whatever those
dispatches are, they are the GPU-side story; vertex and fragment are noise.

## What this does and does not license

It does NOT yet identify the frame-time bottleneck. 30.6% busy means the answer
is in the gaps, and we have not measured the gaps' cause. Candidates, in the
order they should be tested:

1. **What the compute work actually is.** `HK_PERFTEST=notess` is finally
   meaningful — the earlier "notess changed nothing" result was taken on the
   emulated 26.0.3 driver and is void. If compute time collapses, tessellation
   emulation is the GPU-side cost and our three tessellation commits are worth
   re-measuring in isolation.
2. **Whether the gaps are dependency stalls or submission latency.** The union
   already proves they exist; attributing them needs per-command gap analysis,
   which the interval list we collect could report directly.

## Frame rate is now reported by the driver

MangoHud stopped working the moment the thunk went in, and the reason is
structural: MangoHud is an x86-64 Vulkan layer loaded by the guest loader, which
the thunk replaces; the host-side aarch64 MangoHud is not visible inside the
pressure-vessel mount namespace. Rather than fight that, `hk_QueuePresentKHR` now
counts presents and the report carries the frame rate:

    [hk gputime] 2.17 s wall   55 frames  25.4 fps

This is better than the HUD for our purposes: the frame rate and the GPU timing
it must be read against now come from the same source, over the same interval.
`./gputime.sh -s` reports it too.

**Caveat on the numbers above:** that 627 s run includes launcher, intro video
and menus, during which the GPU does almost nothing. The steady-state gameplay
figure will be higher than 30.6% — possibly much higher. The next run has frame
counts, so it can be sliced properly.

---

# CORRECTION: in gameplay the GPU *is* saturated. Compute is 90% of it.

The 30.6% union figure above was diluted by launcher, intro video and menus,
exactly as that section warned. Isolating the steady-state gameplay reports
from the same run inverts the conclusion:

    [hk gputime] 5.25 s wall
    [hk gputime]   vtx      250.6 ms   4.8% of wall   2430 cmds    103.1 us each
    [hk gputime]   frag     218.9 ms   4.2% of wall   2400 cmds     91.2 us each
    [hk gputime]   comp    4256.1 ms  81.0% of wall   1659 cmds   2565.4 us each
    [hk gputime]   GPU busy (union) 4725.6 ms = 89.9% of wall
    [hk gputime]   120 command(s) not profiled (app claimed the timestamp slot)

Consecutive windows read 89.9% and 91.1%, so this is stable, not a spike.

**Retract the "the GPU is idle ~70%, shader optimisation cannot help" claim.**
During gameplay the GPU is ~90% busy. It IS the bottleneck, reducing GPU work
IS the lever, and the earlier ranking of optimisations was aimed at the right
target after all — it was only ever mis-measured, first by a whole-SoC power
proxy and then by averaging over menus.

## Where the time actually goes

| | per 5.25 s window | share of GPU busy | per command |
|---|---|---|---|
| vertex | 250.6 ms | 5.3% | 103.1 us |
| fragment | 218.9 ms | 4.6% | 91.2 us |
| **compute** | **4256.1 ms** | **90.1%** | **2565.4 us** |

Rasterisation is under 10% of GPU time. **Compute is 90%**, in 1659 commands
each averaging 2.57 milliseconds — 25x a fragment command. Whatever those
dispatches are, they are the entire performance story on this workload.

## The next measurement, and why it is now valid

`HK_PERFTEST=notess` on the same save. Tessellation on AGX is emulated in
compute (5 dispatches per draw, see TESSELLATION.md), which makes it the prime
suspect for a compute-dominated profile.

The earlier "notess changed nothing" result is **void** — it was taken on the
emulated x86-64 Honeykrisp 26.0.3, a driver we never built and whose behaviour
we cannot reason about. Every conclusion from before the thunk landed has to be
re-established on our driver, and this is the first one worth redoing.

If compute collapses under `notess`, the three tessellation commits on
`local-deploy` are aimed at 90% of the frame and deserve individual measurement.
If it does not, the compute is vkd3d-proton's or the game's own, and the next
step is identifying which dispatches cost 2.57 ms apiece.

## Note on the run that produced this

The session ended because **the driver's own kill of muvm took Steam down with
it** — muvm hosts both, and the game cannot be signalled separately from the
host because it lives in the guest PID namespace. Not a crash, and not a driver
fault. Worth stating plainly: "kill the game" always means "kill Steam too".

---

# Autonomous measurement: `autorun.sh`

Every measurement so far has cost a human several minutes of clicking through a
launcher, an intro video and a menu, and the human is now the slow part of the
loop. The numbers only mean anything at steady-state gameplay, which is 3-4
minutes after launch, so each experiment was expensive to obtain and hard to
repeat identically.

    ./autorun.sh baseline
    ./autorun.sh notess HK_PERFTEST=notess

Each run: kills any existing muvm, clears the Proton log, launches Steam
straight into the game, waits for the window, runs `nav.txt` to get through the
menus, waits until the reports show real gameplay, samples, then kills the
session and writes `runs/<name>/` with the raw reports, screenshots at each
stage, and a steady-state-only summary.

**Steady state is detected, not assumed.** A window counts as gameplay only when
its compute command count clears `GAMEPLAY_MIN_COMP` (default 800; menus sit
near zero, gameplay ran ~1650 per 5 s window). This is what stops the
launcher/menu dilution that made the first analysis of this run wrong by a
factor of three. The summary averages only qualifying windows.

## How input works

Wine renders into an X11 window on Xwayland (`:0`), so `xdotool` can inject
input and ImageMagick's `import` can capture it. Neither needs the compositor's
cooperation, which a native Wayland client would have required — `xdotool` was
useless earlier in this project for exactly that reason, when it was pointed at
the desktop rather than at Wine.

`nav.txt` holds the click/key sequence, deliberately separate from the harness
because it is the fragile, game-version-specific part.

## Safety notes

- `kill_muvm` resolves PIDs via `ps ... awk '$2=="/usr/bin/muvm"'`. It must
  never use `pkill -f /usr/bin/muvm`, which matches the harness's own shell —
  that mistake killed the controlling shell repeatedly earlier in this project.
- Killing muvm always takes Steam down with it; they are the same VM.
- The first `nav.txt` is a **discovery pass that sends no input at all**, only
  screenshots. Guessing keys blind does not merely fail; it can leave the game
  in a state that resembles a different bug.

---

## Driving the game without a human: what it took

The flow, mapped by driving it manually with screenshots:

    Steam -> launcher (Play / Options / Quit) -> unskippable videos
          -> main menu (CONTINUE selected) -> gameplay

Both menus have the wanted item selected by default, so `Return` twice is the
entire input sequence. No arrow keys, no clicking.

Four things had to be fixed before that worked, each of which failed *silently*:

1. **`import -window root` fails on Xwayland** ("missing an image filename").
   Capturing the window by id works. This is why the first discovery pass
   produced no screenshots at all and looked like the game had never appeared.
2. **The window is "Wine Desktop", capital D.** `xdotool search --name` is a
   case-sensitive regex, so the harness's `Wine desktop` pattern never matched.
3. **The game is a CHILD window of the Wine Desktop and does not inherit its
   focus.** `xdotool windowactivate` on the parent leaves keys going nowhere.
   The game window must be activated by name.
4. **`xdotool key --window` sends synthetic events, which Wine ignores.** Keys
   must go through XTest (plain `xdotool key`) with the window focused.

### Pointer coordinates are skewed and mouse input is unreliable

Xwayland reports the display as 3024x1964 while KDE's logical geometry is
2016x1310 — a 1.5x scale. A `mousemove` to (1181,1127) lands at (1169,1101), and
clicks on the launcher's "Show Launcher" checkbox did not register. Keyboard
input via XTest works reliably, so the harness uses keys only.

This matters because unchecking "Show Launcher" would remove the launcher step
entirely. Worth another attempt later with the scale factor applied, but it is
an optimisation, not a blocker.

## The menu is a legitimate cheaper proxy — and it broke the first detector

The main menu is itself a 3D scene, and measuring it directly:

| | menu | gameplay |
|---|---|---|
| frame rate | 15.7-18.0 fps | ~6 fps |
| GPU busy (union) | 85.9% | 89.9-91.1% |
| compute, % of wall | 73.2% | 81.0% |
| **compute per command** | **928 us** | **2565 us** |
| compute commands / 5 s | ~3960 | ~1659 |

Two consequences:

**The menu confirms the diagnosis independently.** It is GPU-bound and
compute-dominated in the same way as gameplay, at a completely different frame
rate. Whatever the compute work is, it is not specific to the open world.

**The first gameplay detector was wrong, and would have silently mis-measured
every experiment.** It gated on compute command *count* (">= 800 per window"),
assuming menus idle the GPU. They do not — the menu issues *more* compute
commands than gameplay (3960 vs 1659), just cheaper ones. The harness duly
declared "gameplay detected" while sitting in the menu and sampled that instead.

The detector now gates on **per-command compute cost** (>= 1500 us), with frame
rate as corroboration. That separates 928 from 2565 with a wide margin. The
summary also reports the lighter windows separately rather than discarding them,
since the menu is a useful data point in its own right.

This is the second time in this project that an averaging mistake nearly
produced a confident wrong answer — the first being menus diluting the union
figure to 30.6%. Both were caught by insisting the instrument report what it
actually measured rather than a single headline number.

---

# BASELINE, measured autonomously (runs/baseline)

First fully unattended run. 25 steady-state gameplay windows, 128.4 s:

    vertex      6087.7 ms    4.7% of wall
    fragment    5206.5 ms    4.1% of wall
    compute   103806.4 ms   80.8% of wall
    GPU BUSY  115100.7 ms   89.6% of wall
    compute is 90.2% of GPU busy time

    (menu, 28 windows, 145.2 s: GPU busy 71.2%, compute 84.6% of busy)

Per-window at steady state: **6.0 fps**, 2491 us per compute command, 1733
compute commands per 5 s window. The frame rate now comes from the driver's own
present counter, and it agrees exactly with the 6 fps reported by hand — which
is a useful cross-check that the instrument is measuring the real thing.

This reproduces the earlier hand-driven numbers (89.9-91.1% busy, ~2565 us per
compute command) closely enough to treat the harness as trustworthy, and it did
it without a human present.

## Confirmed shape of the problem

    GPU busy         ~90%     -> GPU-bound. Not stalls, not submission latency.
    compute          ~90% of that GPU time
    vertex+fragment  ~9%
    per compute cmd  ~2.5 ms

Everything now turns on what those compute dispatches are.

---

# notess: tessellation is NOT the compute cost (menu-level evidence)

`HK_PERFTEST=notess` on our own driver, flag confirmed live by the banner:

    [hk] HK_PERFTEST active: 0x1 (notess)

The run never reached gameplay (input bug, below), but it sat in the main menu
for 474 s, and the menu is a legitimate GPU-bound, compute-dominated workload.
Comparing like with like — main-menu windows only, isolated by per-command
compute cost in the 700-1200 us band:

| | baseline | notess |
|---|---|---|
| windows | 23 (119 s) | 91 (474 s) |
| frame rate | 18.31 fps | 18.07 fps |
| GPU busy | 74.5% | 84.1% |
| compute, % of wall | 63.6% | 72.3% |
| per compute command | 913 us | 943 us |

**Skipping every tessellated draw changes nothing.** The frame rate is
identical within noise and compute did not fall — it rose slightly, which is
run-to-run variation, not a real increase.

This finally settles a question that has been answered wrongly twice: the
earlier "notess changed nothing" was measured on the emulated 26.0.3 driver and
was void. Now it is measured on our driver with the flag verified active, and
the answer is the same. **The three tessellation commits on `local-deploy` are
optimising something that is not the bottleneck.**

Caveat worth stating: this is menu evidence, not gameplay. The menu may simply
contain little tessellated geometry. A gameplay `notess` run is still wanted,
and is now possible again with the input fix below.

## The input bug, and why it looked random

The harness kept losing the ability to send keys mid-session. Cause:

**Pointer coordinates are scaled 1.5x.** Xwayland reports the display as
3024x1964; KDE's logical geometry is 2016x1310. `xdotool mousemove 752 601`
puts the pointer at **(1128,901)**.

So a click aimed at the middle of the game window (1992,668) actually landed at
(2988,1002) — outside the window, which spans x 552-2472. It clicked the
desktop. After that, KDE's **focus stealing prevention** refused
`windowactivate`'s request to give the window focus back, and every subsequent
keystroke went to whatever the compositor considered focused. The launcher
`Return` worked only because that window was still freshly focused at that
point; nothing after it did.

That also explains the earlier failure to untick "Show Launcher".

The harness now **measures** the scale at startup rather than assuming it
(`xdotool mousemove 600 600`, read back, divide), converts screen coordinates
through it, and gives the game focus by *clicking inside the window* rather
than by asking the window manager politely.

## notess crashes the game on world load

Two independent `notess` runs failed to reach gameplay, and the second showed
why: clicking CONTINUE starts the load, the game dies, and Steam relaunches it
back to its launcher (two fresh `[hk gputime] ... active` banners in the log
mark the new devices). The menu is unaffected — it renders indefinitely at 18
fps under `notess` — so this is specific to loading the world.

`notess` skips draws that use tessellation, so this is not surprising in
hindsight: it is a deliberately-incorrect measurement flag, and Ghost of
Tsushima evidently cannot complete a world load with those draws missing.

**Consequence: `notess` cannot produce a gameplay number on this title.** The
menu comparison stands as the tessellation evidence, and it says tessellation is
not the cost. To get a gameplay-level answer we need an approach that keeps
rendering correct — measuring where the compute time goes rather than deleting
work and seeing what happens.

---

# What the compute actually is: 1,753 dispatches per frame

`runs/baseline2`, with dispatch counting added to the instrument. Steady-state
gameplay, 25 windows, 128.8 s:

    5.94 fps
    vertex      5895.9 ms    4.6% of wall
    fragment    5106.6 ms    4.0% of wall
    compute    99052.4 ms   76.9% of wall
    GPU BUSY  110054.9 ms   85.4% of wall
    compute is 90.0% of GPU busy time

Per-window detail is what matters:

    comp   4033.3 ms  1623 cmds  2485.1 us each
    52581 dispatches (32.4/cmd, 76.7 us each)   116055 draws

**Per frame: 1,753 compute dispatches and 3,868 draws.** Each dispatch averages
**77 us**, so compute costs ~134 ms of the ~168 ms frame.

Two things this rules out:

- **Not many tiny driver meta-dispatches.** At 77 us each these are substantial
  workloads. A clear, a copy or a query resolve is microseconds.
- **Not a submission-overhead problem.** 32 dispatches are packed per control
  stream, and the GPU is 85% busy; this is real GPU work.

The menu, for comparison: 472 dispatches per frame at 84 us each. Gameplay
issues **3.7x more dispatches per frame** at essentially the same per-dispatch
cost — so the cost scales with dispatch count, not with dispatch complexity.

## Input: the missing piece was Wine's internal focus

The game is a **child window** of "Wine Desktop". X focus sits on the parent, so
the child never receives Wine's internal focus and the game ignores every key
and click — which is why input worked once or twice by luck and then never
again. `xdotool windowfocus` on the *child* fixes it.

The tell is visible in screenshots: the menu items are **dimmed** when the game
lacks focus and **bright** when it has it. I had misread that dimming as a
loading state, which sent me chasing the wrong thing twice.

With focus correct, `space` then `Return` advanced the menu immediately
(17.5 -> 30 -> 8.6 fps as it loaded). The harness now does this in `focus_game`
before every key and click.

---

# THE ANSWER: it is the game's own compute shaders

`runs/origin`, with dispatch-origin attribution. Steady-state gameplay,
24 windows, 124.5 s, **5.96 fps**:

    vertex      5928.7 ms    4.8% of wall
    fragment    5074.6 ms    4.1% of wall
    compute    99477.2 ms   79.9% of wall
    GPU BUSY  110480.7 ms   88.7% of wall
    compute is 90.0% of GPU busy time

And the attribution, per 5.31 s window:

    dispatch origin: app 40332, gs/prerast 0, tess 2200, other 0
    55633 dispatches (32.1/cmd, 76.5 us each)

| origin | dispatches | share | per frame |
|---|---|---|---|
| **application (`vkCmdDispatch`)** | **40,332** | **72.5%** | **1,260** |
| driver internal (unattributed) | 13,101 | 23.5% | 409 |
| tessellation emulation | 2,200 | 4.0% | 69 |
| geometry shader / prerast | 0 | 0% | 0 |

## What this settles

**The bottleneck is Ghost of Tsushima's own compute shaders.** ~1,260 of them
per frame, at ~77 us each. Not driver emulation, not submission overhead, not
barriers, not tessellation.

**Tessellation is 4% of dispatches.** The three tessellation commits on
`local-deploy` therefore address at most ~3.6% of GPU time. Perfectly
eliminating tessellation would move 6 fps to roughly 6.2. That work was
carefully done and correctly measured against the wrong target — the ranking
that produced it was built on a whole-SoC power reading, before there was any
way to see inside the frame.

**Geometry-shader emulation is exactly zero.** Worth stating because
`hk_launch_gs_prerast` runs the *vertex* shader as a compute dispatch, which
made it a strong suspect for a compute-dominated profile. The game does not use
that path at all.

## Where the remaining leverage is, in order

1. **The game's compute shaders (72.5%).** This is an AGX *compiler* problem now:
   the quality of code generated for these shaders, occupancy, register
   pressure, and memory behaviour. It is the only thing large enough to change
   the frame rate materially.
2. **Driver-internal dispatches (23.5%).** 409 per frame, currently
   unattributed — meta operations (clears, copies, blits) and the helper at
   `hk_cmd_buffer.c:776`. Worth instrumenting next: if a meaningful share is
   redundant clears or format-conversion blits, that is addressable in the
   driver without touching the compiler.
3. **Tessellation (4%).** Already optimised; leave it.

## Method note

Every earlier answer to "what is slow" came from deleting work and watching the
frame rate (`notess`, `noflush`, `batch`). All three were inconclusive or void.
Attributing GPU time directly answered it in one run, and the instrument that
did it cost less to build than any single one of those experiments.

---

## Per-shader attribution: turning "the game's compute shaders" into a ranked list

**Date:** 2026-09-04. **Driver:** mesa-local `local-deploy`.

### Why the previous answer was not yet actionable

The `runs/origin` measurement established that compute is 90% of GPU busy time
and that 72.5% of dispatches come from the application. That is a true statement
and a useless one: it does not say whether that is five shaders run 250 times
each or two hundred run six times, and those two worlds have nothing in common
as optimisation problems.

Two things were also still wrong with the origin instrument:

1. **23.5% of dispatches were unattributed.** `HK_DISP_OTHER` was declared and
   never incremented. Everything issued through `hk_dispatch_precomp`
   (`hk_cmd_buffer.c:756`) — the libagx helper kernels: clears, fills, copies,
   prefix sums, query bookkeeping — went uncounted. That is ~409 dispatches per
   frame with no name attached to any of them.

2. **`vk_meta` work was mislabelled as application work.** Honeykrisp implements
   image copies and blits by calling `vkCmdDispatch` through the common
   `vk_meta` framework, so they arrive at `dispatch()` indistinguishable from
   the game's own compute and were counted as `HK_DISP_APP`. The 72.5% was
   therefore an upper bound on the application's share, not a measurement of it.

### What was built

**Exhaustive origin accounting.** There is exactly one function that writes a
CDM launch into a control stream — `hk_dispatch_with_usc_launch`
(`hk_cmd_dispatch.c:55`), which is also where `cs->stats.cmds++` lives, i.e. the
denominator every percentage is measured against. It has exactly two entry
paths: `hk_dispatch_with_usc` and `hk_dispatch_precomp`. Both are now
instrumented, *after* their early returns, so the origin counts reconcile
exactly with the dispatch total rather than approximately.

The origin enum is now `app / meta / gs / tess / precomp`:

* `meta` is separated by reading `cmd->in_meta`, which `hk_meta_begin`/`_end`
  already maintain — no new state.
* `precomp` additionally records a per-kernel histogram keyed by
  `enum libagx_program`, so the driver-internal work is reported *by name*
  (`fast_clear_7`, `copy_uint4`, `prefix_sum_tess_scan_1`, ...) rather than as a
  single opaque bucket.

The report also prints a `recorded` total counted at the chokepoint at record
time, next to the `dispatches` total summed at submit time. They measure the
same thing by two different routes; a divergence between them is a bug in the
instrument, and printing both makes that self-checking rather than assumed.

**Per-shader table.** Every dispatch is attributed to the shader that ran, keyed
by the `struct agx_shader_info *` (stable for a compiled variant's lifetime,
unique per variant). Per shader, per report period: dispatch count, total
invocations launched, and how many launches had an indirect grid.

### The cost model, and why it is a model

The firmware timestamps a *control stream*, not a dispatch, and a compute
control stream here holds ~32 dispatches. There is no way to time one shader
directly without splitting streams, which would perturb the thing being measured
past usefulness. So the ranking is:

    invocations (known exactly, from the launch grid)
      x cycles-per-invocation (the compiler's own estimate)

`agx2_stats` carries three cycle estimates, one per issue pipe: `alu`, `fscib`
(F16/F32 and the SCIB path) and `ic`. A shader is limited by whichever pipe is
busiest, so the **max** is the right per-invocation figure — not the sum, which
would assume the pipes never overlap.

**This model is blind to memory stalls.** That is exactly why the report prints
the achievable occupancy (`thr`, max threads in flight per core, which register
pressure caps) and the spill/fill count next to the estimate: a shader with a
low cycle estimate and low occupancy is a memory-bound shader that the model
will under-rank, and the two columns together say so.

### Usage

Unchanged: `HK_GPUTIME=<seconds>`. The report gains two tables — top 20 compute
shaders by estimated cost, and the top 12 libagx helper kernels by dispatch
count — both reset per period, with shader ids stable across periods so a shader
can be followed from one report to the next.

---

## Where the GPU time actually goes (runs/shaders, 129 windows, 4099 frames, 6.0 fps)

### Origin, now complete and self-checking

    app          5,044,485   1230.7/frame   70.1%
    meta           171,315     41.8/frame    2.4%
    gs/prerast           0      0.0/frame    0.0%
    tess           283,188     69.1/frame    3.9%
    precomp      1,696,698    413.9/frame   23.6%
    -----------------------------------------------
    recorded     7,195,686   1755.5/frame
    submitted    7,198,755   1756.2/frame

The last two lines are the point of the exercise: `recorded` is counted at the
CDM chokepoint at record time, `submitted` is summed from `cs->stats.cmds` at
submit time. Two independent routes to the same number, agreeing to 0.04%. The
attribution is exhaustive, not approximately exhaustive.

`meta` is only 2.4%, so separating it out barely moved the application's share —
but it had to be measured rather than assumed, because before this those
dispatches were being counted as the game's own compute.

### The 23.6% that was unattributed, by name

    draw_robust_index_1    144.8 disp/fr        4,635 invoc/fr   35.4%
    copy_uint4              97.8 disp/fr      100,893 invoc/fr   23.9%
    tess_tri_0              33.5 disp/fr        6,778 invoc/fr    8.2%
    tess_tri_1              33.5 disp/fr        6,778 invoc/fr    8.2%
    prefix_sum_tess_1       33.5 disp/fr       34,348 invoc/fr    8.2%
    draw_robust_index_2     30.1 disp/fr          964 invoc/fr    7.4%
    write_u32s              15.9 disp/fr           16 invoc/fr    3.9%

**`draw_robust_index` is 175 dispatches per frame — 10% of every dispatch the
GPU runs — for 5,599 invocations between them.** That is 32 invocations per
dispatch: a single workgroup. This is the robustness index-buffer clamp, emitted
once per robust indexed draw. It is almost pure launch overhead, and
`HK_PERFTEST=norobust` already exists to switch it off.

### What the application's compute actually is

The invocation counts give it away: **2,304,562 ~ 1920 x 1200**. Shader after
shader launches exactly one invocation per pixel. This is a chain of ~15
full-screen compute passes — deferred lighting and post-processing — not a few
heavy compute kernels.

That has a direct consequence: **cost scales with pixels**, so internal
resolution is a linear lever on the largest single block of GPU time.

### The finding that redirects the whole investigation

    menu       33.7 ms/frame   14.33 M invoc/frame   2.35 ns/invoc   8.0% of peak ALU
    gameplay  134.7 ms/frame   33.98 M invoc/frame   3.96 ns/invoc   4.7% of peak ALU

Peak here is 4096 lanes x 1.296 GHz = 5.308e12 lane-cycles/s, and the cycle
estimate is the compiler's own (`agx2_stats`, max of the alu/fscib/ic pipes).
On those estimates an invocation should cost 0.18 ns. It costs 3.96.

**The workload runs at under 5% of the machine's ALU throughput.** It is not
ALU-bound, and it is off by a factor of 21 — far too large to be explained by
loop-count underestimation in the static model. Making the compiler emit fewer
instructions therefore cannot be the main lever: the machine is already idle on
the ALU side while the clock runs.

Candidates for where the other 95% goes, in the order they are worth testing:

1. **Per-dispatch serialisation.** Honeykrisp emits a full, conservative CDM
   cache flush after every `AGX_BARRIER_ALL` dispatch. At 1755 dispatches per
   frame that is ~1755 full drains per frame; the GPU cannot overlap anything.
   `HK_PERFTEST=noflush` bounds this. **This has never actually been tested on
   this game** -- see the retraction below.
2. **Occupancy and spilling.** Two of the hot shaders spill:
   id 160 (255 GPRs, occupancy 384, 272 spills+fills, 17 dispatches/frame) and
   id 102 (255 GPRs, occupancy 384, 181 spills+fills). Everything else reaches
   the full 1024. Spilling turns ALU work into memory traffic, which is exactly
   the kind of cost the static model cannot see.
3. **Memory bandwidth.** 34 M invocations/frame at 6 fps is 204 M/s; it would
   take ~2 KB per invocation to saturate 400 GB/s, which is implausible for a
   post-processing pass. Bandwidth is the least likely of the three, but it is
   measurable and should not be assumed away.

### RETRACTION: the noflush and batch results were void

Recorded earlier: "on their own neither noflush nor batch have an effect on this
game." **That result was measured while the game was running the distro driver
(Mesa 26.0.3), which does not implement either flag** -- the FEX Vulkan thunk
was not yet in place, so nothing the local build did could reach the game. The
flags were inert because they did not exist in the driver being executed, not
because they did nothing. Both are being retested now that delivery is proven.

### Harness lesson: never edit a running bash script

The `runs/shaders` harness died silently after its sample completed, before
writing `reports.txt` or `summary.txt`. Cause: `autorun.sh` was rewritten in
place while bash was still executing it. Bash reads a script incrementally by
byte offset, so an in-place rewrite makes it resume at a meaningless offset. The
data survived only because it also lives in the Proton log, which was recovered
by hand. Edit a copy, or wait for the run to finish.

### Input delivery: `xdotool click` does not work, mousedown/mouseup does

The same run sat at the main menu for twelve minutes through nine retries of
click / Return / space. Diagnosis, in order:

* Pointer *motion* was being delivered the whole time -- hovering visibly moved
  the menu highlight from CONTINUE to NEW GAME. So focus and coordinates were
  both fine, and the pointer-scale calibration was correct (verified: asking for
  (495,405) landed at (742,607), exactly on CONTINUE).
* `xdotool key Down` did not move the selection. Keys were not arriving.
* `xdotool click 1` did nothing, nine times.
* `xdotool mousedown 1; sleep 1; xdotool mouseup 1` advanced the menu on the
  first attempt.

A click is a press and a release in the same instant. The game samples input on
its own frame cadence, which at 6-18 fps is 55-165 ms; a button that goes down
and up between two samples is never observed. `autorun.sh` now holds both
buttons and keys for 1 s.

---

## HK_PERFTEST=noflush hangs the GPU on this game

Retested properly (the first test was void -- wrong driver). Result:

    16:49:55  run starts, navigation begins
    16:53:01  kernel: asahi 406400000.gpu: QueueJob 8184923: Job timed out
              on the DRM scheduler, things will probably break (ran: true)

The game reached ~71 fps during the intro, then stopped presenting entirely and
put up a black screen; `import` on the window hung too. The GPU reports stop at
exactly the timestamp of the DRM scheduler timeout.

So `noflush` is not merely "renders incorrectly" on this workload -- it faults
the GPU. It cannot serve even as a diagnostic upper bound on the CDM barrier
tax, which is what it was written for. **Do not run it again on this game.**

Bounding the barrier cost needs a different approach: count the flushes (the
driver already tracks `cs->stats.flushes`) and expose them next to the dispatch
count, then attack the ones that are provably unnecessary rather than removing
all of them.

### A performance number from a run that did not render is worse than no number

`noflush` produced a real, plausible frame rate while drawing nothing. Every
flag on this branch that touches barriers, robustness or tessellation can do
this. `verify-frame.sh` now checks each run's screenshots:

    ./verify-frame.sh runs/<name> [runs/<reference>]

It reports mean, standard deviation and distinct-colour count per screenshot and
flags BLACK / FLAT / LOW-DETAIL, builds a `thumbs.png` contact sheet so the
frames can actually be looked at, and -- given a reference run -- prints RMSE and
NCC of the gameplay frame against it.

The reference comparison is deliberately reported as a number rather than a
pass/fail: two gameplay screenshots from different runs are never identical
because the camera, weather and time of day all move, so a pixel difference is
not evidence of a bug. What is reliable is catching a frame that has stopped
being an image at all. `runs/shaders` (the baseline) verifies clean.

---

## HK_PERFTEST=norobust: works exactly as intended, and does not help

                        dispatches/fr  precomp/fr  draw_robust_index  invoc/fr   compute ms/fr   fps
    baseline (shaders)      1755.5        413.9      175/frame        33.98 M       134.7       6.00
    norobust                1693.6        260.5      gone             34.11 M       142.1       5.82

The robustness index-clamp dispatches disappeared completely and total dispatch
count fell 10%. Compute time went *up* 5%. The norobust sample is 19 windows
against the baseline's 129, so the honest reading is "no change", not "worse" --
but there is certainly no win here.

**This kills the per-dispatch-overhead hypothesis.** Removing 175 dispatches per
frame, each of which also carried a CDM cache flush, moved nothing. Cost tracks
*invocations*, not dispatches, and the invocation count was identical across the
two runs. The earlier framing of "1755 dispatches x 76.7 us each" was misleading:
that average is dominated by a handful of enormous full-screen dispatches, and
the hundreds of 32-invocation helper launches contribute almost nothing.

So `norobust` should be treated as neutral-and-slightly-risky, not as an
optimisation. Leave it off.

---

## THE ACTUAL FINDING: dynamic resolution is enabled and never engages

> **SUPERSEDED (item 44).** True of that session only. Dynamic resolution does
> engage in general, moving render resolution over a 4x range, which
> invalidated a great many later fps comparisons until it was pinned. See
> `data/measurement-hazards.md`.

From the game's own log (`.../Documents/Ghost of Tsushima DIRECTOR'S CUT/
Ghost of Tsushima DIRECTOR'S CUT.log`):

    [Settings]  Resolution: 1920x1200
    [Settings]  Dynamic resolution FPS target: 30
    [Settings] Anti aliasing: FSR
    [Settings] Upscale method: FSR
    [Settings] Upscale quality: Dynamic
    [Upscaler] ConvertScreenToRenderRes: wxh = 1920x1200 | min = 960x600 | max = 1920x1200
    ...
    [Render] Working set: 4061MB ... fps: 5.8

The game ships a dynamic resolution controller, it is switched on, it targets
30 fps, and it is permitted to drop the render resolution to 960x600 -- one
quarter of the pixels. `ConvertScreenToRenderRes` is called exactly twice, both
during initialisation, both returning the maximum. The game then runs at 5.8 fps
-- missing its own target by more than 5x -- and never scales down.

The driver-side measurement agrees and is much stronger than the log: across 129
windows and 4099 frames of gameplay, shader after shader launched exactly
2,304,562 invocations. 1920 x 1200 = 2,304,000. The render resolution is pinned
at native for the entire session.

This matters more than anything else found so far, because of what was
established above:

* compute is 90% of GPU busy time and the GPU is 88.7% busy -- the frame is
  GPU-bound on compute;
* the application's compute is a chain of ~15 full-screen passes, one invocation
  per pixel;
* cost is proportional to invocations and independent of dispatch count.

A quarter of the pixels is therefore close to a quarter of the compute. The
mechanism to do that is already in the game, already enabled, and idle.

### Two things to establish, in order

1. **Does a fixed upscale quality work at all?** If the DRS *controller* is what
   is broken, pinning `UpscaleQuality` to a fixed level should still reduce the
   render resolution, because that path does not need a feedback measurement.
   Settings live in the Wine registry at
   `pfx/user.reg`, key `Software\Sucker Punch Productions\Ghost of Tsushima
   DIRECTOR'S CUT\Graphics`:

       "UpscaleMethod"=dword:00000002        (FSR)
       "UpscaleQuality"=dword:00000003       (= "Dynamic", per the log)
       "DynamicResolutionTargetFPS"=dword:0000001e   (30)
       "FullscreenWidth"=dword:00000780      (1920)
       "FullscreenHeight"=dword:000004b0     (1200)

   The quality names in the executable are, in memory order,
   `Ultra Performance, Performance, Balanced, Quality, Ultra Quality,
   Ultra Quality Plus`, with `Dynamic` stored separately -- so the enum order
   cannot be read off statically and has to be probed. Backup of the registry
   before any change: `pfx/user.reg.backup-before-upscale-*`.

2. **Why does the controller not engage?** A DRS controller of this kind decides
   using GPU frame time from D3D12 timestamp queries, which vkd3d-proton maps
   onto Vulkan timestamp queries. The game *is* issuing them -- the gputime
   instrument reports "128 command(s) not profiled (app claimed the timestamp
   slot)" in every single window, which is precisely the driver observing the
   application taking timestamps. So the queries are being made; the question is
   whether the values coming back are usable. Note that
   `hk_physical_device.c:809` sets `timestampPeriod = 1.0f` under a comment
   reading "FIXME: Is timestamp period actually 1?".

---

## ROOT CAUSE: vkCmdWriteTimestamp2 could not measure compute work

The question left open above -- *why* does the game's dynamic resolution
controller never engage -- has a concrete answer, and it is a driver bug.

### The mechanism

The firmware writes a timestamp when the control stream it is attached to
**completes**. `hk_CmdWriteTimestamp2` attached the timestamp to the currently
open compute control stream and then left that stream open, so every dispatch
recorded afterwards joined it. For the ordinary pattern

    WriteTimestamp(A); Dispatch; Dispatch; WriteTimestamp(B)

the dispatches landed inside A's stream, B got a fresh empty one, and B - A came
out as roughly zero no matter how much work sat between them. The split existed
but happened lazily, on the *next* write rather than on the write that needed
it. The comment above the code already described the correct behaviour; the code
did not implement it.

### The measurement

`tests/tstest.c` brackets four 512 MiB `vkCmdFillBuffer` calls -- 2 GiB of GPU
writes, no render pass anywhere, so purely the compute path -- between two
timestamps, and compares the query delta against wall-clock time of the submit:

                  wall clock          queries report
    before     11.0 - 12.0 ms      0.045 - 0.049 ms
    after      11.0 - 11.7 ms     10.84  - 11.10 ms

Before the fix the queries claimed the GPU wrote 2 GiB in 47 microseconds. That
is 43 TB/s on a part with 400 GB/s of bandwidth -- not a plausible number, and
not a small error in a measurement but the absence of one. After the fix the
queries land within a few percent of wall time; the remainder is submit
overhead, which is not GPU time and correctly is not counted.

### Why this explains the game

Ghost of Tsushima writes about four timestamps per frame -- visible from the
driver side as "128 command(s) not profiled (app claimed the timestamp slot)" in
every 5 s window. It has dynamic resolution enabled, targets 30 fps (a 33 ms
budget), and is allowed to drop to 960x600. Told that it spent 0.05 ms of its
33 ms budget, the controller concludes it has several hundred times the headroom
it needs and holds render resolution at native 1920x1200 -- which is exactly
what the driver-side invocation counts showed for 4099 consecutive frames.

### The fix

`hk_query_pool.c`: end the compute control stream immediately after attaching
the timestamp, so nothing recorded later can join it. One extra hardware command
per timestamp write, which the existing "splitting compute control streams is
inexpensive" note was already accepting.

### Still broken: the graphics path

Several timestamps inside a single render pass all copy the first one's value
(`libagx_copy_timestamp`), so a pair bracketing draws *within* a render pass
still reports zero. Splitting a render pass is genuinely expensive, unlike
splitting a compute stream, so that one needs a different approach and is left
alone for now. It does not affect this game, whose timestamp writes are on the
compute path.

### What this does NOT yet establish

That fixing the measurement makes the game scale down. The controller may have
other inputs, or may need more than one frame of history to react. That is what
the `tsfix` run is for. What is established is that the input it was being given
was wrong by two orders of magnitude.

---

## The timestamp fix alone did not make dynamic resolution engage

Stated plainly, because the hypothesis was wrong even though the bug it predicted
was real:

                    invoc/frame   compute ms/frame   fps   ConvertScreenToRenderRes
    baseline          33.98 M          134.7        6.00   2 calls, both at init, 1920x1200
    tsfix             33.97 M          135.9        5.90   2 calls, both at init, 1920x1200

Identical. `vkCmdWriteTimestamp2` really was reporting 0.047 ms for 11 ms of GPU
work and that really is a driver bug worth fixing on its own merits, but it is
not what gates the controller.

## Honeykrisp's own vkd3d profile has never reached this game

The `tsfix` log carried a line that had been sitting there all along:

    [D3D] Max supported feature level: 11.0

That is what vkd3d reports when `vertexPipelineStoresAndAtomics` is missing.
Honeykrisp does not expose it by default -- `hk_physical_device.c:268` gates it
behind a driconf option -- and `src/asahi/vulkan/00-hk-defaults.conf` turns it on
precisely for this case:

    <engine engine_name_match="DXVK|vkd3d">
       <option name="hk_disable_border_emulation" value="true" />
          "The amount of bug reports we get about Honeykrisp being slow
           outweighs the number of games that need custom border emulation."
       <option name="hk_enable_vertex_pipeline_stores_atomics" value="true" />
          "Needed for FL11_1."
    <engine engine_name_match="vkd3d">
       <option name="hk_fake_minmax" value="true" />
          "required for FL12_0 ... we prefer to fake support and fail D3D
           conformance rather than be limited to FL11."
       <option name="hk_image_view_min_lod" value="true" />
          "We need this for FL12_0."

**None of these have ever been active for this game**, for two independent
reasons:

1. Fedora's mesa package does not ship `00-hk-defaults.conf` at all --
   `/usr/share/drirc.d/` contains only `00-mesa-defaults.conf`,
   `00-radv-defaults.conf` and `10-asahi-browser-apple.conf`.
2. The driver searches `$DATADIR/drirc.d` (`src/util/xmlconfig.c:1399`), and the
   locally built driver deployed to `/usr/lib64` has `DATADIR` compiled to
   `$HOME/Projects/mesa-local/install/share` -- which exists on the host
   but not inside the pressure-vessel mount namespace the game runs in.
   `deploy-system-driver.sh` copies only the `.so`.

Verified directly rather than inferred, with `tests/drirctest.c`, which creates
instances with different `pEngineName` values and prints the one feature that
gates FL11_1:

    engine=(none)   Apple M1 Max   vertexPipelineStoresAndAtomics = false
    engine=vkd3d    Apple M1 Max   vertexPipelineStoresAndAtomics = TRUE
    engine=DXVK     Apple M1 Max   vertexPipelineStoresAndAtomics = TRUE

So the profile works when the config is found. It simply was not being found
where the game runs.

### The fix

`~/.drirc`, kept in the repo as `drirc-hk-vkd3d.conf`. `$HOME/.drirc` is parsed
unconditionally and in addition to the DATADIR search
(`src/util/xmlconfig.c:1403-1408`), so it works regardless of where the driver
was built or how the container is assembled, and `$HOME` is certainly present
inside the container -- the game loads its saves from it.

It changes nothing for non-vkd3d clients: the `engine=(none)` probe still reports
false, so the desktop compositor is unaffected.

### Why this might matter more than the feature level itself

At FL 11.0 the game may simply be refusing to initialise FSR -- note that the
log shows `[NxUpscaler] XeSS init, code = 0` and an `[NxFfx]` line, but no FSR
initialisation result. If FSR never comes up, there is no upscaler, and a
dynamic resolution controller that works by driving an upscaler has nothing to
drive. That would explain why the resolution never moves and why fixing the
timestamps changed nothing.

This is a hypothesis, not yet a result. The `drirc` run tests it.

---

## CORRECTION: the drirc profile does reach the game, and is not the feature-level gate

The section above hypothesised that Honeykrisp's vkd3d profile never reached the
game and that this explained `Max supported feature level: 11.0`. Instrumenting
the driver settled it, and the hypothesis was wrong.

`HK_DEBUG_DRIRC=1` (new, `hk_instance.c`) prints the app and engine name the
driver actually received and the resulting option values. From the game's own
process:

    [hk drirc] app="GhostOfTsushima.exe" engine="vkd3d"
    [hk drirc] -> vertex_pipeline_stores_atomics=1 fake_minmax=1
                  image_view_min_lod=1 disable_border_emulation=1

All four options are on. The engine name survives the FEX Vulkan thunk intact --
the thunk marshals `pApplicationInfo` and its strings correctly, which was the
other thing worth ruling out. And the game still reports feature level 11.0.

What was genuinely established, and is still worth keeping:

* Fedora's mesa package really does not ship `00-hk-defaults.conf`.
* The driver searches `$DATADIR/drirc.d`, which for this build is an install
  prefix rather than a system path. That is fragile even though it happens to
  work here, because `$HOME/Projects/...` is visible inside the
  container.
* `DRIRC_CONFIGDIR` is now forwarded explicitly (steam wrapper), listing the
  system directory first so Mesa's per-application workarounds are not dropped,
  with a copy of the profile in `drirc.d/`. This makes the behaviour
  deterministic rather than dependent on which paths the container happens to
  expose.

What was NOT established, and what I claimed too early: that the profile was
inactive. It probably was active all along, via the install-prefix path. The
feature level is capped by something else in vkd3d-proton, not by these options.

### Diagnostic order note

The mistake was reasoning from an absence -- no config file in `/usr/share` --
to a conclusion about a running process, instead of asking the running process.
The probe that settled it took ten minutes to write. It should have come first.

---

## FALSIFIED: compute cost is not proportional to pixels

The model everything above was built on -- that the game's compute is ~15
full-screen passes, so cost scales with pixels and resolution is the big lever
-- is wrong. Tested by setting the game to 1280x720 (it snapped from the
requested 1280x800 to 16:9) and measuring:

                     pixels     invoc/frame   Gcycle/frame   compute ms/frame   fps
    1920x1200      2,304,000      33.97 M        36.70            135.9        5.98
    1280x720         921,600      13.70 M        17.82            130.0        6.30
    ratio              0.40         0.40          0.49             0.96        1.05

Invocations tracked pixels exactly. The compiler's estimated cycle demand
halved. **Measured compute time fell 4%.** Sixty percent less work bought five
percent more frame rate. Frames verified correctly rendered at both.

So dynamic resolution, even if it worked, would buy almost nothing -- which
retires the entire line of investigation above it, including the feature-level
and FSR questions. Worth knowing before spending more on them.

### What is actually invariant

                          1920x1200    1280x720
    compute control streams/frame   55.9        56.4
    dispatches/frame              1781.6      1788.0
    dispatches per control stream   31.9        31.7
    microseconds per control stream 2431        2307
    compute ms/frame               135.9       130.0

Everything structural is identical to under 1%. The GPU issues the same 56
control streams holding the same ~1780 dispatches, and each stream takes the
same ~2.4 ms, whether the work inside it is 34 million invocations or 14
million.

### Also refuted: fixed per-dispatch cost, and the barrier

The obvious reading of "time = dispatches x 75 us regardless of work" is a fixed
per-dispatch overhead, and the obvious suspect is the full conservative CDM
cache flush that `dispatch()` requests after every dispatch via
`AGX_BARRIER_ALL`. `tests/disptest.c` measures it directly -- N identical
dispatches through `vkCmdFillBuffer`, which uses **the same `AGX_BARRIER_ALL`**,
timed with the now-working timestamp queries, differencing 1024 against 256
dispatches to cancel any fixed submit cost:

    bytes/dispatch   us/dispatch
             256        1.77
            4096        1.94
           65536        2.30
         1048576       11.29
        16777216       96.49

A dispatch plus its cache flush costs about 2 microseconds, not 75, and the cost
scales cleanly with the work done. So neither the launch nor the barrier is the
game's problem, and the barrier-elision optimisation that looked so promising
would have been wasted effort.

### The surviving lead

Comparing the per-shader tables across the two resolutions, one shader did not
shrink when the render did:

    shader   1920x1200 invoc/fr   1280x720 invoc/fr   GPRs   occupancy   spills
    id 90         6,912,000          2,509,824 (0.36x)  95    1024          0
    id 37         2,304,000            836,608 (0.36x) 103    1024          0
    id 160        1,114,112          1,179,648 (1.06x) 255     384        272

id 160's work is resolution-independent, and its estimated share doubled from
7.3% to 16.5% purely because everything around it shrank. It is also the only
shader in the table that is register-starved: 255 GPRs, occupancy capped at 384
of a possible 1024, and 272 spill/fill instructions.

That matters because of a limitation designed into the cost model from the
start: **it cannot see memory stalls.** A spilling shader's real cost is far
above its ALU estimate. "Estimated cycles halved, measured time did not move" is
exactly the shape of a frame dominated by shaders the estimate calls a minority.

### The instrument that settles it

`HK_GPUTIME_ISOLATE=1` gives every shader dispatch its own control stream so the
firmware times it individually, turning the per-shader table from estimated cost
into **measured** GPU time. Streams holding work from more than one shader are
marked MIXED and not attributed at all, rather than credited to whichever
dispatch came first.

The perturbation was measured rather than assumed: a control stream costs about
2 us to start, against the ~2400 us these streams take. Absolute frame time is
disturbed; the ranking is not.

---

## MEASURED per-shader time, and it inverts the estimate

`HK_GPUTIME_ISOLATE` was built to give every dispatch its own control stream.
**It did not engage** -- the run still shows 56.1 streams per frame and 31.5
dispatches per stream, unchanged. Attribution therefore happened at whole-stream
granularity, only for streams that incidentally held one shader's work. That
still covered 3688 ms of the ~4250 ms of compute in each 5 s window, i.e. 87%,
so the ranking below is sound even though the resolution is coarser than
intended.

    id   disp/fr   invoc/fr    est%   meas ms/fr   meas%   instrs  gprs  occ  spill
    157      2.0      3,776    0.0%      39.84     34.6%      186    31 1024      0
    280     93.7     47,952    1.2%      29.68     25.7%    11251   183  576      0
    94       1.0        352    0.0%      24.12     20.9%      127    19 1024      0
    45       1.0  2,304,000    4.1%       7.33      6.4%      564    27 1024      0
    160     17.0  1,114,112    6.6%       4.58      4.0%     2724   255  384    272
    90       3.0  6,912,000   21.7%       0.65      0.6%     1372    95 1024      0

**Three shaders are 81% of measured GPU time, and all three have negligible
invocation counts.** Meanwhile id 90 -- 6.9 million invocations per frame, the
single largest item in the estimate at 21.7% -- is 0.6% of measured time.

id 157 is the clearest statement of the problem: 3,776 invocations of a
186-instruction shader taking 39.84 ms per frame. That is roughly 10 us per
invocation, about 13,000 cycles per instruction. No compute shader executes that
slowly. It is not computing.

This also settles why the resolution experiment did nothing: the
pixel-proportional shaders (90, 45, 89, 88 -- millions of invocations each) are
a rounding error in the real cost, so removing 60% of the pixels removed 60% of
something that was never the bottleneck.

### It also retires the cost model

The estimate -- invocations x cycles-per-invocation from `agx2_stats` -- is not
merely imprecise here, it is anti-correlated with reality at the top of the
table. It was built with the caveat that it cannot see memory stalls, and that
caveat has now eaten the model. Rank by `meas%`; keep `est%` only as the
contrast that shows how far off a static model can be.

### Two candidate causes, needing different fixes

1. **A data-dependent loop.** `agx2_stats` has a `loops` field which the table
   does not print. A 186-instruction shader with a loop can execute unboundedly
   many instructions, and the static estimate would be meaningless for exactly
   the shaders that matter.
2. **The interval includes waiting, not executing.** The firmware timestamps a
   command start-to-end. If that span includes time the command spends waiting
   on a barrier rather than running, then these three shaders are simply the
   ones sitting at synchronisation points -- and, more seriously, the "GPU busy
   90%" figure that has anchored this whole investigation would be overstated,
   with the machine idle rather than saturated.

These lead to completely different work, so the next step is to distinguish
them, not to act on either.

### Outstanding defects in the instrumentation

* `HK_GPUTIME_ISOLATE` does not fire. The guard is
  `dev->gputime.isolate && cs->cmd && cs == cs->cmd->current_cs.cs` in
  `hk_dispatch_with_usc`; app dispatches do use `current_cs.cs`
  (`hk_cmd_buffer_get_cs`, `hk_cmd_buffer.h:648-652`), so the reason is not yet
  known and needs a counter rather than more reading.
* `shaders.sh` column offsets are stale since the measured columns were added.
* Iterating on this via 12-minute game runs is the wrong loop. A local
  compute-shader test would turn it into seconds.

---

## CONFIRMED (partly): the top shader loops; the second one does not

Printing `stats.loops` settled the first of the two candidate causes and
eliminated it for the second.

    id   blake3         disp/fr   invoc/fr   est%   meas ms/fr  meas%  inst  loops  wg
    150  2e4ccbe89b31       2.0      3,776   0.0%      39.53   47.9%   186     1    32
    94   e4919730b5b4       1.0        352   0.0%      23.67   28.7%   127     0    32
    45   713d5ea0d214       1.0  2,304,000   4.1%       7.34    8.9%   564     0    64
    160  6fc0efe726d2      17.0  1,114,112   6.6%       4.58    5.5%  2724     2    64

(The `id` is a per-process index. `2e4ccbe89b31` is the same shader that was id
157 in the previous run -- which is exactly why the SPIR-V hash was plumbed
through into `agx_shader_info`.)

**id 150 has a hardware loop**, and at 47.9% of attributed measured time it is
the single largest cost in the frame. That fits the mechanism measured directly
in `tests/cstest.c`: a 32-invocation dispatch of a 13-instruction shader with a
data-dependent loop takes 2881 us, against 7 us with the loop trip count set to
1, while its static estimate stays at 9 cycles either way.

**id 94 has zero loops**, and the story does not stretch to cover it. 352
invocations, 127 instructions, 23.67 ms per frame -- about 67 microseconds per
invocation with nothing in the static description to explain it.

Between them these two shaders are **47% of all compute time** (39.53 + 23.67 of
134.8 ms/frame) while performing essentially no arithmetic. The property they
share is a near-total absence of parallelism: 32-thread workgroups, 11
workgroups for id 94, on a 32-core GPU. `cstest.c` measured that effect too --
4096 invocations cost 32 us per dispatch where 32 invocations cost 60 us for the
same loop count, because more threads hide the latency.

### Why this closes out the structural experiments

Every lever tried today moved something that was never the cost:

* resolution scales the million-invocation shaders (ids 90, 45, 89, 88), which
  measure at under 10% of compute between them;
* dispatch count, cache flushes and feature level do not touch these two at all.

47% of the frame sits in two dispatches per frame that do no meaningful work.

### Next: read the shaders

Both are the game's own `vkCmdDispatch` calls, so the algorithm is not ours to
change. The question is why this driver executes them so slowly, and that needs
the actual code rather than more inference. Candidates worth checking in the
disassembly, in rough order of suspicion: subgroup operations lowered badly
(`agx_nir_lower_subgroups.c`), emulated 64-bit atomics, and dependent memory
chains with no occupancy to hide them.

    AGX_MESA_DEBUG=shaders MESA_SHADER_CACHE_DISABLE=true

then find `source_blake3` 2e4ccbe89b31 and e4919730b5b4 in the NIR headers.

---

## RETRACTION: the measured per-shader ranking is unsound

The ranking reported above -- three shaders at 81% of measured GPU time -- rests
on charging a control stream's firmware interval to a shader when every dispatch
in that stream came from it. Reading the two hottest shaders showed the
attribution cannot be right, and a direct count confirmed it.

### What the shaders actually are

**`2e4ccbe89b31`** (charged 29.3% of the frame). 32-thread workgroups, 4 SSBOs,
`uses_wide_subgroup_intrinsics`. Inside its loop:

    %15 = @ballot (%14)               // %14 = (subgroup_invocation == 0)
    %16 = ufind_msb %15
    %17 = @read_invocation (%11, %16)
    %28 = @load_agx (%27, %24)  readonly|reorderable|speculatable, r32_uint
    %29 = ushr %28, 2
    %30 = iadd %3, %29                // next address derived from this load
    %40 = @load_agx (%39, %36)

A dependent load chain in a loop, wrapped in the ballot/read_invocation idiom
vkd3d-proton emits for bindless `NonUniformResourceIndex`. Plausibly expensive
on its own merits: latency-bound with 118 workgroups of 32 threads on a 32-core
GPU, and nothing a compiler can fix -- full occupancy (1024 threads), no spills,
31 GPRs, 186 instructions.

**`e4919730b5b4`** (charged 17.6% of the frame) is a **buffer clear**:

    %10 = imadshl_agx %8, 0x30, %3.w, 0x0      // 48-byte stride
    if (%11 < %3.z) @store_agx (0x0, %14, 0x0) // store zero
    if (%16 < %3.z) @store_agx (0x0, %21, 0x0)

No loop, no loads, no subgroup ops. 352 invocations writing zeros -- on the
order of 16 KB. **It cannot execute for 23.67 ms.**

### Ruling out the obvious excuse first

If the firmware interval included time a command spends waiting rather than
executing, a trivial shader scheduled behind expensive work would be charged for
it. `tests/waittest.c` submits a heavy dispatch and a trivial one back to back
in separate control streams:

    case                              heavy ms   trivial ms
    trivial alone                        0.026        0.031
    trivial AFTER a heavy dispatch      11.873        0.018

The trivial dispatch does not inflate. **The interval measures execution.** So
that excuse is gone and the attribution itself is at fault.

### The actual defect

`HK_GPUTIME_ISOLATE` was supposed to give each dispatch its own control stream.
It does end the stream -- the `hk_cs` pointers differ per dispatch, verified with
a debug print -- but the streams do not survive as separate submitted commands.
Counted directly, with new counters now in the report:

    dispatch origin: app 1216  (recorded 1216)
    CDM commands built 13, timestamped 13, intervals harvested 12
    1216 dispatches (101.3/cmd)

**1216 dispatches, 13 timed commands.** Timing granularity is ~101 dispatches per
interval, so charging an interval to one shader charges it for roughly a hundred
other dispatches as well. That is exactly how a 16 KB buffer clear acquires
23.67 ms.

The `est%` column was already known to be unreliable; now the `meas%` column is
too, for a different reason. Neither ranking should be used.

### What survives

* Compute is ~90% of GPU busy time, and the GPU is ~90% busy. Unchanged.
* Cost does not scale with pixels: 60% fewer invocations, 4% less time.
* Structure is invariant across resolutions: 56 streams, ~1780 dispatches,
  ~2.4 ms per stream.
* A dispatch plus its cache flush costs ~2 us in isolation (`disptest.c`).
* A small dispatch with a data-dependent loop genuinely can cost milliseconds
  (`cstest.c`: 2881 us for 32 invocations at 100k iterations).
* `2e4ccbe89b31` really does contain a dependent-load loop. Its *share* is
  unproven; its *shape* is read directly from its NIR and is not in doubt.

### What is needed next

Find why per-dispatch control streams coalesce into ~101-dispatch commands. Until
then there is no per-shader timing on this driver, and the question "which shader
costs the frame" remains open. The counters that expose the problem are now in
the report permanently, so it cannot be mistaken for working again.

---

## THE ANSWER: register pressure and occupancy, in five named shaders

With `merge_control_streams` no longer defeating `HK_GPUTIME_ISOLATE`, every
dispatch is its own timed command and the attribution reconciles:

    CDM commands built 34091, timestamped 34016, intervals harvested 34018

    id   blake3         disp/fr   invoc/fr   est%  meas ms/fr  meas%  inst  gprs  occ  spill loop
    164  dbe39607fa5d     411.6     84,731   0.5%      41.85  30.6%  2839   255  384    272    0
    165  6fc0efe726d2      17.0  1,114,112   6.6%      19.19  14.0%  2724   255  384    272    2
    259  c0408dcabfa6     103.0     52,756   1.4%      15.33  11.2% 11251   183  576      0   17
    260  dc6351ad59d8      57.0      3,648   0.1%      11.17   8.2% 11250   183  576      0   17
    261  18a1bc3d2c5b      28.0     14,336   0.4%       5.92   4.3% 11389   183  576      0   30

**Five shaders are 68% of compute time**, and they share one signature:

* **Occupancy is capped by register pressure** at 384 or 576 of a possible 1024.
  The hardware can keep only 37-56% of its threads in flight, so there is far
  less parallelism than the machine has, and nothing to hide memory latency
  behind.
* The top two sit at the **255-GPR ceiling and spill 272 times each**, turning
  register traffic into memory traffic.
* The other three are ~11,000-instruction shaders carrying 17-30 hardware loops.

The static estimate ranked all five at under 7% combined. It cannot see spill
traffic or loop trip counts, which is exactly what these shaders are made of.

Both previous suspects are gone from the table, confirming they were artifacts
of merged attribution: `2e4ccbe89b31` (the dependent-load loop) and
`e4919730b5b4` (the 16 KB buffer clear that had been credited with 23.67 ms).

### Perturbation

Isolation cost frame rate: 4.40 fps against 6.0. Almost all of it is CPU-side --
1780 commands per frame instead of 56 -- because GPU compute per frame moved only
141 ms against 135.9 ms, 3.7%. The ranking is representative; the frame rate
during an isolate run is not.

### This vindicates the original instinct, which the estimate had talked me out of

The session opened by proposing occupancy and register pressure as the compiler
target. That was dropped when the static model said the workload ran at 5% of
peak ALU and therefore could not be instruction-bound. The model was wrong, and
measurement now points back at precisely occupancy and register pressure -- for
five specific shaders, identified by hash, rather than as a guess about the
workload as a whole.

### Next

Reduce register pressure on `dbe39607fa5d` and `6fc0efe726d2` (255 GPRs, 272
spills, occupancy 384). Levers worth measuring, in order:

* `AGX_MESA_DEBUG=demand` -- bound register allocation tightly to demand
  (`agx_register_allocate.c:1501`).
* Scheduler pressure heuristics -- `agx_compile.c:3431` disables the pressure
  scheduler under `nosched`, so the pressure-aware path is already the default
  and is what would need improving.
* `agx_max_registers_for_occupancy()` -- whether targeting a higher occupancy
  tier and accepting more spills is a net win at 255 GPRs is an empirical
  question, and `tests/cstest.c` now makes it a seconds-long one.

---

## AGX_OCCUPANCY: the occupancy/spill trade cannot be tested with an env knob

The five shaders holding 68% of compute time are all capped by register pressure
at 384-576 threads of a possible 1024, so the obvious question is whether giving
up registers -- and accepting more spilling -- to double the threads in flight is
a net win.

The allocator's policy (`agx_register_allocate.c:1492-1494`) is to take the
register demand, find the occupancy tier it lands in, and then use *all* the
registers that tier allows, to reduce live range splitting. It never tries to
reach a HIGHER tier. So `AGX_OCCUPANCY=<threads>` was added to cap `max_regs` to
what a requested occupancy permits.

**It crashed the game.**

    err:vulkan:vkCreateComputePipelines Exception 0xc0000005 in Unix call.
    Unhandled exception code c0000409          (stack buffer overrun)

Capping below the shader's actual `demand` asks the allocator to spill through a
path that is not prepared for it: the `force_spilling` branch higher in the same
function does extra work to guarantee room for preloaded and exported registers,
which a bare cap skips.

The knob is now clamped to `capped >= demand`, so it can only hand back
registers that tier-rounding granted, never force the extra spilling a genuinely
higher occupancy would require. **That makes it useless for the question it was
written to answer** -- on these shaders demand already sits at the ceiling.

Testing the occupancy/spill trade properly needs work in the spiller, not an
environment variable. That is a real piece of compiler work and should be
scoped as such rather than attempted as a flag.

### Harness: runs now have a hard ceiling

The crash cost twelve minutes of the harness clicking at a desktop, because the
per-stage timeouts compound -- 600 s waiting for a window plus 1200 s waiting for
gameplay is half an hour for a game that died at startup. Added:

* `RUN_TIMEOUT` (default 1500 s), a wall-clock ceiling on the entire run,
  checked in both waiting loops.
* An abort when the game window disappears after having appeared, which is what
  a crashed game looks like from outside.

Neither existed before; every previous failed run burned its full budget.

---

## VERIFIED BUG: every AGX SSBO load is wrongly marked lane-divergent

Found by analysing the NIR of `2e4ccbe89b31`, then confirmed directly in the
Mesa source. This is a one-line defect with a large blast radius.

### The chain

1. The game's SPIR-V contains `OpGroupNonUniformBroadcastFirst` (HLSL
   `WaveReadLaneFirst`) -- an **application** idiom, not driver-generated. Mesa
   lowers it to `ballot` + `ufind_msb` + `read_invocation`
   (`src/asahi/compiler/agx_nir_lower_subgroups.c:47-52`).
2. Mesa already has the optimisation that deletes such a broadcast when its
   source is uniform: `src/compiler/nir/nir_opt_uniform_subgroup.c:209` bails
   only on `nir_src_is_divergent`.
3. `src/asahi/compiler/agx_compile.c:2935` runs `agx_nir_lower_address`,
   rewriting `load_global` into `load_agx`, **two lines before**
   `nir_opt_uniform_subgroup` at line 2937.
4. `src/compiler/nir/nir_divergence_analysis.c:1123` lists
   `nir_intrinsic_load_agx` in the **unconditionally divergent** block, whereas
   `nir_intrinsic_load_global` (line 681) is divergent only if one of its
   sources is, and `nir_intrinsic_load_constant_agx` (line 856) is already in
   the source-dependent group.

The same load is therefore uniform as `load_global` and divergent as `load_agx`,
purely because of an opcode swap two lines earlier. **Every SSBO load in every
AGX shader is treated as lane-divergent**, so the broadcast elimination can
never fire on anything derived from one.

### Why the dump proves the value really is uniform

`nir_opt_preamble` uses a different, source-recursive test
(`nir_opt_preamble.c:253-255`: `load_agx` is movable if `ACCESS_CAN_REORDER` and
its sources are movable) and it succeeded -- the value feeding `read_invocation`
arrives as `@load_preamble(base=52)`, hoisted from a preamble `load_agx`. A value
in the preamble is by construction dispatch-uniform. Two passes disagree about
the same value, and the divergence one is wrong.

### Cost in the hot shader

The surviving `read_invocation` pins roughly 35 loop-invariant instructions --
including **two dependent SSBO loads** -- inside the hot loop, re-executed every
iteration. Nothing else rescues them: `nir_opt_licm` is never run anywhere under
`src/asahi/` (zero call sites).

### The fix

Move `nir_intrinsic_load_agx` from the unconditional block into the
`nir_intrinsic_load_global` case: divergent iff a source is divergent, plus the
existing `load_may_tear` check. `load_agx`'s sources are `(base64, offset)`; a
load from an address every lane agrees on returns the same value in every lane.

Not applied yet: the register-allocator agent is building in this tree, and a
divergence change alters every shader and would contaminate its evidence.

### Separately, from the same analysis

* `src/asahi/vulkan/hk_shader.c:342` carries
  `/* TODO: handle also the iadd(amul) pattern, this is important */`. Every
  offset in this shader is exactly that shape (`i*6+4` then `+ base`), so every
  load takes the slow byte-wise robustness path (3 ops + 3 bcsels) rather than
  the element-wise fast path at `hk_shader.c:328-339`. **Roughly a third of this
  shader's 152 ALU ops is bounds-clamping.**
* `hk_shader.c:422-427`: SSBO `load_global_bounded` always takes the expensive
  address-clamp path while `load_global_constant_bounded` can take the cheap
  `nir_bounds_agx` post-clamp. These loads are already marked `speculatable`, so
  the cheap path may apply. Unclear whether the restriction is deliberate.
* Two descriptor loads (`agx_nir_lower_texture.c:643-670` robustness size crawl,
  and `libagx_buffer_image_offset`) hit the **same 64-byte descriptor** and are
  not merged.
* The double indirection in this shader -- an index loaded from an SSBO then used
  as a descriptor-heap index -- is the **application's** GPU-driven bindless
  pattern. hk contributes exactly one level of indirection and it is
  preamble-hoisted, so the binding model is not at fault here.

---

## Feature level 11.0: hk qualifies for FL12_0, and it is not a performance lever

Settled by disassembling the vkd3d-proton the game actually loads -- **3.1.0,
build 7ad4937e28ea1a0**, at `Proton - Experimental/files/lib/wine/vkd3d-proton/
x86_64-windows/d3d12core.dll`, byte-identical to the copy in the prefix -- rather
than by recalling upstream. `d3d12_device_caps_init_feature_level` decodes to:

    11_1 <= OutputMergerLogicOp (= features.logicOp)
         && features.vertexPipelineStoresAndAtomics
         && maxPerStageDescriptorStorageBuffers >= 64
         && maxPerStageDescriptorStorageImages  >= 64
    12_0 <= 11_1 && TiledResourcesTier >= 2 && ResourceBindingTier >= 2
                 && TypedUAVLoadAdditionalFormats
    12_1 <= 12_0 && ROVsSupported && ConservativeRasterizationTier >= 1

Checked against observed `vulkaninfo` values, **hk satisfies every FL12_0
requirement** with the driconf active: logicOp true, vertexPipelineStores true
under the vkd3d profile, both descriptor limits 1048576, all sparse residency
bits for Tier 2, and all 18 typed-UAV-load formats carry both
`STORAGE_IMAGE_BIT` and `STORAGE_READ_WITHOUT_FORMAT_BIT`. `ResourceBindingTier`
is hardcoded to 3 in vkd3d and can never block.

FL12_1 is definitively out of reach and always will be: no
`fragmentShaderPixelInterlock` (ROVs) and no
`VK_EXT_conservative_rasterization` at all.

### Why "11.0" is still printed -- two reframings

* **DXVK reaches FL11_1 in the same process tree**, under the same FEX thunk and
  the same driver, dumping `logicOp : 1` and `vertexPipelineStoresAndAtomics : 1`
  across eight runs (`runs/*/steam-stdout.txt`). The FL11_1 inputs are genuinely
  true at the API boundary the game sees, so the thunk is not eating them.
* **The game's line may not be vkd3d's cap.** It prints whichever level from its
  own candidate list survives `CheckFeatureSupport(FEATURE_LEVELS)`. A list of
  {12_2, 12_1, 12_0, 11_0} against a real cap of 11_1 prints exactly "11.0". The
  executable is packed, so the list could not be recovered.

Best remaining hypothesis, at about even odds: the real cap is 11_1 and 12_0
fails on `TiledResourcesTier` landing at Tier 1, because hk fakes minmax only in
the Vulkan 1.2 core property (`hk_physical_device.c:913`, `fake_minmax`) while
deliberately not advertising `VK_EXT_sampler_filter_minmax`
(`hk_physical_device.c:201`).

### It does not matter for frame time

FSR is already initialising -- `ConvertScreenToRenderRes` runs and reports its
min/max -- so nothing is being refused on a feature-level check; the controller
simply never moves off maximum. 11_0 is above the game's own abort threshold.
And resolution scaling was already measured as near-irrelevant on this workload
(60% fewer pixels, 4% less time). Treat this as upstream hygiene.

### How to settle it for free

`VKD3D_FEATURE_LEVEL` accepts exactly 11_0/11_1/12_0/12_1/12_2 and forges the
caps backing the level, not just the label. **12_0 forges only things hk
genuinely backs**, so setting it on a run being done for another purpose costs
nothing and the game's own log line answers the question. **Do not go past
12_0**: 12_1 forges `ROVsSupported` and conservative rasterisation Tier 1, and hk
has neither, so a shader using ROVs would miscompile or fail.

Note this vkd3d build logs nothing explaining its choice -- the only string in
that function is `DX Ultimate supported!` on the 12_2 path -- so `VKD3D_DEBUG`
will not reveal the reason.

---

## The divergence fix is correct and does not move this game

                        fps   compute ms/frame
    baseline (drirc3)  5.98        135.9
    divergence fix     6.03        135.9

Identical. Frames verified correct. The bug is real -- on a synthetic shader
doing `subgroupBroadcastFirst` of a uniformly-loaded value it goes 23 -> 19
instructions, 17 -> 11 ALU, and the two ballot/read_invocation ops disappear --
but it buys nothing on this workload.

**Why, most likely:** removing the broadcast was necessary but not sufficient.
The broadcast was *pinning* ~35 loop-invariant instructions inside the hot loop;
with it gone, something still has to hoist them, and **`nir_opt_licm` is never
run anywhere under `src/asahi/`** (zero call sites). `nir_opt_preamble` only
takes values that are dispatch-uniform and reorderable. So the two dependent
SSBO loads plausibly still sit in the loop, merely without a broadcast in front
of them. Confirming that needs a re-dump of the shader to compare against its
previous "186 instrs, 152 alu, 1 loops, 107 preamble inst".

Keep the fix regardless: it is a genuine correctness/consistency defect in NIR
that made every AGX SSBO load falsely divergent, and it unblocks any future work
that depends on uniformity analysis.

## Feature level 11.0 is definitively a red herring

Forced `VKD3D_FEATURE_LEVEL=12_0`, which makes vkd3d report 12_0
unconditionally and forge the caps behind it. The game still logged:

    [D3D] Max supported feature level: 11.0

So that line does **not** report vkd3d's feature level cap. It comes from the
game's own candidate list or another path. Combined with the earlier findings --
hk satisfies every FL12_0 requirement, and DXVK reaches FL11_1 in the same
process tree -- the feature level is not a constraint on anything here and needs
no further work.

Cost of settling it: nothing. It rode along on a run being done for another
purpose, which is the right way to answer a low-value question.

---

## AGX_OCCUPANCY on the game: the knob works, and the trade is a loss

                        fps   compute ms/frame
    baseline           5.98        135.9
    AGX_OCCUPANCY=576  5.76        143.2

-3.7% frame rate, +5.4% compute time. Frames verified correct.

The knob is demonstrably doing its job on the real shaders --
`6fc0efe726d2` moved from 255 GPRs / 384 threads / 272 spills to
**183 / 576 / 290** -- so this is a genuine answer, not a no-op. On this
workload the extra spill traffic costs more than the extra threads in flight
hide. Leave the default alone; the allocator's existing policy is right here.

## The divergence fix transformed the hottest shader, and frame time still did not move

`dbe39607fa5d` is the shader that measured 30.6% of compute under sound
per-dispatch attribution. Its compiled form, before and after:

                instrs   gprs  threads  spills  preamble
    before        2839    255      384     272       107
    after          168     47     1024       0       398

A 17x reduction in main-shader instructions, register pressure down from the
255 ceiling to 47, occupancy at maximum, spilling gone entirely. The occupancy
cap was not binding (183 permitted, 47 used), so this is the divergence fix:
loads became uniform, hence preamble-eligible, and `nir_opt_preamble` hoisted
the loop-invariant work out. Exactly the predicted mechanism, and further than
predicted -- the earlier worry that "nothing will hoist them without LICM" was
wrong, because the preamble pass does it once uniformity is correct.

**And the frame rate is unchanged.** That is now a finding rather than a
non-result, and it means the earlier conclusion "the divergence fix does not
move this game" was drawn too quickly, from frame time alone, without checking
what it did to the code.

Two possibilities, and they need the sound instrument to separate:

1. That shader's real cost was never its instructions -- consistent with
   everything else measured today -- so removing 94% of them changes nothing.
2. Its cost genuinely fell and something else expanded to fill the frame.

The `occ576` run cannot answer this: isolation was off, so its per-shader column
is the merged attribution already retracted. Re-measuring with
`HK_GPUTIME_ISOLATE=1` and the fix in place.

---

## CORRECTION, and the cleanest result of the session

**The claim that the divergence fix transformed `dbe39607fa5d` (2839 -> 168
instrs) was wrong.** The 168-instr / 47-GPR / 1024-thread dump came from a
*different variant* of the same SPIR-V hash -- the game compiles several -- and
was compared against the profiler's row for the hot variant. A dump and a
profile row must be confirmed to describe the same compile before they can be
compared.

Sound per-dispatch attribution with the fix in place (`isolate3`,
36758 commands built / 36650 timestamped / 36649 harvested):

    shader          before fix (isolate2)      after fix (isolate3)
    dbe39607fa5d    2839 inst 255 gprs 384 thr 272 spill  ->  2708 / 255 / 384 / 272
    measured             41.85 ms/fr (30.6%)                    42.57 ms/fr (31.9%)

A 4.6% instruction reduction on that shader. Not 17x.

### What the run does establish, and it is stronger

                            estimated total   measured (single-shader streams)
    before fix (isolate2)      917.4 Gcycle              3420 ms
    after fix  (isolate3)      430.1 Gcycle              3607 ms

**The divergence fix cut estimated cycle demand by 53% across all ~297 live
shaders and measured time did not fall.** The non-isolated runs agree exactly:
135.9 ms/frame before, 135.9 ms/frame after.

This is no longer an inference from a static model that was already known to be
unreliable. It is a controlled experiment in which the ALU work genuinely
halved and the frame took exactly as long. **This workload is not ALU-bound**,
and the compiler-codegen avenue is closed: the hot ranking is stable
(dbe39607fa5d 31.9%, 6fc0efe726d2 14.4%, three ~11k-instruction shaders behind),
and making those shaders cheaper in instructions demonstrably does nothing.

### Consequence for what to do next

Every candidate has now been eliminated by measurement rather than argument:
ALU throughput, instruction count, pixels/resolution, dispatch count, cache
flushes, register occupancy, feature level. What remains by subtraction is the
memory system -- and it has never been measured, not once. Measuring achieved
bandwidth and dependent-load latency directly is the next step, and it either
produces a target or says the workload is simply heavier than this GPU.

---

## RESULT: 5.98 -> 12.43 fps by letting independent dispatches overlap

                      fps    compute ms/frame   GPU busy
    baseline         5.98         135.9           91%
    overlap 0x1f    12.43          53.1           82%

**2.08x frame rate, 2.56x less compute time**, over 27 windows and 1736 frames.
Frame verified visually and statistically. Setting:
`HK_PERFTEST=overlap AGX_CDM_BARRIER_MASK=0x1f`.

### How it was found

Everything else had been eliminated by measurement, so the memory system -- never
measured -- was all that remained:

    dependent load latency   428 ns (16 KiB working set) .. 905 ns (64 MiB)
    threads to saturate      ~4096 (7.4x throughput from 64 -> 4096 threads)
    the game supplies        ~213 invocations per dispatch, 412 dispatches/frame

So the machine needs an order of magnitude more threads in flight than any one
of this game's dispatches provides, and the only way to get them is to run
several dispatches at once. `dispatch()` was issuing every one with
`AGX_BARRIER_ALL`, which serialises them. Same work, no barriers between:
one dispatch of 64 groups took 2.66 ms, 64 dispatches of 1 group took 17.20 ms.

### Why the first four attempts hung

`AGX_BARRIER_NONE` emits **no barrier block at all**. The command stream needs
the block for sequencing even when no cache maintenance is wanted, so omitting
it hung the GPU -- seen as `XIO: fatal IO error 110 (Connection timed out)` from
the session's X server, with no GPU fault in the kernel log.

The fix is a barrier that is *weakened*, not *omitted*: `hk_cdm_barrier_masked()`
emits a real barrier block with a chosen subset of the 23 cache bits.
`agx_cdm_barrier()` sets all of them after every launch, under a comment
admitting the bits are not understood and this is "to be safe"
(`libagx_dgc.h:372`).

### What the bits do, measured

Narrowing to one dependency pattern -- compute writes a buffer, compute reads
it, nothing else, no barrier between (`tests/coherence.c`) -- and sweeping:

    mask       coherency (300 trials)      64 small dispatches
    0x1fffff   PASS                        17.2 ms   (all bits, the default)
    0x88       PASS  (minimal sufficient)  17.3 ms
    0x9f       PASS                        17.1 ms
    0x1f       FAIL 300/300                 3.5 ms
    0x8        --                           3.5 ms
    0x0        FAIL 60/60, 7616/8192 stale  0.37 ms

Greedy reduction from all-bits gives a **minimal sufficient mask of 0x88** for
that pattern: bit 3 (`USC cache inval`, the only field genxml names) and bit 7.
No single bit is individually required -- the bits are redundant -- so this had
to be found by cumulative removal, not one-at-a-time.

The bits separate by role:

* **bit 3 (+4): descriptor/uniform state.** Omitting it is what hung the GPU.
  Required between *any* two dispatches.
* **bit 7: data coherency, and it serialises.** Required only between
  *dependent* dispatches.

That is why `0x0` hangs and `0x1f` does not, and why `0x1f` is both fast and
"incoherent" by the test above.

### Why failing that coherency test is not a defect

`coherence.c` deliberately does what Vulkan forbids: a producer and a consumer
dispatch with no barrier between them. Dispatches the application has not
separated with a barrier are *required* to be independent, and independent
dispatches need no coherency. So a mask that fails that test is still correct
for the case it is used in -- and `hk_CmdPipelineBarrier2` ends the control
stream when the application does ask for ordering, while every driver-internal
helper kernel and every indirect dispatch still gets the full barrier.

### Conformance

Same driver, both configurations, failure sets compared case-by-case:

    suite                                 passed   base fails   overlap fails
    dEQP-VK.memory_model.*                  2218      30           the same 30
    dEQP-VK.synchronization.op.single_queue 2353       0            0
    dEQP-VK.compute.*                      10736       0            0

~15,300 tests, no regression. The 30 `memory_model.message_passing.*` failures
are pre-existing on this driver. `memory_model` and `synchronization` are
precisely the suites that would catch a missing cross-dispatch flush.

### Residual risk, stated plainly

The load-bearing assumption is that neither the game nor vkd3d-proton relies on
ordering it never requested. CTS cannot test that -- undefined behaviour is
undefined. The evidence for it is 1736 frames rendering correctly plus ~15,300
conformance tests unchanged; the evidence against it is that a correct-looking
screenshot is weak proof. The bit meanings remain unknown; `0x1f` is empirical.

---

## Item 41 — the mask sweep: `0x1f` was never actually overlapping

### Why look again

STATE.md's next-step list said "`0x1f` is the first thing that worked, not the
minimum. Bits 0-2 and 4 are unexamined individually." The starting evidence was
a single line in the old bit table: `0x0` cost 0.37 ms where `0x1f` cost 3.5 ms
on the same case. A 10x gap sitting behind the chosen default is not a rounding
error, so it was worth an hour.

### The sweep

`tests/cstest.c CSTEST_CASE=64,1,100000`: 64 dispatches of ONE workgroup, no
dependency between them, each doing a long serial loop. One workgroup cannot
fill the GPU, so this case measures precisely one thing -- whether consecutive
dispatches may run at the same time.

Masks `0x1f`, `0x18`, `0x10`, `0x08` all cost 37.18 ms; `0x0` cost 2.89 ms. So
the choice among bits 0-4 changed nothing at all, and the entire gap was
between "some of these bits" and "none of them".

Sweeping all 21 bits individually (full table in `data/barrier-bit-cost.md`)
split them cleanly:

    free      bits 7, 17, 18, 19          2.891-2.894 ms   (floor is 2.892)
    cheap     bits 1, 2, 0                4.2 - 7.4 ms
    costly    bits 4, 5, 6, 10-16         37.2 ms
    worse     bit 3                       48.5 ms
    worst     bits 8, 9                   183 ms

Two documented claims died here, and both had been *inferred from the game*
rather than measured:

  - "bit 7 is the data-coherency bit and the one that serialises." Bit 7 is
    free. It serialises nothing.
  - "bits 3 and 4 are descriptor state, required between any two dispatches;
    omitting them hangs the GPU." `0x80` omits both and the game runs fine.

### Which means the headline explanation was wrong too

`0x1f` is as serialised as the full mask on the case above (37.2 vs 188 ms,
against a 2.89 ms floor). So the 2.08x that `0x1f` delivered on the game was
NOT "letting independent dispatches overlap", which is what STATE.md claimed.
It was making each barrier about 10x cheaper while still serialising.

Separating the two costs took a third shape of test:

    case                                   0x0     0x80    0x1f     all
    64 tiny dispatches, long serial work   2.89    2.89    37.2    188.4
    64 dispatches that each fill the GPU  43.1    39.6    45.7    183.5
    64 empty dispatches                    0.042   0.038   0.064    0.251

Row 3 isolates fixed per-barrier cost (~0.3 us at `0x1f`, ~3.3 us at the full
mask). Row 1 isolates loss of overlap. The full mask pays both; `0x1f` paid
only the second; `0x80` pays neither.

### A test that failed to reproduce the hang

`tests/statetest.c` was written to catch the descriptor-state hazard that
`cstest.c` structurally cannot see -- cstest binds one descriptor set once and
dispatches the same pipeline 64 times, so nothing can go stale. statetest gives
each of 64 dispatches its own buffer, its own push constants and alternates
between two pipelines, with no dispatch reading another's output.

It passes at every mask down to `0x0`, 100 trials each. So it does NOT
reproduce the original hang either, and the honest conclusion is that the cause
of that hang was never identified -- it was almost certainly the missing
barrier block or the unsettled deferred flush from the earlier attempts, not
the mask. Recorded rather than quietly dropped, because a test that fails to
reproduce a bug is evidence about the bug.

### On the game

`runs/mask80`, `AGX_CDM_BARRIER_MASK=0x80`:

    12.39 fps -> 14.8 fps      (+19%)
    compute 53.5 -> 39.5 ms/frame   (-26%)
    GPU busy 82.0% -> 78.6%

Frame captured mid-run and checked: village at dusk, banners, torches, lanterns,
158,713 colours, no corruption.

Note the harness reported "still in a light window" throughout: its gameplay
gate is `fps <= 15`, and the game is now faster than that. The gate has been
widened. The measurement itself is unaffected -- 57 compute streams per frame
matches gameplay exactly, and the menu runs at 52 fps with 8.7 ms of compute.

### And the safety argument got better, not worse

While looking for what the bits do, the more useful question turned out to be
what can share a control stream. Answer: `vkCmdPipelineBarrier2`,
`vkCmdSetEvent2`/`WaitEvents2`, every draw and every query operation all end
the compute stream. So dispatches sharing a stream are independent *by the
Vulkan execution model*, not by luck -- and independent dispatches need no
coherency. The driver's own dispatches, which do have unexpressed
dependencies, all still pass `AGX_BARRIER_ALL`.

That replaces "empirical, and undefined behaviour is undefined" with a real
argument. The residual risk is now specific and checkable: it depends on hk
continuing to end the stream at every one of those points, which nothing
enforces.

---

## Item 42 — reading the hot shader, and three negative results

### The dump tool was broken in two ways

`AGX_DUMP_SHADER` printed `=== match ===` and then no shader. Cause: a race.
`agx_compile_shader_nir()` reassigns the GLOBAL `agx_compiler_debug` on entry,
and the targeted dump ORs `AGX_DBG_SHADERS` into it; with hundreds of pipelines
compiling in parallel the next thread to enter wiped the flag before
`nir_print_shader()` was reached. Fixed by keeping the requested bits
thread-local (`4877ecd2780`).

With that fixed the dumps arrived and immediately interleaved, splicing
mid-line:

    32  %6 = imadshl_agx %4.x, %5 (0x40), %0.x, shader: MESA_SHADER_COMPUTE

Fixed with a lock across the whole compile of a matched shader (`736ef7a3e5f`).

Worth noting: the earlier shader analysis in item ~38 was produced through this
broken path.

### What the hot shader does

`6fc0efe726d2`, 8x8 workgroup, 4 SSBOs, texture_gather, wide subgroup
intrinsics, and **2560 bytes of scratch declared by the shader itself**. It
opens by storing a constant lookup table into that scratch, 16 bytes at a time:

    32x4 %4 = vec4 %1 (0x97), %2 (0xa0), %3 (0x89), %0 (0x5b)
              @store_scratch (%4, %5 (0x0))

160 such stores = 2560 bytes exactly, then 44 dynamically-indexed
`load_scratch`. In the final code: 514 `stack_store`, 244 `stack_load`.

Scratch is per-invocation. ~1.38 M invocations per frame each build a private
copy of the same constants.

`nir_opt_large_constants` is the pass for this and is commented out in
`hk_shader.c:841`. The CPU half of the plumbing exists; the GPU half does not.
See `data/compute-breakdown.md`.

### Three things that did NOT work, measured

**Compute-only barriers in-stream.** 465 pipeline barriers per frame, of which
24.3 are compute-only and end a stream containing work -- against ~52 compute
streams per frame, so on paper worth nearly half of them. Implemented as
`HK_PERFTEST=csbarrier` and run:

    control    16.88 fps   compute 35.0 ms/frame   51.7 compute streams/frame
    csbarrier  16.95 fps   compute 35.2 ms/frame   51.8 compute streams/frame

No effect, and the stream count explains why: skipping the end leaves the
stream open only until the next draw, which ends it anyway. The barrier was
never the thing creating the stream boundary.

**The vsync hypothesis.** GPU idle is sharply bimodal -- ~220 small gaps per
frame totalling 5.6 ms, and ONE gap of ~8.8 ms, with nothing in between. 8.8 ms
is one vblank at 120 Hz and 15.00 fps is 120/8, so vsync looked compelling.
`MESA_VK_WSI_PRESENT_MODE=immediate` moved nothing: 16.49 vs 16.88 fps, 81 vs
80 big gaps, 8.51 vs 8.79 ms mean. The override did apply (wrapper reports 5
env vars forwarded against 4). Evidence that should have been weighted earlier:
single gaps reach **13.6 ms**, and a vblank wait cannot exceed 8.333 ms.

So there is an ~8.5 ms per-frame stall -- 14% of the frame -- that responds to
neither GPU work nor present mode. Most likely the game's CPU work under FEX.
Not settled.

**More dispatch concurrency.** `tests/concurrency.sh`, min of 5 runs per point:
total time is flat at ~4 ms from 2 through 64 dispatches, then 8.8 ms at 128
and 11.0 ms at 256. The GPU runs ~64 independent single-workgroup dispatches at
once, and the game's ~414 small dispatches per frame already have that width.
This avenue is spent.

### Where that leaves the frame

    59.2 ms total
      45.6 ms  GPU busy       <- addressable
       5.6 ms  drain gaps     <- addressable, but csbarrier did not touch it
       8.5 ms  non-GPU stall  <- not addressable from the driver

### A methodological correction

The same driver measured 14.72 fps (`runs/mask80`) and 16.88 fps
(`runs/clean80`). Same save, same navigation; the camera ends up elsewhere and
this game's cost tracks what is on screen. Differences under ~15% in fps are
not evidence. Compute-ms-per-frame is far less scene-sensitive and should carry
the argument; the big claims here (129.5 -> 53.5 -> 35.0 ms) do.

---

## Item 43 — constant tables out of scratch, and a measurement method that finally holds

### The change

`nir_opt_large_constants` enabled, plus the GPU-side plumbing it needed
(`6a71e0feba7`). Two things had to be fixed, not one:

1. **Order.** `agx_preprocess_nir()` lowers anything over 256 bytes to scratch,
   and runs BEFORE `hk_lower_nir()` where the commented-out call sat. Simply
   uncommenting it would have found nothing left to move. It now runs in
   `hk_preprocess_nir()` just ahead of that, with copies lowered first (the
   pass asserts on `copy_deref`).
2. **Nowhere for the data.** `hk_shader.c` already captured `nir->constant_data`
   and serialised it into the pipeline cache, but `hk_upload_shader()` only
   uploaded the binary. The section now goes to its own BO, and the
   `load_constant` intrinsics are lowered to global loads based at an address
   read from a uniform slot past the root descriptor and descriptor sets,
   bound once at fast-link time.

**Bug worth remembering:** a USC uniform packet loads the CONTENTS at an
address into uniform registers, not the address. Passing the data address
directly loaded the table into uniform registers instead of a pointer to it,
and the shader read nonsense. The pointer now lives in the same BO just past
the data -- the self-referential trick the root descriptor table uses.
`tests/bigconst.c` caught it on the first run, which is the whole reason it was
written before the game was touched.

### Effect on the shader that started this

`6fc0efe726d2`, identical 17.0 dispatches and 1,114,112 invocations per frame
in both runs, so this is the same work either way:

    measured ms/frame   4.24  ->  0.22      (19x)
    scratch             3104  ->  0
    spills:fills      194:78  ->  0:0
    GPRs                 255  ->  119
    occupancy      384 thr    ->  832 thr
    instructions        2540  ->  1974

The register pressure collapsing was not the goal and is the more interesting
half: the 194 spills were the constant table's addressing, not an inherent
shortage. So extending `can_remat()`, which was item 2 on the list, is largely
moot for this shader. Across all profiled shaders, scratch fell from 3232 bytes
in 3 shaders to 116 bytes in 1.

### A measurement method that stops needing repair

The gameplay gate was `fps <= 15 && >= 50 ms compute/frame`. It had to be
widened to `<= 25 && >= 20`, and then the very next run tripped it again at
21.0 fps / 19.3 ms -- reported as "2 windows, 13.75 fps" for a run that was
actually the best yet. Widening a threshold that moves every time the driver
improves is not a fix.

The gate is now **compute control streams per frame** (`>= 35`). That is a
property of what is being drawn, not of how fast it is drawn, so it does not
drift as the driver improves. Measured distribution: menu 8-23, gameplay 43-58,
nothing in between.

The same number also identifies the SCENE, which turned out to matter more.
Bucketing by it showed `runs/baseline2` contains two different places:

    44 streams/frame  x82 windows   16.93 fps   40.8 ms compute
    52-58 streams/f   x23 windows    5.84 fps  134.4 ms compute

Every earlier headline compared the heavy scene of one run against whatever
the next run happened to sample. The summary now prints a per-scene breakdown
so that cannot happen silently again.

### Like-for-like results

Heavy scene (55-58 streams/frame):

    driver                       fps    compute ms/frame
    before this work            5.84    134.4
    weak CDM barrier 0x80      15.85     36.0
    + constant data            21.01     19.2

Lighter scene (44 streams/frame): 16.93 -> 19.04 -> 27.77 fps, compute
40.8 -> 33.0 -> 16.1 ms.

3.6x frame rate and 7.0x compute time on the heavy scene, over 22,144 frames.

### What this invalidates

Compute was 76.7% of GPU busy time and is now 61.7%, with fragment up to 13.5%
of wall and 17.3% for vertex. The premise this whole investigation ran on --
"compute is ~80-90% of GPU time, so compute is the problem" -- no longer holds.
Anything that comes next should re-rank from measurement rather than inherit
it.

---

## Item 44 — a cache bug that faked a driver regression, and two dead hypotheses

### The preamble hypothesis, and why it was wrong

`a30540657d3d`, the top compute shader after the constant-data work, has a
preamble BIGGER than its body: 2976 instructions against 2196, containing 199
memory loads (177 `load_agx`, 22 `load_constant_agx`) filling 209 uniform
registers. AGX runs the preamble on a single thread before the body.

The hypothesis: `nir_opt_preamble` hoists on the assumption that preamble work
is amortised across many invocations, and this shader is dispatched with only
843 -- 13 workgroups. 199 serial loads at the measured 400 ns dependent-load
latency would be ~80 us, against a measured 76.8 us per dispatch. A suspiciously
good fit.

It is wrong. `tests/preambletest.c` -- 96 hoistable uniform loads, trivial body,
sweeping workgroup count, min of 5:

    workgroups   with preamble   without
             1        0.535 us   0.528 us
           256        3.950 us   4.856 us

The preamble costs essentially nothing even at ONE workgroup, and only ever
helps at scale. The 199 loads are independent and pipeline; they are not a
dependent chain, so the 80 us arithmetic was fantasy. Two hypotheses died here
-- that the preamble is expensive, and that it is expensive specifically for
small dispatches.

### The bug the experiment exposed

`AGX_MESA_DEBUG=nopreamble` on the game measured 18.28 fps / 25.93 ms of
compute against 22.15 / 18.02 for the run before it. Reported as a 44%
regression, then retracted when its own control `prectl` came in at
18.44 / 26.03 -- apparently showing nopreamble made no difference.

Both readings were wrong, because `prectl` was not a control.

`hk_physical_device_compiler_flags()` mixes `pdev->dev.debug` and the occupancy
target into the shader cache key. `pdev->dev.debug` is **ASAHI_MESA_DEBUG**.
The compiler debug flags are **AGX_MESA_DEBUG**, a different variable, and were
not in the key at all -- though several of them change generated code:
nopreamble, noopt, nosched, spill, nopromote, demand.

So the one nopreamble run wrote preamble-less binaries into the cache under the
ordinary key, and every later run loaded them. `prectl`, `warm1` and `warm2`
were all silently running nopreamble code.

What exposed it was comparing PER-SHADER STATISTICS between two runs that
should have been identical:

    6fc0efe726d2   noxoverlap2: 1974 instrs, 119 gprs, occupancy 832
                   warm1:       3964 instrs, 207 gprs, occupancy 512

Same shader, same invocation count, different code. A scene difference cannot
do that. Frame-rate numbers alone would never have shown it -- they looked
exactly like a plausible regression from the preceding commits, and the next
step would have been bisecting driver changes that were not at fault.

So `nopreamble` IS a ~44% compute regression, the first reading was right, the
retraction was wrong, and preambles matter a great deal -- consistent with the
isolation test above.

### Fixed

`c72c9e345aa` puts `agx_get_compiler_debug()` in the cache key. The 1.2 GB
cache was cleared, since the fix cannot retroactively invalidate entries
written under the unchanged default key.

**Methodological consequence:** before this fix, ANY experiment using
AGX_MESA_DEBUG silently poisoned every subsequent run on the machine. Several
earlier items in this file used `AGX_MESA_DEBUG=shaders`; those were dumps
rather than codegen changes, so their conclusions stand, but the hazard was
live for the whole project.
