# fsr2dlss architecture

fsr2dlss is a proxy for `amd_fidelityfx_dx12.dll`. Lies of P loads that DLL by name to run FSR 3.1
upscaling and frame generation. The proxy forwards the calls it does not handle to the renamed real
AMD DLL and routes the upscale and frame-generation work to NVIDIA Streamline instead, so the game
runs DLSS Super Resolution and DLSS Multi-Frame Generation while still believing it uses FSR.

The runtime stack is pinned: Streamline 2.14.16 plugins with the DLSS 310.9.16 super-resolution and
frame-generation models (see `third_party/streamline/SDK_VERSION`). On an RTX 50-series card DLSS-G
runs on Blackwell hardware flip metering, which paces the generated frames in the display engine and
gives real 3x (and 4x) multi-frame generation, or Dynamic Multi-Frame Generation.

## The FidelityFX contract

`amd_fidelityfx_dx12.dll` exports five entry points. The proxy exports the same five and forwards the
ones it does not intercept to `amd_fidelityfx_dx12.amd.dll`:

```
ffxCreateContext  ffxConfigure  ffxQuery  ffxDispatch  ffxDestroyContext
```

Each call carries a type id in its descriptor header. Three effect families matter:

- UPSCALE, `0x00010000` create and `0x00010001` dispatch: the upscale pass.
- FRAMEGEN, `0x00020000` family, with PREPARE `0x00020004` (depth, motion vectors, camera) and
  dispatch `0x00020003`.
- FGSWAPCHAIN, `0x00030000` family, where `NEW_DX12` `0x00030005` creates the frame-generation
  swapchain. This is the substitution point.

The descriptor struct offsets were reverse-engineered from live captures and are recorded in
[`src/CONTRACT.md`](../src/CONTRACT.md). A few facts about this game's data drive the bridge: depth is
inverted with an infinite far plane, motion vectors are render-resolution and UV-space with camera
motion included, and the game supplies camera near/far/fov scalars rather than matrices, so the bridge
builds a reversed-Z projection. Because `motionVectorScale` equals the render size, the motion vectors
are in UV space and Streamline gets `mvecScale = {1, 1}`.

## Layout

```
              game (LOP-Win64-Shipping.exe)
                        |  loads by name
                        v
      amd_fidelityfx_dx12.dll   <- the proxy (this repo)
        |  proxy.cpp routes by type id
        +-- UPSCALE dispatch   -> slbridge_upscale()          DLSS super-resolution
        +-- FGSWAPCHAIN NEW     -> slbridge_create_swapchain() DLSS-G swapchain
        +-- FRAMEGEN PREPARE    -> feedDLSSG()                 depth / MV / constants / markers
        +-- FRAMEGEN dispatch   -> slbridge_dispatch()         HUD-less tags, colorspace
        +-- everything else     -> forward to amd_fidelityfx_dx12.amd.dll
                        |
                        v
           streamline\  (Streamline 2.14.16 + nvngx 310.9.16)
```

## Components

### proxy.cpp

Lazily loads the renamed real DLL and caches its five entry points, then exports the five `ffx*`
functions. Each one dereferences the `ffxContext`: if it is a handle the bridge created it goes to the
bridge, otherwise it forwards to AMD. The UPSCALE dispatch is special-cased first so DLSS always runs
on it, whether or not the upscale context belongs to the bridge.

proxy.cpp also handles the Steam overlay. The Steam overlay detects FSR frame generation by placing an
inline hook on this DLL's `ffxConfigure` export. proxy.cpp saves its own clean `ffxConfigure` prologue
at load and runs a small background thread that restores it, removing Steam's hook so the overlay no
longer treats the game as an FSR title. This is gated by `SteamOverlayFix` in the ini and is on by
default.

### status.h

