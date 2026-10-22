# Installing fsr2dlss

This replaces Lies of P's FSR 3.1 upscaling and frame generation with NVIDIA DLSS Super Resolution and
DLSS Multi-Frame Generation. It has been tested on Lies of P on an RTX 5090. Other FSR 3.1 games that
use the same FidelityFX API might work but are untested.

This is an unofficial mod that swaps a game DLL. Back up the original first and expect to update it as
the game and drivers change.

## 1. Requirements

- An RTX 40- or 50-series GPU. 3x/4x Multi-Frame Generation and Dynamic MFG need a 50-series card.
- A recent NVIDIA driver (the bundled Streamline 2.14 / DLSS 310.9 need a 2025 or newer driver).
- Hardware-Accelerated GPU Scheduling turned on: Windows Settings, System, Display, Graphics, Default
  graphics settings. Reboot after changing it.
- Windows 10 or 11, DirectX 12, and a copy of Lies of P.

## 2. Get the files

Download the newest `fsr2dlss-vX.Y.Z.zip` from the repo's **Releases** page and unzip it. It contains
everything:

```
amd_fidelityfx_dx12.dll     the mod (proxy)
fsr2dlss.ini                configuration (defaults are fine)
INSTALL.md                  this file
VERSIONS.txt                exact versions of the bundled DLLs
licenses\                   Streamline, DLSS, Reflex, Dear ImGui, MinHook license texts
streamline\
  sl.interposer.dll  sl.common.dll  sl.dlss.dll  sl.dlss_g.dll  sl.pcl.dll  sl.reflex.dll
  nvngx_dlss.dll     nvngx_dlssg.dll
```

Nothing has to be downloaded separately: the Streamline plugins and the DLSS / DLSS-G model DLLs are
bundled. You can also build the proxy yourself; see ARCHITECTURE.md.

## 3. Install

Let `WIN64` be `...\steamapps\common\Lies of P\LiesofP\Binaries\Win64`, the folder with
`LOP-Win64-Shipping.exe`. Installing is just file copies.

1. Back up `WIN64\amd_fidelityfx_dx12.dll` (the real one, about 6.6 MB) and rename it to
   `amd_fidelityfx_dx12.amd.dll`. The proxy forwards to it.
2. Copy the zip's `amd_fidelityfx_dx12.dll` into `WIN64`.
3. Copy the zip's `streamline\` folder into `WIN64`.
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

