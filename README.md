# fsr2dlss

Replaces AMD FSR 3.1 with NVIDIA DLSS in Lies of P. The game's upscaling becomes DLSS
Super-Resolution and its frame generation becomes DLSS-G Multi-Frame Generation, all driven through
NVIDIA Streamline. Built and tested on an RTX 5090.

The mod is a proxy for `amd_fidelityfx_dx12.dll`. The game loads it by name, so it forwards the
calls it doesn't care about to the renamed real AMD DLL and translates the upscale and
frame-generation work to Streamline. As far as the game is concerned it is still running FSR; DLSS
runs underneath it.

## What you get

- DLSS Super-Resolution instead of FSR. The DLSS quality mode is chosen from the in-game FSR preset:
  a lower preset gives more upscaling and more frames, native resolution gives DLAA.
- DLSS-G frame generation. On an RTX 50-series card this is true 3x Multi-Frame Generation running on
  Blackwell hardware flip metering (roughly 75 fps rendered turns into 225 fps displayed). 2x and 4x
  are available through the `Multiplier` setting.
- The Steam overlay reports DLSS with the correct native and generated frame counts.

## Requirements

- An RTX 50-series GPU for 3x/4x MFG, a recent NVIDIA driver, and Hardware-Accelerated GPU Scheduling
  turned on in Windows.
- Windows 10 or 11, DirectX 12, and a copy of Lies of P.

## Install

Full steps are in [docs/INSTALL.md](docs/INSTALL.md). In short: back up the game's
`amd_fidelityfx_dx12.dll` and rename it to `amd_fidelityfx_dx12.amd.dll`, drop the built proxy in its
place, create a `streamline\` folder with the Streamline 2.7.4 plugin set (plus the DLSS
super-resolution plugin and nvngx models), copy `fsr2dlss.ini` next to the proxy, and set the game to
FSR + Frame Generation. `src/install.ps1` handles the DLL rename.

The Streamline and nvngx binaries are NVIDIA redistributables and are not included here; source them
from a game you own.

## Build

Needs MinGW-w64 GCC, the Streamline SDK headers under `third_party/sl2.2`, and
[MinHook](https://github.com/TsudaKageyu/minhook). Run `src/build.ps1`, or check
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the exact compile line. The GitHub Actions build
produces an artifact with the proxy DLL and a sample `fsr2dlss.ini`.

## Layout

```
src/     proxy.cpp, slbridge.cpp, nvhook.cpp, slbridge.h, exports.def   the mod
         install.ps1, uninstall.ps1, fg_overlay.ps1, build.ps1          tooling
         CONTRACT.md   reverse-engineered FfxApi struct offsets
         SL_SPEC.md    Streamline DLSS-G integration notes
docs/    ARCHITECTURE.md, INSTALL.md
```

## Credits

The idea traces back to LukeFZ's FSR2Streamline; this is a from-scratch rebuild for the FSR 3.1
FidelityFX API. Uses NVIDIA Streamline and MinHook (both MIT). NVIDIA binaries are not included.