`ModStatus g_status`: plain counters and flags written by proxy.cpp (FidelityFX calls and contexts,
upscale passes, the real DLL), slbridge.cpp (Streamline state, the game's FG toggle, DLSS-G swapchain)
and dxgihook.cpp (native hooks, captured queue), read by the overlay's status panel.

### config.cpp

The one `fsr2dlss.ini` parser and writer, shared by every component through the global `g_cfg`
(`config.h`). The overlay edits `g_cfg` live and bumps a generation counter so the bridge re-applies
DLSS options cheaply; `config_save()` rewrites the ini with comments.

### slbridge.cpp

The Streamline bridge and the core of the mod. It initializes Streamline against the game's existing
D3D12 device with manual hooking, resolves the DLSS, DLSS-G, Reflex and PCL feature functions, and
reports the SDK version it compiles against (2.14.16) to `slInit`. Resources are tagged per frame with
`slSetTagForFrame` (`eUseFrameBasedResourceTagging`), the current Streamline model; `LegacyTagging`
in the ini brings back the deprecated global `slSetTag` path and host SDK 2.7.4, which is what
fsr2dlss builds before 2.14 used. DLSS-G options use the header's `DLSSGOptions` (struct version 5),
whose `numFramesToGenerate` selects the multiplier and whose `eDynamic` mode is Dynamic MFG.

- DLSS super-resolution (`slbridge_upscale`), described below.
- DLSS-G swapchain substitution (`slbridge_create_swapchain`): upgrade the DXGI factory with
  `slUpgradeInterface`, create the swapchain with the game's native command queue, and hand the
  resulting DLSS-G swapchain back to the game. `Present`, `Present1`, `ResizeBuffers` and
  `ResizeBuffers1` are hooked in place on that swapchain's vtable to bracket the frame with present
  markers and to draw the overlay. The swapchain is never wrapped in a separate COM object, because
  that breaks the identity Streamline's frame generation binds to.
- Per-frame feed (`feedDLSSG`, driven from FRAMEGEN PREPARE, keyed by the game's frame id), below.
- UI layer for DLSS-G: after the overlay has drawn, the Present hook tags the hud-less copy of the
  back buffer and the overlay's UI colour+alpha texture (`kBufferTypeHUDLessColor`,
  `kBufferTypeUIColorAndAlpha`, valid until present) and sets
  `enableUserInterfaceRecomposition`, so DLSS-G composites the overlay onto generated frames rather
  than interpolating it. The tags are cleared with null resources when the layers stop.
- Live statistics (`BridgeStats`) for the overlay: presents per second (the base framerate, since the
  game presents once per rendered frame) and the sum of `DLSSGState::numFramesActuallyPresented` per
  second (the displayed framerate), plus the card's MFG limits and DLSS-G status.

### dxgihook.cpp

Makes the overlay independent of frame generation. A worker thread started in `DllMain` hooks
`D3D12CreateDevice` and waits for the game's device (from that hook or from the FidelityFX backend
descriptor; after 20 s it falls back to a throwaway device). With that device it creates a probe queue
and a 64x64 swapchain on a hidden window, reads the runtime vtables (every DXGI swapchain and D3D12
queue in the process shares them), and MinHooks `IDXGISwapChain::Present/Present1/ResizeBuffers/
ResizeBuffers1/SetColorSpace1` and `ID3D12CommandQueue::ExecuteCommandLists`. The queue hook keeps,
per thread and with a reference held, the last direct queue that thread executed on, so the presenting
thread's own render queue is what the overlay submits to. `SetColorSpace1` is recorded per swapchain
so the overlay knows the colour space of a swapchain it was not told about. The Present hooks draw
the overlay on the game's own swapchain whenever the DLSS-G swapchain is not live (or being torn
down), and pass straight through otherwise (Streamline presents the real and generated frames through
a native swapchain that lands here).

### overlay.cpp

The in-game overlay, Dear ImGui drawn straight into the current swapchain's back buffer from a
Present hook, on the game's direct queue (one command list, three rotating allocators fenced on that
queue; back-buffer references are released every frame so `ResizeBuffers` is never blocked). The UI
is drawn into its own premultiplied RGBA layer and composited onto the back buffer with a fullscreen
blend; on the DLSS-G path a copy of the back buffer is taken before that composite and both textures
are handed to the bridge as the hud-less and UI layers. It binds
lazily to whatever swapchain and queue a hook hands it (the DLSS-G one from the bridge, the game's own
from dxgihook.cpp) and rebinds when they change. Its status panel reads `ModStatus` (`status.h`): what
the game is calling (FSR contexts, upscale passes per second, the FG toggle), whether the original AMD
DLL loaded, Streamline's state and which swapchain is hooked. The
toggle key is polled with `GetAsyncKeyState` so it works regardless of how the game reads input. While
open, the swapchain window's WndProc is subclassed: keyboard and mouse messages go to ImGui and are
swallowed (releases pass through so no key sticks), and `SetCursorPos`/`ClipCursor` are hooked so
the game cannot re-centre or hide the pointer. The DX12 backend in `third_party/imgui_backends` is
upstream ImGui plus a pixel-shader output transform so the UI is encoded correctly on HDR10 (PQ) and
scRGB swapchains; the bridge tells it the colour space when it sets `SetColorSpace1`, and on the game's
own swapchain it is derived from the back-buffer format and the output's HDR state.

### nvhook.cpp

Inline-hooks `nvapi64.dll!nvapi_QueryInterface` with MinHook. It also answers NGX's driver-profile
query for the DLSS Frame Generation model preset: NGX resolves `NvAPI_DRS_GetSetting` through the same
export and reads `NGX_DLSS_FG_OVERRIDE` / `NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION` (the settings
the NVIDIA App writes); when `FgPreset` is set, the hook returns "override on" and the chosen letter,
on Windows and under Proton (dxvk-nvapi) alike. The bridge restarts DLSS-G for a few presents after a
preset change so NGX re-creates the feature with it. When `FlipMetering = software` it
returns null for `NvAPI_D3D12_SetFlipConfig` (id `0xF3148C42`) and forwards everything else, which
drops DLSS-G onto its software pacer for clean 2x. When `FlipMetering = hardware` (the default) it
blocks nothing, and the Blackwell display engine's flip queue paces the generated frames for true
3x/4x. It is installed before `slInit`.

