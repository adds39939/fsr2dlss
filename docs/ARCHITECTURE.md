# fsr2dlss architecture

fsr2dlss is a proxy for `amd_fidelityfx_dx12.dll`. Lies of P loads that DLL by name to run FSR 3.1
upscaling and frame generation. The proxy forwards the calls it does not handle to the renamed real
AMD DLL and routes the upscale and frame-generation work to NVIDIA Streamline instead, so the game
runs DLSS Super-Resolution and DLSS-G Multi-Frame Generation while still believing it uses FSR.

The runtime stack is fixed: the Streamline 2.7.4 frame-generation plugins with nvngx 310.1, plus the
2.7.1 DLSS super-resolution plugin. On an RTX 50-series card DLSS-G runs on Blackwell hardware flip
metering, which paces the generated frames in the display engine and gives real 3x (and 4x)
multi-frame generation.

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
           streamline\  (Streamline 2.7.4 + nvngx 310.1, plus sl.dlss 2.7.1)
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

### slbridge.cpp

The Streamline bridge and the core of the mod. It initializes Streamline against the game's existing
D3D12 device with manual hooking, resolves the DLSS and DLSS-G feature functions, and reports host SDK
2.7.4 to `slInit`. It uses global `slSetTag` (the tagging model 2.7.4 uses) and version 5
`DLSSGOptions`, whose `numFramesToGenerate` field selects the multi-frame multiplier.

- DLSS super-resolution (`slbridge_upscale`), described below.
- DLSS-G swapchain substitution (`slbridge_create_swapchain`): upgrade the DXGI factory with
  `slUpgradeInterface`, create the swapchain with the game's native command queue, and hand the
  resulting DLSS-G swapchain back to the game. `Present` and `Present1` are hooked in place on that
  swapchain's vtable to bracket the frame with present markers. The swapchain is never wrapped in a
  separate COM object, because that breaks the identity Streamline's frame generation binds to.
- Per-frame feed (`feedDLSSG`, driven from FRAMEGEN PREPARE, keyed by the game's frame id), below.

### nvhook.cpp

Inline-hooks `nvapi64.dll!nvapi_QueryInterface` with MinHook. When `FlipMetering = software` it
returns null for `NvAPI_D3D12_SetFlipConfig` (id `0xF3148C42`) and forwards everything else, which
drops DLSS-G onto its software pacer for clean 2x. When `FlipMetering = hardware` (the default) it
blocks nothing, and the Blackwell display engine's flip queue paces the generated frames for true
3x/4x. It is installed before `slInit`.

## DLSS super-resolution

At the UPSCALE dispatch the bridge reads the upscale descriptor (offsets in CONTRACT.md): color,
depth, motion vectors, output, jitter, and render size, each an `FfxApiResource`. Then it:

1. Calls `slDLSSSetOptions` on its super-resolution viewport, on change only. The DLSS quality mode is
   derived from the render-to-output ratio, so any in-game FSR preset maps to the matching DLSS mode:
   1.0 is DLAA, 0.67 Quality, 0.58 Balanced, 0.5 Performance, and so on.
2. Sets constants (jitter, `mvecScale = {1, 1}`, inverted depth).
3. Tags the scaling input color, depth, motion vectors, and output.
4. Calls `slEvaluateFeature(kFeatureDLSS)` on the game's own command list, writing into the game's
   output buffer, which the downstream frame-generation and present path then consumes.

Super-resolution runs on its own viewport and frame token, separate from DLSS-G, so their constants
never collide. Its own frame token is also what lets the Steam overlay recognize DLSS is active. If
the evaluate fails the bridge returns false and the proxy forwards the upscale to FSR.

## DLSS-G frame generation

The frame-generation swapchain is substituted at FGSWAPCHAIN `NEW_DX12`, and its `Present` is hooked
in place.

Each frame, from FRAMEGEN PREPARE, `feedDLSSG` gets a frame token and emits the full PCL marker
cadence: simulation start and end, render-submit start, then render-submit end and present start in
the present hook, and present end after the real present. It tags depth and motion vectors with
explicit extents, builds reversed-Z constants, and calls `slDLSSGSetOptions` with the requested
`numFramesToGenerate`. Reflex options and sleep run around the present. At the FRAMEGEN dispatch the
bridge sets the swapchain color space once (HDR10 PQ) and can optionally tag HUD-less and UI layers.

The game presents once per rendered frame; Streamline presents the real frame plus the generated ones
through the hooked swapchain, and the display engine meters them. `slDLSSGGetState` reports
`numFramesActuallyPresented`, which the bridge tracks to confirm the multiplier.

## Build

The toolchain is MinGW-w64 GCC (a UCRT build with the d3d12 and dxgi headers), the Streamline SDK
headers under `third_party/sl2.2`, and MinHook. The proxy compiles against the 2.2 headers; the ABI is
forward-compatible with the 2.7.4 runtime.

```
g++ -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
    -I third_party/sl2.2 -I minhook_src/include \
    -o src/amd_fidelityfx_dx12.dll \
    src/proxy.cpp src/slbridge.cpp src/nvhook.cpp \
    mh_obj/buffer.o mh_obj/hook.o mh_obj/trampoline.o mh_obj/hde32.o mh_obj/hde64.o \
    src/exports.def -lkernel32 -luser32
```

See [`src/build.ps1`](../src/build.ps1). MinHook is cloned to `minhook_src/` and its C sources are
compiled once into `mh_obj/`.

## Credits and licenses

The idea traces to LukeFZ's FSR2Streamline; this is a from-scratch rebuild for the FSR 3.1 FidelityFX
API. It uses NVIDIA Streamline and MinHook, both MIT. The `nvngx_*` and `sl.*` binaries are NVIDIA
redistributables and are not included here.
