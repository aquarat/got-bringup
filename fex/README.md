# FEX Vulkan thunk — enabling native aarch64 Vulkan under muvm

Without this, anything running under FEX (i.e. every Steam game) uses an
**emulated x86-64 Honeykrisp 26.0.3** shipped inside the FEX rootfs, and never
touches the driver we build. With it, `libvulkan.so.1` is overlaid by FEX and
Vulkan runs as **native aarch64** against `/usr/lib64/libvulkan_asahi.so`.

## Why it is not on by default

Three independent reasons, all of which had to be fixed:

1. **FEX loads `ThunksDB.json` from its config directory only.** Not from
   `FEX_THUNKCONFIG` — that variable feeds the *enable map*, not the database
   (`FileManagement.cpp`, `LoadThunkDatabase`). muvm blanks
   `/usr/share/fex-emu` inside the guest, so the global copy is invisible and
   the database ends up empty. An empty database silently discards every enable.
2. **Nothing on the system enables the entry.** The DB only defines thunks; a
   config must set `{"ThunksDB":{"Vulkan":1}}`. No config on this machine did.
3. **`GuestThunks/` is invisible for the same reason as (1)**, so
   `FEX_THUNKGUESTLIBS` has to point at a copy on the shared filesystem.

## Files here

| File | Where it must be installed |
|---|---|
| `ThunksDB.json` | `~/.fex-emu/ThunksDB.json` |
| `Config.json`   | `~/.fex-emu/Config.json` |
| `GuestThunks/`  | anywhere shared; `FEX_THUNKGUESTLIBS` points at it |

`ThunksDB.json` is the system database plus explicit pressure-vessel container
paths. FEX expands `@PREFIX_LIB@` using prefixes chosen from the **FEX rootfs**
layout — Fedora, so `lib64` — while the Steam container is Debian-multiarch
(`lib/x86_64-linux-gnu`). Those paths can therefore never be generated and must
be named literally.

## Install

    cp ThunksDB.json Config.json ~/.fex-emu/
    # the steam wrapper sets FEX_THUNKGUESTLIBS itself

## Disable

    ASAHI_VULKAN_THUNK=0 steam      # one run
    rm ~/.fex-emu/Config.json       # permanently (leaves the DB harmlessly)

## Verify

    muvm -e HK_GPUTIME=5 -e FEX_THUNKGUESTLIBS=$PWD/GuestThunks \
      -- FEXBash -c 'vkcube --c 1500'

Look for `[hk gputime]` in the output: that banner comes from our driver, so
seeing it from an x86-64 process proves the thunk is live.

## Caveats

- `vulkaninfo` SIGILLs under the thunk (unimplemented entry point). Use
  `vkcube` to test instead — rendering is unaffected.
- Immediate present mode SIGSEGVs; FIFO is fine.
- The enable is global, so it applies to the Steam client too.