Updating: overwrite `amd_fidelityfx_dx12.dll` and the `streamline\` folder with the new zip's. Keep
your `fsr2dlss.ini` if you have customised it (new keys fall back to defaults).

## 4. In-game settings

In the graphics options set Upscaling / Super Resolution to FSR (any preset) and turn Frame
Generation on. The mod swaps FSR for DLSS underneath and picks the matching DLSS quality mode from the
preset you chose. The game still shows FSR in its menu; that is expected. DLSS frame generation only
runs while the game's Frame Generation is on, because that is when the game hands the mod its swapchain.

## 5. The overlay (Home key)

Press **Home** in game to open the configuration overlay (the key is `ToggleKey` in `fsr2dlss.ini`).
It is available from the moment the game window appears, with or without FSR and frame generation on.
While it is open the mouse and keyboard go to the overlay, not the game. Press Home again or click
Close to return. Changes apply immediately; **Save to fsr2dlss.ini** writes them to disk for the next
start.

- **Status** (top of the menu): whether the original AMD DLL was found; whether the game is issuing
  FSR calls and how often (so you can tell FSR is actually selected in the game); whether the game's
  frame generation is on and its swapchain was replaced by DLSS-G; Streamline's state; which swapchain
  the overlay is hooked into (DLSS-G or the game's own); and DLSS-G's status flags if it reports a
  problem.

- **DLSS Super Resolution**: enable/disable DLSS (off = the game's FSR), the DLSS quality mode (Auto
  follows the in-game FSR preset), and the DLSS model preset (Default, K, J, L, M, or the legacy E/F).
- **DLSS Frame Generation**: the preset (Off, Fixed multiplier, Dynamic Multi-Frame Generation), the
  multiplier (2x, 3x, 4x, clamped to what the card reports), the Dynamic target fps, and flip metering
  (hardware/software, applied at the next start).
- **Overlay**: the FPS counter on/off and its corner, UI scale, HDR brightness, and the Steam overlay
  fix.

The FPS counter (on by default, top-right) shows `<displayed> fps | base <rendered> | 3x`: the
framerate reaching your display, the framerate the game actually renders, and the multiplier in
effect. With frame generation off it shows the plain framerate.

## 6. Configure fsr2dlss.ini

The file sits next to `amd_fidelityfx_dx12.dll`. Everything in it can also be changed from the overlay.

| Section / key | Values | Notes |
|---|---|---|
| `[FrameGen] Mode` | `fixed`, `dynamic`, `off` | FG preset. `dynamic` needs card/driver support for Dynamic MFG and falls back to fixed otherwise. |
| `[FrameGen] Multiplier` | `2`, `3`, `4` | Fixed multiplier. Clamped to the card's maximum. |
| `[FrameGen] DynamicTargetFps` | number | Target for `dynamic`; `0` = display refresh rate. |
| `[FrameGen] FlipMetering` | `hardware`, `software` | `hardware` gives true 3x/4x MFG. `software` is clean 2x. Restart to apply. |
| `[Upscaling] DLSS` | `true`, `false` | DLSS upscaling on, or pass through to FSR. |
| `[Upscaling] Quality` | `auto`, `dlaa`, `quality`, `balanced`, `performance`, `ultraperformance` | DLSS mode; `auto` follows the in-game FSR preset. |
| `[Upscaling] Preset` | `default`, `K`, `J`, `L`, `M`, `E`, `F` | DLSS model preset (DLSS 310 letters). |
| `[Overlay] SteamOverlayFix` | `true`, `false` | Make the Steam overlay report DLSS with correct counts. |
| `[Overlay] Enabled`, `ToggleKey` | `true`/`false`, key name | Overlay availability and key (`Home`, `F10`, `Insert`, `0x24`, ...). |
| `[Overlay] ShowFps`, `FpsCorner` | `true`/`false`, `0`-`3` | FPS counter and its corner (0 TL, 1 TR, 2 BL, 3 BR). |
| `[Overlay] Scale`, `BrightnessNits` | `0.5`-`3.0`, `50`-`1000` | UI scale; paper white on HDR displays. |
| `[HUD] HudlessTags` | `false` | Experimental. Leave off for this game. |
| `[Logging] Debug` | `false` | Write `ffx_bridge.log`. Leave off unless troubleshooting. |
| `[Compat] LegacyTagging` | `false` | Troubleshooting: use the pre-2.14 Streamline tagging path. |

## 7. Verify it is working

- The FPS counter shows a displayed framerate well above the base framerate and the multiplier.
- The Steam overlay shows DLSS with a native and a generated frame count.
- For a definitive check, run `src\fg_overlay.ps1` as administrator once to enable NVIDIA's on-screen
  DLSS and DLSS-G indicators, then relaunch. Remove them later with `fg_overlay.ps1 -Off`.

## 8. Troubleshooting

Turn on logging first: set `Debug = true` in `fsr2dlss.ini` (or in the overlay, then Save), relaunch,
and read `WIN64\ffx_bridge.log` and `WIN64\streamline\sl.log`.

- **Crash on launch.** Usually the `streamline\` folder is incomplete. Make sure all six Streamline
  plugins and both `nvngx_*.dll` models are present, and that the original DLL was renamed to
  `amd_fidelityfx_dx12.amd.dll`.
- **Image looks like FSR, not DLSS.** The super-resolution plugin is not loading. Check that
  `sl.dlss.dll` and `nvngx_dlss.dll` are in `streamline\`; the log should print `[SR] DLSS upscale ACTIVE`.
- **Frame generation not multiplying.** Confirm HAGS is on and that in-game Frame Generation is on.
  The overlay's status line prints DLSS-G's status flags if Streamline reports a problem.
- **Driver too old.** `sl.log` says so explicitly; update the NVIDIA driver.
- **Something that worked with an older fsr2dlss broke.** Set `[Compat] LegacyTagging = true`, which
  restores the old Streamline tagging path, and report what you see.
- **Overlay does not open.** Check `ToggleKey` and that the game window has focus. With `Debug = true`
  the log shows `[DXGI] native swapchain hooks installed` and `[OVL] bound to swapchain`; if the DXGI
  hook failed, the overlay still appears once the game's Frame Generation is on.
- **Status says "no FidelityFX calls yet".** The game is not using FSR: pick FSR as the upscaler in the
  graphics menu. "FSR context exists but no upscale pass" means FSR is selected but idle (menu/loading).

## 9. Uninstall

Delete `amd_fidelityfx_dx12.dll`, rename `amd_fidelityfx_dx12.amd.dll` back to
`amd_fidelityfx_dx12.dll`, and remove `streamline\`, `fsr2dlss.ini`, and any `ffx_bridge.log`.
