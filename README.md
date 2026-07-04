# fsr2dlss

Replace **AMD FSR 3.1** with **NVIDIA DLSS + DLSS Frame Generation (DLSS‑G)** in games built on
AMD's unified FidelityFX API (`amd_fidelityfx_dx12.dll`). Proven end‑to‑end on **Lies of P**
(UE4 / D3D12 / HDR10) on an **RTX 5090 (Blackwell)**.

It's a transparent proxy of `amd_fidelityfx_dx12.dll`: it forwards untouched calls to the renamed
real DLL and translates the upscale + frame‑generation work to NVIDIA Streamline. The game keeps
thinking it runs FSR; DLSS runs underneath.

## Status: ✅ working

- **DLSS Super‑Resolution** replaces the FSR upscale (auto‑picks DLAA/Quality/Balanced/… from the
  in‑game preset).
- **DLSS Frame Generation** replaces FSR frame gen — confirmed doubling
  (`numFramesActuallyPresented == 2`) and by NVIDIA's on‑screen indicators.

The novel result is getting **real DLSS‑G to engage under injection on Blackwell**, which required
an **intermediate Streamline version (2.7.1)** plus a **flip‑metering block**. See
**[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** for the full story and design.

## Repo layout

```
src/        proxy.cpp, slbridge.cpp, nvhook.cpp, slbridge.h, exports.def   (the mod)
            install.ps1, uninstall.ps1, fg_overlay.ps1, build.ps1          (tooling)
            CONTRACT.md   (reverse-engineered FfxApi struct offsets)
            SL_SPEC.md    (Streamline DLSS-G integration notes)
docs/       ARCHITECTURE.md   (how it works — read this first)
```

## Build (summary — see docs/ARCHITECTURE.md §8)

Needs MinGW‑w64 GCC, the NVIDIA Streamline 2.2 SDK headers, and [MinHook](https://github.com/TsudaKageyu/minhook).
None are committed. Then `g++ … proxy.cpp slbridge.cpp nvhook.cpp mh_obj/*.o exports.def` (or `src/build.ps1`).

## Install (summary — see docs/ARCHITECTURE.md §9)

1. Run `src/install.ps1` (backs up the game's real DLL, installs the proxy).
2. Provide a **matched Streamline 2.7.1** runtime in the game's `streamline\` folder
   (`sl.*` 2.7.1 + `nvngx_dlss`/`nvngx_dlssg` 310.1) — source these from a game you own (e.g.
   Cyberpunk 2077). **Not committed** (NVIDIA redistributables).
3. Create `sl_hostver.txt` containing `2.7.1`.
4. In‑game: Upscaling = **FSR**, Frame Generation = **ON**.

## Credits

Concept from LukeFZ's dead "FSR2Streamline". Uses NVIDIA Streamline (MIT) and MinHook (MIT).
The flip‑metering block mirrors OptiScaler's `DisableFlipMetering`. NVIDIA binaries not included.
