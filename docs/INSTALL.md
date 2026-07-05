# Installing fsr2dlss

This replaces Lies of P's FSR 3.1 upscaling and frame generation with NVIDIA DLSS and DLSS-G. It has
been tested on Lies of P on an RTX 5090. Other FSR 3.1 games that use the same FidelityFX API might
work but are untested.

This is an unofficial mod that swaps a game DLL. Back up the original first and expect to update it as
the game and drivers change.

## 1. Requirements

- An RTX 50-series GPU. Frame generation needs one, and 3x/4x Multi-Frame Generation is 50-series only.
- A recent NVIDIA driver.
- Hardware-Accelerated GPU Scheduling turned on: Windows Settings, System, Display, Graphics, Default
  graphics settings. Reboot after changing it.
- Windows 10 or 11, DirectX 12, and a copy of Lies of P.

## 2. Get the files

**The mod.** Download the `fsr2dlss` artifact from the repo's GitHub Actions build. It contains the
proxy `amd_fidelityfx_dx12.dll`, a sample `fsr2dlss.ini`, and a `streamline\` folder with the
Streamline plugins (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`,
`sl.pcl.dll`, `sl.reflex.dll`). Streamline is MIT-licensed, so those ship with the mod. You can also
build the proxy yourself; see ARCHITECTURE.md.

**The two NGX models.** DLSS runs from two NVIDIA NGX DLLs that are proprietary and cannot be bundled.
Download them from TechPowerUp and drop them into the `streamline\` folder alongside the Streamline
plugins:

- `nvngx_dlss.dll` (super-resolution): https://www.techpowerup.com/download/nvidia-dlss-dll/
- `nvngx_dlssg.dll` (frame generation): https://www.techpowerup.com/download/nvidia-dlss-3-frame-generation-dll/

Use version **310.1.0** of each; a newer 310.x build should also work. TechPowerUp lets you pick the
version from a dropdown, then gives you the DLL to copy into `streamline\`.

## 3. Install

Let `WIN64` be `...\steamapps\common\Lies of P\LiesofP\Binaries\Win64`, the folder with
`LOP-Win64-Shipping.exe`. Installing is just file copies.

1. Back up `WIN64\amd_fidelityfx_dx12.dll` (the real one, about 6.6 MB) and rename it to
   `amd_fidelityfx_dx12.amd.dll`. The proxy forwards to it.
2. Copy the proxy `amd_fidelityfx_dx12.dll` from the artifact into `WIN64`.
3. Copy the artifact's `streamline\` folder into `WIN64`, then add `nvngx_dlss.dll` and
   `nvngx_dlssg.dll` from step 2 to it.
4. Copy `fsr2dlss.ini` into `WIN64`.

The final layout:

```
WIN64\
  amd_fidelityfx_dx12.dll        the mod (proxy)
  amd_fidelityfx_dx12.amd.dll    the original AMD DLL the proxy forwards to
  fsr2dlss.ini
  streamline\
    sl.interposer.dll  sl.common.dll  sl.dlss.dll  sl.dlss_g.dll
    sl.pcl.dll         sl.reflex.dll  nvngx_dlss.dll  nvngx_dlssg.dll
```

## 4. In-game settings

In the graphics options set Upscaling / Super Resolution to FSR (any preset) and turn Frame
Generation on. The mod swaps FSR for DLSS underneath and picks the matching DLSS quality mode from the
preset you chose. The game still shows FSR in its menu; that is expected.

## 5. Configure fsr2dlss.ini

The file sits next to `amd_fidelityfx_dx12.dll`.

| Setting | Values | Notes |
|---|---|---|
| `Multiplier` | `2`, `3`, `4` | Frame-gen multiplier. Clamped to the card's maximum. |
| `FlipMetering` | `hardware`, `software` | `hardware` gives true 3x/4x MFG. `software` is clean 2x. |
| `DLSS` | `true`, `false` | DLSS upscaling on, or pass through to FSR. |
| `SteamOverlayFix` | `true`, `false` | Make the Steam overlay report DLSS with correct counts. |
| `HudlessTags` | `false` | Experimental. Leave off for this game. |
| `Debug` | `false` | Write `ffx_bridge.log`. Leave off unless troubleshooting. |

## 6. Verify it is working

- The framerate roughly triples with Frame Generation on, and the image looks cleaner than FSR.
- The Steam overlay shows DLSS with a native and a generated frame count.
- For a definitive check, run `src\fg_overlay.ps1` as administrator once to enable NVIDIA's on-screen
  DLSS and DLSS-G indicators, then relaunch. Remove them later with `fg_overlay.ps1 -Off`.

## 7. Troubleshooting

Turn on logging first: set `Debug = true` in `fsr2dlss.ini`, relaunch, and read `WIN64\ffx_bridge.log`
and `WIN64\streamline\sl.log`.

- **Crash on launch.** Usually the `streamline\` folder is incomplete. Make sure all six Streamline
  plugins and both `nvngx_*.dll` models are present, and that the original DLL was renamed to
  `amd_fidelityfx_dx12.amd.dll`.
- **Image looks like FSR, not DLSS.** The super-resolution plugin is not loading. Check that
  `sl.dlss.dll` and `nvngx_dlss.dll` are in `streamline\`; the log should print `[SR] DLSS upscale ACTIVE`.
- **Wrong colors.** A model mismatch. Use `nvngx` version 310.1.0 with the bundled Streamline plugins.
- **Frame generation not multiplying.** Confirm HAGS is on and that in-game Frame Generation is on.

## 8. Uninstall

Delete `amd_fidelityfx_dx12.dll`, rename `amd_fidelityfx_dx12.amd.dll` back to
`amd_fidelityfx_dx12.dll`, and remove `streamline\`, `fsr2dlss.ini`, and any `ffx_bridge.log`.
