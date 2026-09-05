# Installing the patched driver on Fedora Asahi Remix

The driver cuts *Ghost of Tsushima DIRECTOR'S CUT*'s GPU compute time per
frame on an M1 Max from **102.8 ms to 18.6 ms on the heaviest scene measured**,
and by less on lighter ones. Everything behind that number, including the
measurement hazards that invalidated earlier figures (an older "4.1x" among
them), is in `STATE.md` and `data/per-fix-results.md`.

This page is how to get it onto your machine.

## Before anything else: what you are agreeing to

This is a development build of Mesa, from a fork, carrying changes that are not
upstream. To reach a Steam game it has to replace
`/usr/lib64/libvulkan_asahi.so`, which is **also the driver your desktop
compositor uses**. A driver that is subtly wrong can take your session down.

That is not hypothetical caution, it is the actual trade. In exchange, the risk
is bounded:

* `honeykrisp-got enable` refuses to deploy a driver that cannot enumerate the
  GPU, before it touches anything.
* The distro driver is kept as a plain file copy, so recovery needs no working
  GPU at all: **Ctrl-Alt-F3, log in, `sudo honeykrisp-got disable`**.
* The swap is a rename, not a rewrite, so applications already running keep the
  driver they started with.

Nothing is replaced by installing the package. `enable` is a separate,
deliberate step.

## Requirements

* Apple Silicon (M1/M2 family) running **Fedora Asahi Remix**. `aarch64` only.
* `mesa-vulkan-drivers` installed — that is the file that gets backed up.
* For the game: muvm, FEX, Steam, Proton and vkd3d-proton, as Fedora Asahi
  Remix normally sets them up.

## 1. Install the package

From the dnf repository, so that `dnf upgrade` carries the driver forward
instead of leaving you on whatever you downloaded once:

    sudo dnf config-manager addrepo \
        --from-repofile=https://aquarat.github.io/got-bringup/honeykrisp-got.repo
    sudo dnf install honeykrisp-got

On Fedora 40 and older that is `dnf config-manager --add-repo <url>`. Either way
it just writes `/etc/yum.repos.d/honeykrisp-got.repo`, so
`sudo curl -fsSLo /etc/yum.repos.d/honeykrisp-got.repo <url>` does the same
thing with nothing installed.

Or grab a single `honeykrisp-got-*.aarch64.rpm` from
[Releases](https://github.com/aquarat/got-bringup/releases):

    sudo dnf install ./honeykrisp-got-*.aarch64.rpm

Or let the script do whichever of those is available. Read it first — it
installs a driver:

    curl -fsSLO https://raw.githubusercontent.com/aquarat/got-bringup/main/install.sh
    less install.sh
    bash install.sh

### Are the packages signed?

Whether the published packages are signed depends on whether a packaging key has
been configured; `packaging/SIGNING.md` explains how, and the repository's
[index page](https://aquarat.github.io/got-bringup/) states plainly which it is.
If they are unsigned the `.repo` file sets `gpgcheck=0` and `dnf` will tell you
so at install time. That is not a formality — nothing then proves a package came
from this pipeline rather than from whoever served it to you.

## 2. Check it before deploying it

    honeykrisp-got test

This runs `vulkaninfo` against the packaged driver only, through its own ICD
manifest, and touches nothing. You should see an Apple GPU and a `driverInfo`
naming a Mesa 26.3 development build.

## 3. Deploy it

    sudo honeykrisp-got enable
    honeykrisp-got user-config      # as your normal user, NOT root

`enable` swaps the driver. `user-config` writes the Honeykrisp vkd3d/DXVK
driconf profile into `$HOME/.drirc`, which is not optional: without it the game
reports `[D3D] Max supported feature level: 11.0` and refuses to start. The
options exist upstream in `src/asahi/vulkan/00-hk-defaults.conf`, but the driver
only looks for that file under its own `DATADIR`, and inside the pressure-vessel
mount namespace the game runs in, that directory is not visible. `$HOME` is
shared through into muvm and `$HOME/.drirc` is parsed unconditionally, so that
is where it has to go.

## 4. The FEX Vulkan thunk

**Without this the driver you just installed is never used by any game.**
Everything under FEX otherwise runs against an emulated x86-64 Honeykrisp
shipped inside the FEX rootfs.

    cp fex/ThunksDB.json fex/Config.json ~/.fex-emu/

`fex/README.md` explains why this is not on by default — three independent
reasons, all of which had to be found the hard way.

## 5. Check the whole path

    honeykrisp-got doctor

That checks the architecture, the render node, the deployed driver, `$HOME/.drirc`
and the FEX thunk config in one go, and tells you which step you skipped.

Then launch:

    ASAHI_LOCAL_MESA=0 steam

`ASAHI_LOCAL_MESA=0` is correct and required. It stops the Fedora Asahi steam
wrapper pointing `VK_DRIVER_FILES` at its own build, so everything resolves
through the system path — which is now this driver.

## Did it actually work?

    HK_GPUTIME=5 ASAHI_LOCAL_MESA=0 steam

`[hk gputime]` in the output comes from this driver and from nothing else, so
seeing it from an x86-64 process proves the whole chain — thunk, pressure-vessel
import, system path — is live. `gputime.sh` reads the output.

If frame rate is what you want to compare, **pin the render resolution first**.
The game's dynamic resolution moves render resolution over a 4x range chasing
30 fps, so a driver improvement can be absorbed entirely as extra pixels at
constant frame rate. `data/measurement-hazards.md` has the details and
`data/game-settings-pinned.md` the settings.

## Turning it off

    HK_PERFTEST=nooverlap ...              # keep the driver, restore the old barrier
    sudo honeykrisp-got disable            # put the distro driver back
    sudo dnf remove honeykrisp-got         # remove it entirely (disables first)
    sudo rm /etc/yum.repos.d/honeykrisp-got.repo    # and stop tracking updates

## After a mesa update

A `dnf upgrade` that touches mesa reinstalls the distro driver over the top,
which would silently halve your frame rate with nothing to explain it. The
package installs an RPM file trigger that re-applies the swap, but **only if you
had enabled it**, and it prints a line saying so. `honeykrisp-got status` will
always tell you which driver is actually in place.

While it is enabled, `rpm -V mesa-vulkan-drivers` reports the modified file.
That is expected, and it is the honest signal that something replaced it.

## Building it yourself instead

    ./build-driver.sh                       # clone the pinned Mesa fork and build it
    ./deploy-system-driver.sh deploy        # swap it in

`mesa-source.env` names the repository, branch and exact commit — the same one
the packages are built from, and the one every number in `STATE.md` came from.

To build the RPM locally, on any aarch64 Fedora machine or container:

    podman run --rm -v "$PWD:/src" -w /src fedora:44 packaging/ci-build.sh

That leaves the binary packages and a source RPM in `out/`. The source RPM is
131 MB — it carries the whole Mesa tarball — so it is not attached to releases;
`packaging/make-srpm.sh` regenerates it from the pin whenever you want it.

## Which machines this has actually been tested on

One: an M1 Max under Fedora Asahi Remix 44. The conformance runs (15,307 tests
across `dEQP-VK.memory_model`, `synchronization.op.single_queue` and `compute`,
no regression against the unmodified driver) were on the same machine. Other
Apple GPUs should work and none has been tried. If yours does not, the failure
mode that matters is the weak CDM barrier: `HK_PERFTEST=nooverlap` tells you
in one run whether that is what you are looking at.