## DLSS super-resolution

At the UPSCALE dispatch the bridge reads the upscale descriptor (offsets in CONTRACT.md): color,
depth, motion vectors, output, jitter, and render size, each an `FfxApiResource`. Then it:

1. Calls `slDLSSSetOptions` on its super-resolution viewport, on change only. In `Quality = auto` the
   DLSS mode is derived from the render-to-output ratio, so any in-game FSR preset maps to the matching
   DLSS mode: 1.0 is DLAA, 0.67 Quality, 0.58 Balanced, 0.5 Performance, and so on; otherwise the
   configured mode is used. The configured model preset is applied to every mode's preset field.
2. Takes a frame token. With frame-based tagging it uses the frame index DLSS-G will use for this
   frame (the game's next frame id), so the SR tags (viewport 1) and the FG tags (viewport 0) share
   one frame entry; before FG has run it counts its own.
3. Sets constants (jitter, `mvecScale = {1, 1}`, inverted depth) and tags the scaling input color,
   depth, motion vectors, and output for that frame.
4. Calls `slEvaluateFeature(kFeatureDLSS)` on the game's own command list, writing into the game's
   output buffer, which the downstream frame-generation and present path then consumes.

If the evaluate fails the bridge returns false and the proxy forwards the upscale to FSR.

## DLSS-G frame generation

The frame-generation swapchain is substituted at FGSWAPCHAIN `NEW_DX12`, and its `Present` is hooked
in place.

Each frame, from FRAMEGEN PREPARE, `feedDLSSG` gets a frame token and emits the full PCL marker
cadence: simulation start and end, render-submit start, then render-submit end and present start in
the present hook, and present end after the real present. It tags depth and motion vectors with
explicit extents and builds reversed-Z constants. Right before each Present, on the present thread,
the hook asserts `slDLSSGSetOptions` from the configuration: `eOff` when the game or the ini has
frame generation off, `eOn` with `numFramesToGenerate = multiplier - 1` (clamped to
`numFramesToGenerateMax`), or `eDynamic` with `dynamicTargetFrameRate` when Dynamic MFG is selected
and the card reports it. Reflex options and sleep run around the present. After the present the hook
queries `slDLSSGGetState` for the card's limits, status and the number of frames actually presented.

At the FRAMEGEN dispatch the bridge sets the swapchain color space once (HDR10 PQ) and can optionally
tag HUD-less and UI layers.

## Build

The toolchain is MinGW-w64 GCC (a UCRT build with the d3d12 and dxgi headers). Dependencies are
fetched into `deps/` by `src/fetch_deps.sh` (bash) or `src/build.ps1` (PowerShell): MinHook and Dear
ImGui at pinned tags, and the Streamline SDK zip pinned by version and SHA-256 in
`third_party/streamline/SDK_VERSION`, from which the runtime DLLs are extracted. `src/build.sh` /
`src/build.ps1` then compile MinHook, ImGui (core, Win32 backend, and the patched DX12 backend) and
the mod sources, link `amd_fidelityfx_dx12.dll` (statically, exporting only the five `ffx*` entries
from `src/exports.def`), and assemble `dist/`:

```
dist/
  amd_fidelityfx_dx12.dll  fsr2dlss.ini  INSTALL.md  VERSIONS.txt
  licenses/   streamline, DLSS (NVIDIA RTX SDKs), Reflex, Dear ImGui, MinHook
  streamline/ sl.interposer sl.common sl.dlss sl.dlss_g sl.pcl sl.reflex nvngx_dlss nvngx_dlssg
```

`.github/workflows/build.yml` runs this on pull requests (MSYS2 on `windows-latest`) and uploads
`dist/` as an artifact. `.github/workflows/release.yml` runs on every push to `main`: it calls the
build workflow, zips the artifact as `fsr2dlss-v<VERSION>.<run>.zip` with a SHA-256 file, and creates
a GitHub Release tagged `v<VERSION>.<run>` (the `VERSION` file holds the major.minor).

Updating Streamline/DLSS: replace `third_party/streamline/include` with the new SDK's headers, put
the new version and zip hash in `SDK_VERSION`, build, and test. Both the plugins and the `nvngx_*`
models come from that one zip.

## Credits and licenses

The idea traces to LukeFZ's FSR2Streamline; this is a from-scratch rebuild for the FSR 3.1 FidelityFX
API. It uses NVIDIA Streamline, Dear ImGui and MinHook, all MIT. The `nvngx_*` models are NVIDIA
binaries redistributed under the NVIDIA RTX SDKs license as part of the release package.
