# Streamline 2.2.0 DLSS-G integration spec (for the FfxApi proxy) — condensed

COMPILE AGAINST sl2.2 headers (C:\Users\Adam\fsrb\sdk\sl2.2). 2.12 differs (no sl.pcl at 2.2; markers via sl.reflex).
Feature ids: kFeatureDLSS=0, kFeatureNIS=2, kFeatureReflex=3, kFeatureDLSS_G=1000.
Functions are C exports of sl.interposer.dll; feature funcs (slDLSSGSetOptions, slReflexSetMarker...) NOT exports —
resolve via slGetFeatureFunction AFTER slSetD3DDevice. Use PFun_* typedefs from headers for GetProcAddress.

## Init (once, on first ffxCreateContext; device from backend desc 0x00000002 +0x10)
- Load streamline\sl.interposer.dll; GetProcAddress: slInit, slShutdown, slSetD3DDevice, slUpgradeInterface,
  slGetNativeInterface, slIsFeatureSupported, slGetFeatureRequirements, slSetTag, slSetConstants,
  slGetNewFrameToken, slGetFeatureFunction.
- Preferences: flags |= eUseManualHooking (1<<2); featuresToLoad={kFeatureDLSS_G,kFeatureReflex} n=2;
  pathsToPlugins=&<streamlineDirW> n=1; renderAPI=eD3D12; engine=eCustom; logLevel=eVerbose; showConsole=true;
  pathToLogsAndData=<dir>; applicationId (any, e.g. 0x1337 / reuse 100721531 LoP id from sl.log).
- slInit(pref, kSDKVersion=2.2.0 magic). slSetD3DDevice(device). THEN slGetFeatureFunction for DLSS_G/Reflex funcs.
- Support: LUID=device->GetAdapterLuid(); AdapterInfo{deviceLUID=&luid,size=8}; slIsFeatureSupported(kFeatureDLSS_G,ai).
  slGetFeatureRequirements(kFeatureDLSS_G,req) -> req.flags: eVSyncOffRequired(1<<3), eHardwareSchedulingRequired(1<<4).

## Swapchain substitution at ffxCreateContext NEW_DX12 0x00030005 (the crux)
- have: out IDXGISwapChain4** , DXGI_SWAP_CHAIN_DESC* (old desc; convert to DESC1+hwnd), IDXGIFactory*, ID3D12CommandQueue*, device.
- slUpgradeInterface((void**)&factory) -> factory becomes SL proxy (manual hooking).
- factory->CreateSwapChainForHwnd(queue, hwnd, &desc1, fsdesc, null, &sc1)  [SL auto-attaches DLSS-G]
- sc1->QueryInterface(IID IDXGISwapChain4,&sc4); return sc4 via *swapchain.
- MUST call sc4->GetCurrentBackBufferIndex() every frame else status=eFailGetCurrentBackBufferIndexNotCalled.
- Flip-model only; recreate SC when DLSS-G toggles. Present through proxy sc4.

## Per frame
- options: DLSSGOptions{mode=eOn} (2.2: {eOff,eOn}); slDLSSGSetOptions(vp{0},opt).
- tags via slSetTag(vp, tags, n, cmdList): kBufferTypeDepth=0, kBufferTypeMotionVectors=1 REQUIRED;
  HUDLessColor=2 + UIColorAndAlpha=23 recommended (UI not R10G10B10A2). lifecycle=eValidUntilPresent.
  Resource{eTex2d, ptr, D3D12_RESOURCE_STATES state} — state must be correct at Present.
  -> depth+MV from FRAMEGEN PREPARE 0x00020004; HUDLessColor from FRAMEGEN CONFIGURE 0x00020002.
- constants (sl_consts Constants v1) via slSetConstants(c, *frame, vp): REQUIRED cameraViewToClip, clipToCameraView,
  clipToPrevClip, prevClipToClip; jitterOffset(px); mvecScale (NDC: {1/W,1/H} if MV in px, {1,1} if NDC);
  cameraPos/Up/Right/Fwd; cameraNear/Far/FOV/AspectRatio; depthInverted=eTrue; cameraMotionIncluded=eTrue;
  reset on cut. FFX gives near/far/fov ONLY -> build projection from fov+aspect+near/far(inverted,infinite);
  clipToPrevClip=identity + cameraMotionIncluded=eTrue (MVs carry camera motion). garbage->eFailCommonConstantsInvalid.
- frame token: slGetNewFrameToken(ft, &gameFrameIndex) — index MUST match across constants/tags/markers.
- Reflex (MANDATORY for DLSS-G): once slReflexSetOptions{mode=eLowLatency}. each frame, same ft:
  slReflexSleep(*ft); markers eSimulationStart/End, eRenderSubmitStart/End, ePresentStart .. Present .. ePresentEnd.
  ReflexMarker enum(2.2): eSimulationStart=0,eSimulationEnd,eRenderSubmitStart,eRenderSubmitEnd,ePresentStart,ePresentEnd,...

## Gotchas
- HDR: DLSS-G supports R8G8B8A8 (SDR) and R10G10B10A2 HDR10. NOT scRGB FP16 (R16F) -> eFailHDRFormatNotSupported.
  LoP FG backBufferFormat captured = 17 (R10G10B10A2) -> OK. Confirm swapchain DESC format; force HDR10 if needed.
- No MSAA backbuffer. GetCurrentBackBufferIndex each frame. HAGS on if eHardwareSchedulingRequired.
- Production interposer verifies PLUGIN signatures (user's are NV-signed -> ok). Host (our DLL) not checked.
- Interpolation is INTERNAL to SL Present — no interpolation cmdlist/texture to hand back. The game's per-frame
  FGSWAP queries 0x00030003 (InterpolationCommandList) / 0x00030004 (InterpolationTexture) must be satisfied with
  throwaway objects (a real but ignored cmd list + a dummy texture) so the game's FFX FG dispatch path doesn't crash.
