# Every change made to this machine during GoT bring-up

Complete, ordered, with the exact revert for each. Nothing here is permanent.
Recorded 2026-09-03.

---

## 1. Proton per-tool environment  [ACTIVE]

**File:** `~/.local/share/Steam/steamapps/common/Proton - Experimental/user_settings.py`
**Created by:** Claude. Did not exist before (only `user_settings.sample.py` shipped).

    DXVK_CONFIG        = dxgi.customVendorId = 0000; dxgi.customDeviceId = 0000
    VKD3D_SHADER_MODEL = 6_6
    MANGOHUD           = 1
    MANGOHUD_CONFIG    = fps,frametime,frame_timing,cpu_stats,cpu_load_change,
                         core_load,cpu_mhz,gpu_stats,gpu_load_change,vram,ram,
                         swap,resolution,vulkan_driver,engine_version,
                         position=top-left,font_size=18,background_alpha=0.6
    PROTON_LOG         = 1

**Revert:** `rm "$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/user_settings.py"`

**WARNING:** this applies to EVERY game using Proton Experimental, not just GoT
(Satisfactory too). Once the config is stable it should move to per-game Steam
launch options for app 2215430:

    DXVK_CONFIG="dxgi.customVendorId = 0000; dxgi.customDeviceId = 0000" VKD3D_SHADER_MODEL=6_6 %command%

**PROTON_LOG=1 costs performance** (it enables wine tracing incl. +loaddll).
Remove it before taking any performance measurement.

---

## 2. Wine virtual desktop in the GoT prefix  [ACTIVE]

**File:** `~/.local/share/Steam/steamapps/compatdata/2215430/pfx/user.reg`
**Appended:**

    [Software\\Wine\\Explorer]
    "Desktop"="Default"

    [Software\\Wine\\Explorer\\Desktops]
    "Default"="1920x1200"

Needed because `appledrm.show_notch=1` leaves the panel advertising only
3024x1964 (aspect 1.54), which no game mode table contains. Confirmed working:
the game logs `rect(0 1920 0 1200)`, `aspect = 1.60`.

**Revert:** restore the backup, or delete both keys:
`cp <scratchpad>/user.reg.bak "$HOME/.local/share/Steam/steamapps/compatdata/2215430/pfx/user.reg"`
(Backup lives in the session scratchpad, which does NOT survive a reboot —
if you care, copy it somewhere permanent now. Deleting the two keys by hand is
equally safe.)

---

## 3. Installed package: xdotool  [ACTIVE, system-wide]

    sudo dnf install -y xdotool     # pulled in libxdo as a dependency

Installed so an automated agent can drive the game (X11/XTEST input injection —
the game is an Xwayland client, so this reaches it). No input-injection tool was
present beforehand: no ydotool, wtype, xdotool or dotool.

Screenshot tooling was already present (`spectacle`, ImageMagick `import`).

**Revert:** `sudo dnf remove xdotool libxdo`

---

## 4. Documentation folder  [ACTIVE]

    $HOME/Projects/got-bringup/          (renamed from got-gpu-detection)
      README.md        diagnostic record - how each problem was found
      CHECKLIST.md     actionable driver-gap list, the working document
      CHANGES.md       this file
      evidence/        captured logs and registry state

`Projects/STATE.md` finding 8 was updated to point at the new paths.

**Revert:** `rm -rf $HOME/Projects/got-bringup` and drop finding 8.

---

## 5. Temporary files  [CLEANED UP]

Probe files were written to `$HOME/probe-*.txt` while inspecting the
muvm guest (the guest shares the host home, and `muvm -i` swallows stdout when
stdin is not a tty, so writing to a shared file was the only way to read guest
output). All removed.

---

## NOT changed

For the record, these were considered and deliberately left alone:

- **Kernel cmdline.** `appledrm.show_notch=1` is still set. Reverting it needs
  a reboot and is your call: `sudo grubby --update-kernel=ALL --remove-args=appledrm.show_notch=1`
- **`~/.local/bin/steam`** wrapper — not modified.
- **Mesa source trees** — not modified. All investigation was read-only.
- **`/usr/bin/steam`** — not modified.
- **Game files** — not modified. No patching of `GhostOfTsushima.exe`.

---

## How to get back to a completely stock game setup

    rm "$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/user_settings.py"
    # then remove the two Wine\Explorer keys from
    #   ~/.local/share/Steam/steamapps/compatdata/2215430/pfx/user.reg
    sudo dnf remove xdotool libxdo

The game will then show "No installed graphics card has been detected" again,
and behind it "Shader Model 6.6 support not detected". Both dialogs are the
untreated symptoms; nothing is permanently altered.

---

