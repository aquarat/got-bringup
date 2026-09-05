# Ghost of Tsushima on Honeykrisp — driver bring-up and measurement

Making *Ghost of Tsushima DIRECTOR'S CUT* run efficiently on an Apple M1 Max
under Fedora Asahi Remix, through muvm + FEX + Proton + vkd3d-proton on the
**Honeykrisp** Vulkan driver.

**Result: compute time per frame reduced ~4.1x**, from two driver changes in
Mesa. See `STATE.md` for where things stand and `data/per-fix-results.md` for
what each change is worth on its own.

| fix | fps | compute |
|---|---|---|
| Dispatch overlap (weak CDM barrier) | +34.5% | 2.17x |
| Constant tables out of per-invocation scratch | +30.1% | 1.88x |

The driver changes live in a separate repository (a Mesa fork); this one holds
the measurement harness, the tests, and the written record. `mesa-source.env`
names that fork and pins the exact commit every number here was produced from.

## Just want the faster driver?

**[INSTALL.md](INSTALL.md)** — an RPM for Fedora Asahi Remix, built from that
pinned commit by `.github/workflows/build-driver.yml`. Installing it changes
nothing; `honeykrisp-got enable` is a separate step, because reaching a Steam
game means replacing the Vulkan driver your compositor is also using. Read the
first section of INSTALL.md before you do it.

Or build it yourself: `./build-driver.sh`, then `./deploy-system-driver.sh deploy`.

## Read this before trusting any number

`data/measurement-hazards.md` is the most useful thing here. Three hazards
silently corrupted measurements during this work, two of which produced
conclusions that had to be retracted:

* **The game's dynamic resolution moves render resolution over a 4x range** to
  chase 30 fps, so a driver improvement can be absorbed as extra pixels at
  constant frame rate. Frame rate is not a measure of GPU efficiency here
  unless resolution is pinned.
* **`AGX_MESA_DEBUG` was missing from the shader cache key**, so one run with a
  codegen-affecting flag poisoned every later run.
* **`HK_PERFTEST` was missing too** — an ablation reported a pass as worth 1.5%
  when it had not been disabled at all.

With resolution pinned the harness reproduces to **0.6% on fps and 0.08% on
compute time**, against roughly 15% before.

## Layout

| path | what |
|---|---|
| `INSTALL.md` | how to get the driver onto a machine that is not this one |
| `STATE.md` | where the work stands; start here |
| `data/` | the analysis: per-fix results, measurement hazards, what is left |
| `CHECKLIST.md` | the full record, including every wrong turn and why it was wrong |
| `mesa-source.env` | the Mesa fork, branch and commit the driver comes from |
| `build-driver.sh` | clone that commit and build the driver from it |
| `packaging/` | the RPM: spec, the `honeykrisp-got` tool, the CI build |
| `autorun.sh` | unattended measurement run |
| `reset-session.sh` | tear down a wedged muvm/Steam session |
| `tests/` | standalone Vulkan tests that isolate one question each |
| `runs/*/reports.txt` | the raw evidence behind every number quoted |

## Tests

Each is a small standalone Vulkan program answering one question, built with
`cc -O2 -o NAME NAME.c -lvulkan`:

| test | question |
|---|---|
| `coherence.c` | which CDM barrier bits give coherency between dependent dispatches |
| `cstest.c` | dispatch cost and overlap, the fastest driver A/B available |
| `drawcost.c` | what an empty draw costs (use degenerate triangles) |
| `preambletest.c` | what a shader preamble costs against workgroup count |
| `bigconst.c` | does `nir_opt_large_constants` fire, and are its results right |
| `robtest.c` | does robust buffer access still return zero out of bounds |
| `memtest.c` | dependent-load latency, bandwidth, latency hiding |
| `concurrency.sh` | how many dispatches the GPU runs at once |

## What is deliberately not here

`runs/` keeps only `reports.txt` and `summary.txt`. The raw Steam and Proton
logs, screenshots and a full Wine registry backup are excluded: together they
were 730 MB, and they carried a Linux username in ~2250 paths, a private LAN
address, a persistent game-telemetry client ID, and — in the registry dump — a
speaker's MAC address and the hostnames and IPs of a home network. None of it
is needed to reproduce anything here.
