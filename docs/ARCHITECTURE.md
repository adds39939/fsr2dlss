# fsr2dlss — Architecture & Design

A from‑scratch mod that replaces **AMD FSR 3.1** (upscaling + frame generation) with
**NVIDIA DLSS Super‑Resolution + DLSS Frame Generation (DLSS‑G)** in games that use AMD's
unified **FidelityFX API** (`amd_fidelityfx_dx12.dll`). Developed and proven on **Lies of P**
(UE4, D3D12, HDR10) on an **RTX 5090 (Blackwell)**.

This is the spiritual successor to LukeFZ's dead "FSR2Streamline" Nexus mod, rebuilt for the
FSR **3.1.1 FfxApi** era. The novel part is getting **real DLSS‑G frame generation** to actually
engage under injection on Blackwell — see [§7 The Key Discovery](#7-the-key-discovery-why-streamline-271).

---

## 1. What it does

The game asks AMD's FidelityFX to (a) upscale each rendered frame and (b) generate interpolated
frames. We interpose that DLL and instead drive NVIDIA Streamline:

| Game requests (FfxApi)          | We do instead                                  |
|---------------------------------|------------------------------------------------|
| FSR upscale dispatch            | **DLSS Super‑Resolution** (`slEvaluateFeature`)|
| FSR frame‑gen swapchain + present | **DLSS‑G** swapchain substitution + present   |

Both are confirmed active via NVIDIA's own on‑screen indicators and Streamline telemetry
(`numFramesActuallyPresented == 2`, `sl.dlss`/`sl.dlss_g` plugins loaded).

---

## 2. The interposed contract (FSR 3.1 FfxApi)

`amd_fidelityfx_dx12.dll` exports 5 unified entry points; we export the same 5 and forward the
ones we don't handle to the **renamed original** (`amd_fidelityfx_dx12.amd.dll`):

```
ffxCreateContext  ffxConfigure  ffxQuery  ffxDispatch  ffxDestroyContext
```

Work is identified by a **type id** in each desc header. The three effect families:

- **UPSCALE**   `0x00010000` (create) / `0x00010001` (dispatch) — the upscale pass
- **FRAMEGEN**  `0x00020000` family — FG context, `PREPARE 0x00020004` (depth+MV+camera), `dispatch 0x00020003`
- **FGSWAPCHAIN** `0x00030000` family — `NEW_DX12 0x00030005` creates the FG swapchain (our substitution point)

Struct layouts (offsets) were reverse‑engineered from live captures and are documented in
[`src/CONTRACT.md`](../src/CONTRACT.md). Key facts about this game's data:

- Depth is **inverted + infinite‑far**; motion vectors are **render‑res, UV‑space, camera‑motion‑included**.
- FFX provides camera **near/far/fov scalars, not matrices** → we build a reversed‑Z projection.
- HDR10 PQ (R10G10B10A2) backbuffer; upscale color buffers are FP16 (R16G16B16A16_FLOAT).
- `motionVectorScale == renderSize` ⇒ MVs are in UV space ⇒ Streamline `mvecScale = {1,1}`
  (passing the FFX scale blows up reprojection — this caused an early "everything is red" bug).

---

## 3. High‑level architecture

```
                game (LOP-Win64-Shipping.exe)
                          │  loads by name
                          ▼
        amd_fidelityfx_dx12.dll  ◄── OUR PROXY (this repo)
          │  proxy.cpp routes by type id
          ├── UPSCALE dispatch  ──►  slbridge_upscale()  ──► SL DLSS (super-res)
          ├── FGSWAPCHAIN NEW    ──► slbridge_create_swapchain() ──► SL DLSS-G swapchain
          ├── FRAMEGEN PREPARE   ──► feedDLSSG() (depth/MV/constants/markers)
          ├── FRAMEGEN dispatch  ──► slbridge_dispatch() (hudless/UI tags, colorspace)
          └── everything else    ──►  forward to amd_fidelityfx_dx12.amd.dll (real AMD)
                          │
                          ▼
             streamline\  (matched SL 2.7.1 + nvngx 310.1)
             nvhook.cpp   (blocks NvAPI_D3D12_SetFlipConfig → software pacer)
```

The proxy owns nothing heavy; all NVIDIA work lives in the Streamline bridge.

---

## 4. Components

### `src/proxy.cpp` — the transparent FfxApi proxy
- `DllMain`/`ensure()` lazily `LoadLibrary`s the renamed real DLL and caches its 5 entry points.
- Exports the 5 `ffx*` functions. Each derefs the `ffxContext` and routes: if the handle is one
  **we created** (`slbridge_is_mine`), dispatch to the bridge; else forward to AMD.
- The **UPSCALE dispatch** (`0x00010001`) is the first FfxApi call of the frame. We try
  `slbridge_upscale(desc)`; if it runs DLSS we return OK (skip AMD FSR); if it declines we
  fall through and forward to AMD (safe FSR fallback).

### `src/slbridge.cpp` — the Streamline bridge (the core)
Stateful. Responsibilities:
- **Init** (`slbridge_init`): `LoadLibrary sl.interposer.dll`, resolve exports, `slInit` +
  `slSetD3DDevice` (manual hooking, late device is fine), `slIsFeatureSupported` for DLSS + DLSS‑G,
  resolve feature functions. **Installs the flip‑metering block first** (see nvhook).
- **DLSS Super‑Resolution** (`slbridge_upscale`) — [§5](#5-dlss-super-resolution).
- **DLSS‑G swapchain substitution** (`slbridge_create_swapchain`) — upgrade the DXGI **factory**
  (not the queue) via `slUpgradeInterface`, `CreateSwapChainForHwnd` with the **native** queue →
  Streamline returns a DLSS‑G‑capable swapchain. Hand it to the game and **vtable‑hook `Present`
  in place** (slot 8 / `Present1` slot 22) to bracket present markers. **Do NOT wrap it in a COM
  proxy** — that breaks SL's swapchain identity.
- **Per‑frame DLSS‑G feed** (`feedDLSSG`, from FRAMEGEN PREPARE) — [§6](#6-dlss-g-frame-generation).
- **Version‑adaptive** — reads `sl_hostver.txt` and gates the tagging/options API by SL version
  (see [§7](#7-the-key-discovery-why-streamline-271)).

### `src/nvhook.cpp` — the flip‑metering block (critical enabler)
Uses **MinHook** to inline‑hook `nvapi64.dll!nvapi_QueryInterface` and return `nullptr` for exactly
one id — `NvAPI_D3D12_SetFlipConfig` (`0xF3148C42`) — forwarding every other query untouched.
This disables **Blackwell hardware flip metering**, forcing DLSS‑G onto its **software pacer**,
which actually presents the interpolated frame under injection. (Same trick as OptiScaler's
`DisableFlipMetering`.) Independent Flip is unaffected. Installed **before `slInit`**.

---

## 5. DLSS Super‑Resolution

At the UPSCALE dispatch (`slbridge_upscale`), read the FfxApi upscale desc (offsets in CONTRACT.md):
color(`+0x18`), depth(`+0x48`), motion vectors(`+0x78`), output(`+0x138`), jitter(`+0x168`),
renderSize(`+0x178`). Each `FfxApiResource` is 48 bytes `{ void* res(+0); desc 8×u32(+8); u32 state(+40) }`.

Then:
1. `slDLSSSetOptions(vp1, {mode, outputWidth/Height, colorBuffersHDR=eTrue, useAutoExposure=eTrue})`,
   only on change. **Mode is auto‑picked from the render:output ratio** (1.0→DLAA, 0.67→Quality,
   0.58→Balanced, 0.5→Performance, …), so any in‑game FSR preset maps to the right DLSS mode.
2. `slSetConstants(jitter, mvecScale={1,1}, depthInverted=eTrue, …)` on an SR frame token.
3. `slSetTag(vp1, {ScalingInputColor=3, Depth=0, MotionVectors=1, ScalingOutputColor=4},
   eValidUntilEvaluate, cmdList)`.
4. `slEvaluateFeature(kFeatureDLSS, token, {&vp1}, 1, cmdList)` — records DLSS onto the game's own
   command list, writing into the game's **own output buffer**, which the downstream DLSS‑G/present
   path then consumes.

SR runs on its **own viewport (vp1) and token space**, independent of DLSS‑G (vp0), so their
constants never collide. If evaluate fails, return `false` → proxy forwards to AMD FSR (safe).
Runtime toggle: create `dlss_sr_off.flag` to force FSR.

---

## 6. DLSS‑G Frame Generation

1. **Swapchain**: at FGSWAPCHAIN `NEW_DX12`, substitute an SL DLSS‑G swapchain (see §4) and
   vtable‑hook its `Present`.
2. **Per frame** (`feedDLSSG`, from FRAMEGEN PREPARE, keyed by the game's `frameID`):
   - Get a frame token; emit the **full PCL marker cadence** (`eSimulationStart/End`,
     `eRenderSubmitStart`, then `eRenderSubmitEnd`+`ePresentStart` in the present hook,
     `ePresentEnd` after). Full cadence is required for the software pacer.
   - Tag **Depth + MotionVectors** (`eValidUntilPresent`, explicit extents); build reversed‑Z
     constants (`depthInverted=eTrue`, `cameraMotionIncluded=eTrue`, `mvecScale={1,1}`).
   - `slDLSSGSetOptions(mode=eOn, numFramesToGenerate=1, …)`; `slReflexSetOptions(eLowLatency,
     useMarkersToOptimize=false)`; `slReflexSleep` after present.
3. At FRAMEGEN dispatch: set the swapchain colorspace once (HDR10 PQ = `G2084_P2020`), optionally
   tag HUDLessColor/UIColorAndAlpha.
4. The **present hook** drives DLSS‑G: the game presents once, SL presents twice (real + generated).

`slDLSSGGetState().numFramesActuallyPresented == 2` is the ground‑truth "it's doubling" signal.

---

## 7. The Key Discovery: why Streamline 2.7.1

This is the crux and the novel result. DLSS‑G **would not generate a single frame** on the
**SL 2.11** plugins under injection, despite everything reporting healthy (`status=eOk`, VRAM
allocated, flip‑metering "good feedback", all tags/markers/constants correct, and — proven with
PresentMon — the swapchain reaching **Hardware Independent Flip**). Every configuration produced
`numFramesActuallyPresented == 1`. OptiScaler's own DLSS‑G output backend *also* fails on this game
(its swapchain `Present` returns `DXGI_ERROR_INVALID_CALL`).

The only version where DLSS‑G ever generated was **SL 2.2** — but 2.2's `sl.dlss_g` can't decode
the 310.x model output, so the image came out **red**.

The fix is to use an **intermediate Streamline version: 2.7.1** (sourced from Cyberpunk 2077,
matched with `nvngx_dlssg`/`nvngx_dlss` **310.1**). 2.7.1 is:
- **New enough** that its `sl.dlss_g` decodes the 310.1 model with **correct colors**.
- **Old enough** that it **predates hardware flip metering** (added 2.7.2) and the **frame‑based
  tagging API** `slSetTagForFrame` (added 2.7.30). So it uses the **software pacer + global
  `slSetTag`** — the exact path that generated on 2.2.

Combined with the **flip‑metering block** (nvhook) and the **full marker cadence**, DLSS‑G engages
and presents at ~2×.

**The bridge is version‑adaptive** so future SL versions can be tested without a rebuild:
`slbridge_init` reads **`sl_hostver.txt`** (e.g. `2.7.1`), reports that host SDK version to `slInit`,
and gates `eUseFrameBasedResourceTagging` + v5 `DLSSGOptions` to `>= 2.7.30` (else global `slSetTag`
+ v1 options). Just swap the `streamline\` plugin set and edit the txt file.

> Takeaway for future work: if NVIDIA fixes injected DLSS‑G on newer Streamline (or OptiScaler's
> DLSS‑G backend stabilizes), this bridge can move forward by changing `sl_hostver.txt` + plugins.
> Until then, **2.7.1 is the sweet spot**.

---

## 8. Build

Toolchain: **MinGW‑w64 GCC** (winlibs UCRT build; has d3d12/dxgi headers), **NVIDIA Streamline
2.2 SDK headers** (for compile — the ABI is forward‑compatible), and **MinHook** (MIT).

```
# 1) get deps (not committed): MinGW-w64 -> mingw64/ ; Streamline 2.2 headers -> sdk/sl2.2/ ;
#    MinHook source -> minhook_src/  (git clone https://github.com/TsudaKageyu/minhook)
# 2) compile MinHook C sources once into mh_obj/*.o (gcc -c src/*.c src/hde/*.c)
# 3) build the proxy:
g++ -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
    -I sdk/sl2.2 -I minhook_src/include \
    -o src/amd_fidelityfx_dx12.dll \
    src/proxy.cpp src/slbridge.cpp src/nvhook.cpp \
    mh_obj/buffer.o mh_obj/hook.o mh_obj/trampoline.o mh_obj/hde32.o mh_obj/hde64.o \
    src/exports.def -lkernel32 -luser32
```
See [`src/build.ps1`](../src/build.ps1).

---

## 9. Install & runtime files

`src/install.ps1` renames the game's original `amd_fidelityfx_dx12.dll` → `amd_fidelityfx_dx12.amd.dll`
and drops our proxy in its place. Required alongside it in the game's `Win64\` folder:

- `streamline\` — a **matched SL 2.7.1** set: `sl.interposer, sl.common, sl.dlss, sl.dlss_g,
  sl.pcl, sl.reflex` (all 2.7.1) + `nvngx_dlss, nvngx_dlssg` (310.1). **These are proprietary
  NVIDIA redistributables — source them from a game** (e.g. Cyberpunk 2077 `bin\x64`). Not committed.
- `sl_hostver.txt` — contains `2.7.1`.

Runtime toggles (no rebuild): `dlss_sr_off.flag` (force FSR upscaling), `fg_interp.flag`
(show only generated frames — diagnostic), `fg_fpscap.txt` (Reflex FPS cap). NVIDIA on‑screen
indicators via `src/fg_overlay.ps1` (admin; sets `DLSSG_IndicatorText`/`ShowDlssIndicator`).

In‑game: set **Upscaling = FSR** and **Frame Generation = ON**. The game thinks it's running FSR;
we substitute DLSS underneath. (Steam's overlay reads the menu setting, so it mislabels it "FSR" —
cosmetic only; NVIDIA's indicators confirm DLSS.)

---

## 10. Known limitations / future work

- **`hudTagged=0`** on the 2.7.1 global‑tag path — no HUDless/UI layer is fed to DLSS‑G, so fast
  motion may show slight UI ghosting. Could add `slSetTag(HUDLessColor + UIColorAndAlpha)`.
- **Hardware flip metering is disabled** (software pacer). This is currently required for injected
  DLSS‑G on Blackwell; a future Streamline may allow HW metering under injection.
- **Hardcoded absolute paths** (game `Win64\` dir) in a few string constants — fine for the target
  install, but parameterize for portability.
- **Newer SL versions** (2.7.30+, 2.11) don't engage under injection here; revisit if NVIDIA/OptiScaler
  change that.

---

## 11. Credits & licenses

- Concept: LukeFZ "FSR2Streamline" (dead; this is a from‑scratch 3.1 FfxApi rebuild).
- **NVIDIA Streamline** (MIT) — the SL runtime + headers.
- **MinHook** by Tsuda Kageyu (MIT) — inline hooking for the flip‑metering block.
- Flip‑metering block technique mirrors **OptiScaler**'s `DisableFlipMetering`.
- FfxApi struct layouts reverse‑engineered from runtime captures (see `src/CONTRACT.md`).

NVIDIA `nvngx_*` / `sl.*` binaries are **not** included — they are NVIDIA redistributables to be
sourced from a game you own.