## 6. Steam wrapper: muvm VRAM cap  [ACTIVE]

**File:** `~/.local/bin/steam` (your own wrapper, modified)
**Backups:** `~/.local/bin/steam.bak-preVRAM`, `evidence/steam-wrapper.bak-preVRAM`

Section 2 of the wrapper now injects muvm sizing arguments in addition to `-e`
environment forwarding. It passes `--vram=8192` by default.

Rationale: muvm defaults `--vram` to 50% of system RAM (15808 MB here) and that
number reaches the game as its video memory budget. GoT streams textures to fill
it; working set hit 12.8 GB and fps collapsed 56.6 -> 10.0 with no settings
change. See CHECKLIST.md item 2.

    ASAHI_VRAM_MB=10240 steam    # tune
    ASAHI_VRAM_MB=0     steam    # restore muvm's default for one run

Verified by dry run:

    DRYRUN: return pexpect.spawn("muvm",
        ["-e", 'MESA_SHADER_CACHE_MAX_SIZE=12G', '--vram=8192', "--"] + cmd)

**Revert:** `cp ~/.local/bin/steam.bak-preVRAM ~/.local/bin/steam`

**UNTESTED** at time of writing. 8192 is a first guess, not a measured optimum.

---

## 7. Desktop / menu launchers pinned to system Mesa  [ACTIVE]

Two user-level `.desktop` entries, both in `~/.local/share/applications/`, which
shadows `/usr/share/applications/` — the packaged files are untouched.

**`steam.desktop`** (NEW — no user override existed before; copied from the
system entry with only the Exec line changed):

    Exec=env ASAHI_LOCAL_MESA=0 steam %U

**`Ghost of Tsushima DIRECTOR'S CUT.desktop`** (MODIFIED, Steam-generated;
backup at `evidence/GoT.desktop.bak`):

    Exec=env ASAHI_LOCAL_MESA=0 steam steam://rungameid/2215430

Both pin the launch to system Mesa, because the local build cannot create a
Vulkan instance inside pressure-vessel (CHECKLIST.md item 6). Without this the
menu entries would silently use the broken default and the game would die at
DXGI init with no visible explanation.

Verified with the exact Exec line:

    steam wrapper: without -cef-force-occlusion; forwarding 1 env var(s) into
    muvm; muvm args ['--vram=8192']; vulkan: system Mesa (ASAHI_LOCAL_MESA=0)

**Revert:**

    rm ~/.local/share/applications/steam.desktop
    cp <this dir>/evidence/GoT.desktop.bak \
       ~/.local/share/applications/"Ghost of Tsushima DIRECTOR'S CUT.desktop"
    update-desktop-database ~/.local/share/applications

**Note:** `Satisfactory.desktop` was left alone. If Satisfactory also uses Steam
Linux Runtime it will hit the same mesa-local failure; if it works, that is a
useful data point for item 6 (it would mean the failure is runtime-version
specific rather than universal).

**Remove these overrides once item 6 is fixed** — otherwise they will silently
keep pinning system Mesa long after the local build works.

---

## `~/.local/bin/steam` — HK_GPUTIME added to the forward list

`HK_GPUTIME` joined `FORWARD`, so it reaches the muvm guest and therefore the
game. Without this the variable is silently dropped and the driver never turns
profiling on — the same trap that made `MESA_SHADER_CACHE_MAX_SIZE` a no-op
before this wrapper existed.

Verified:

    steam wrapper: forwarding 3 env var(s) into muvm
    DRYRUN: pexpect.spawn("muvm", ["-e", 'MESA_SHADER_CACHE_MAX_SIZE=12G',
                                   "-e", 'HK_GPUTIME=5', ...

**Revert:** remove the `"HK_GPUTIME",` line from `FORWARD`.

## mesa-local `local-deploy` — two new commits

    46b2d153884 asahi/hk: measure real GPU time with the firmware timestamps
    32581824bf9 asahi/hk: add HK_PERFTEST=noflush to bound the CDM barrier tax

The second renders incorrectly by design and is only ever used for measurement;
it is documented as such in the source and in its commit message. It has already
paid for itself by retiring the barrier hypothesis (CHECKLIST.md).

Both are deployed to `/usr/lib64/libvulkan_asahi.so`. Revert the whole system
driver with `./deploy-system-driver.sh restore`.

## A note on the perftest banner

`hk_device.c` now prints `[hk] HK_PERFTEST active: 0x...` at device creation
whenever the variable is set. Cheap insurance: several experiments in this
project have been forwarded through three layers (host env → muvm → Proton →
pressure-vessel), and a flag that silently fails to arrive looks exactly like a
flag that arrived and did nothing. The banner tells those apart.
