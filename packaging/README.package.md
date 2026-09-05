# honeykrisp-got

The Honeykrisp (Asahi) Vulkan driver, built from a Mesa fork carrying two
changes that cut *Ghost of Tsushima DIRECTOR'S CUT*'s per-frame compute time by
about **4.1x** on an M1 Max:

| fix | fps | compute |
|---|---|---|
| independent compute dispatches allowed to overlap | +34.5% | 2.17x |
| shader constant tables kept out of per-invocation scratch | +30.1% | 1.88x |

Measured at pinned render resolution, one change removed at a time. The evidence,
including the three measurement hazards that invalidated earlier numbers, is at
<https://github.com/aquarat/got-bringup>.

## Installing it changes nothing on its own

The driver lands in `/usr/lib64/honeykrisp-got/` and is inert until you say:

    sudo honeykrisp-got enable
    honeykrisp-got user-config     # as your normal user, not root

`enable` swaps it for `/usr/lib64/libvulkan_asahi.so` and keeps the distro file
as `/usr/lib64/libvulkan_asahi.so.honeykrisp-got-backup`.

## Why it has to be that path

A second ICD manifest in `/usr/share/vulkan/icd.d` does not reach a Steam game.
FEX overlays `libvulkan.so.1` only at fixed paths, and pressure-vessel imports
the *host* driver from the host's standard library path. Replacing that file is
the only mechanism that delivers a driver to the game.

## What that costs you

`/usr/lib64/libvulkan_asahi.so` is also what your compositor uses. `enable`
refuses to deploy a driver that cannot enumerate the GPU, and replaces the file
by rename so running clients are not disturbed, but a driver that is *subtly*
wrong can still take your desktop down. Recovery needs no working GPU:

    Ctrl-Alt-F3, log in, then:  sudo honeykrisp-got disable

This is a development build of Mesa, not a distro package. `rpm -V
mesa-vulkan-drivers` will report the modified file for as long as it is enabled.

## Commands

    honeykrisp-got status         what is deployed right now
    honeykrisp-got test           vulkaninfo against this driver, changing nothing
    honeykrisp-got doctor         check everything the game needs
    honeykrisp-got enable         deploy it   (root)
    honeykrisp-got disable        put the distro driver back   (root)
    honeykrisp-got user-config    hk driconf options into $HOME/.drirc

A mesa update reinstalls the distro driver and would silently undo `enable`. An
RPM file trigger re-applies it, but only if you enabled it, and it says so.
