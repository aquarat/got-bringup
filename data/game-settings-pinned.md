# Pinning the game's render resolution

The game's dynamic resolution controller moves render resolution between
960x600 and 1920x1200 to chase a 30 fps target, which makes frame rate useless
as a measure of GPU efficiency. See `measurement-hazards.md`.

Pinned by editing two values in the Wine prefix registry
(`compatdata/2215430/pfx/user.reg`, section
`[Software\\Sucker Punch Productions\\Ghost of Tsushima DIRECTOR'S CUT\\Graphics]`):

| key | original | set to | effect |
|---|---|---|---|
| `UpscaleQuality` | `dword:00000003` (Dynamic) | `dword:00000002` | fixed "Balanced" upscale ratio |
| `DynamicResolutionTargetFPS` | `dword:0000001e` (30) | `dword:00000001` | target always met, so no scaling |

Both can also be changed in the game's own Options menu (Upscale Quality:
anything other than Dynamic).

Verified: the game then logs `Upscale quality: Balanced` and
`Dynamic resolution FPS target: 0`, and 47 of 54 sample windows sit at exactly
808,832 invocations of the one-invocation-per-pixel shader.

**A full registry backup is deliberately NOT kept in this repository.** A Wine
`user.reg` is a whole-desktop dump: the one taken here contained a speaker's
MAC address, eight LAN hostnames and ten private IPs from the author's home
network, none of which has anything to do with the graphics driver. Only the
two values above matter, and they are recorded here.
