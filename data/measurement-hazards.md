# Measurement hazards on this workload

Three things silently corrupt measurements here. All three were hit during this
work; two produced conclusions that had to be retracted.

## 1. The game's render resolution varies, within and between runs

This is the big one, and it invalidates more than scene variance does.

Shader `8c4c1c4aeb67` runs one dispatch per frame at one invocation per pixel.
Its invocation count across runs:

    clean2     576000 ... 2304000     (960x600 up to 1920x1200)
    fragattr   702915 ... 2314521     (never dropped to quarter res)

2304000 is exactly 1920x1200; 576000 is exactly 960x600. **Dynamic resolution
IS engaging** and moving continuously, between and within runs.

An earlier item in CHECKLIST.md concluded the opposite -- "dynamic resolution
is enabled and never engages", "the render resolution is pinned at native for
the entire session" -- on the evidence that shader after shader launched
exactly 2,304,562 invocations. That was true of that session and is not true in
general. **Treat that item as superseded.**

Consequence: comparing `compute ms/frame` between two runs compares two
different pixel counts. `runs/fragattr` reads 24.8 ms against `runs/clean2`'s
17.9 ms for this reason alone -- it stayed at high resolution while clean2
spent time at quarter.

**What to do instead:** compare a NAMED SHADER at a MATCHED INVOCATION COUNT.
Same SPIR-V hash, same invocations, and then instruction counts, register
usage and measured milliseconds are all comparable. That is what exposed the
shader-cache bug, and what showed the robustness change did nothing here.

## 2. AGX_MESA_DEBUG was not in the shader cache key

Fixed in `c72c9e345aa`. Before that, one run with
`AGX_MESA_DEBUG=nopreamble` wrote preamble-less binaries into the cache under
the ordinary key and every later run picked them up, looking exactly like a
44% driver regression. See CHECKLIST item 44.

## 3. HK_PERFTEST was not in the shader cache key either

Fixed in `551b3415ffe`. Same bug as hazard 2, one variable over: an ablation
reported a pass as worth 1.5% when the flag had not disabled it at all.

## 4. Scene content varies even at a matched stream count

`runs/norobust` and `runs/robctl`, both restricted to 44 compute streams per
frame, differ by 22.56 against 29.07 fps. The stream count identifies the scene
well enough to separate menu from gameplay, and not well enough to compare
frame rates a few percent apart.

## The rule that follows

The harness can measure a 2x change. It cannot measure a 5% change.

And even a matched resolution is not a matched scene: the menu is pinned too,
and "gameplay" spans at least two scenes in which the same fix can differ by
3x. Filter on resolution AND streams-per-frame, then report per scene.

For anything smaller, do not use frame rate or per-frame milliseconds. Use:

* **compile-time statistics** -- instruction count, GPRs, occupancy, spills,
  scratch -- which are deterministic and free of all three hazards; or
* **a named shader at a matched invocation count**, which is immune to
  resolution and scene; or
* **a synthetic test** (`tests/*.c`), which controls everything.

Every conclusion in `STATE.md` marked as measured rests on one of those, or on
a change large enough that these hazards cannot account for it.

---

## The mechanism, confirmed, and how to switch it off

From the game's own log:

    [Settings] Dynamic resolution FPS target: 30
    [Settings] Upscale method: FSR
    [Settings] Upscale quality: Dynamic
    [Upscaler] ConvertScreenToRenderRes: wxh = 1920x1200 | min = 960x600 | max = 1920x1200

min 960x600 is 576,000 pixels and max 1920x1200 is 2,304,000 -- exactly the
range of invocation counts observed for the one-invocation-per-pixel shader
`8c4c1c4aeb67`. So the controller is live, it targets 30 fps, and the game
never reaches that target, so it has authority to move resolution across a 4x
range at will.

**This means frame rate is not a measure of GPU efficiency on this workload.**
A driver change that makes the GPU faster can be absorbed entirely as extra
pixels at a constant frame rate. That is not hypothetical here:

| run | fps | compute ms/f | invocations/f | ns/invocation |
|---|---|---|---|---|
| `noxoverlap` | 22.40 | 18.0 | 584,899 | 30.82 |
| `xoverlap` | 22.56 | 18.0 | 659,307 | 27.26 |
| `noxoverlap2` | 22.15 | 18.0 | 585,535 | 30.77 |
| `xoverlap2` | 22.21 | 18.3 | 740,525 | 24.77 |

Two independent pairs: identical frame rate, identical compute time, and
13-27% more pixels with subqueue overlap enabled.

**SUPERSEDED.** This was read at the time as a real gain the frame rate had
hidden. Measured again at pinned resolution it vanishes -- see
`per-fix-results.md`. The extra pixels were the resolution controller landing
somewhere different between runs, i.e. correlation, not causation. Subqueue
overlap is worth nothing measurable on this workload and is default OFF.

### Switching it off

The setting lives in the Wine registry at

    .../compatdata/2215430/pfx/user.reg
    [Software\\Sucker Punch Productions\\Ghost of Tsushima DIRECTOR'S CUT\\Graphics]
    "UpscaleQuality"=dword:00000003     <- 3 is Dynamic
    "DynamicResolutionTargetFPS"=dword:0000001e

Setting `UpscaleQuality` to a fixed quality level pins the render resolution
and makes frame rate meaningful again. It can also be changed in the game's own
Options menu (Upscale Quality: anything other than Dynamic). A backup of the
original values, and how to restore them, are recorded in
`game-settings-pinned.md`. The full registry backup is deliberately kept OUT
of this repository: a Wine `user.reg` is a whole-desktop dump, and the one
taken here carried a speaker's MAC address, eight LAN hostnames and ten
private IPs that have nothing to do with the graphics driver.

Until that is done, **every A/B in this project should be read as
ns-per-invocation, not fps**.
