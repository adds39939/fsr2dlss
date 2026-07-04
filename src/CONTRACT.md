# Lies of P FfxApi (FSR 3.1.1) runtime contract — empirical + struct model

Captured from a real game run (ffx_bridge.log). Display 3840x2160, HDR (R10G10B10A2 backbuffer),
depth INVERTED + INFINITE far. FG-context flags = 0x38 = DEPTH_INVERTED|DEPTH_INFINITE|HIGH_DYNAMIC_RANGE.

## Call graph
SETUP (once):
  ffxCreateContext  FGSWAPCHAIN NEW_DX12   0x00030005  (+backend 0x00000002 holds ID3D12Device)
  ffxQuery          UPSCALE GetUpscaleRatio 0x00010002
  ffxCreateContext  UPSCALE                0x00010000  (+backend 0x00000002)
  ffxCreateContext  FRAMEGEN               0x00020001  (+backend 0x00000002)  flags=0x38
PER FRAME:
  ffxConfigure  UPSCALE  KEYVALUE          0x00010007   (key/u64/ptr)
  ffxDispatch   UPSCALE                    0x00010001   <-- the upscale (color/depth/MV/output)
  ffxConfigure  FRAMEGEN                   0x00020002   (swapChain, HUDLessColor, callbacks, enabled, frameID)
  ffxDispatch   FRAMEGEN PREPARE           0x00020004   <-- depth+MV+camera scalars (DLSS-G inputs!)
  ffxDispatch   FRAMEGEN                   0x00020003   <-- presentColor + outputs[4]
  ffxQuery      FGSWAP InterpolationCmdList 0x00030003  (game wants ID3D12GraphicsCommandList*)
  ffxQuery      FGSWAP InterpolationTexture 0x00030004  (game wants FfxApiResource* to write interp into)
DESTROY: ffxDestroyContext x2

## Base header (all descs): { uint64 type; void* pNext; }  = 16 bytes
## FfxApiResource = 48 bytes: { void* resource(+0); FfxApiResourceDescription desc(+8, 8xu32: type,format,width,height,depth,mipCount,flags,usage); u32 state(+40); pad(+44) }
##   format: 4=R16G16B16A16_FLOAT, 28=R32_FLOAT(depth), 17=R10G10B10A2_UNORM. state: 1=COMMON 2=UAV 4=COMPUTE_READ 0x80=PRESENT
## Backend DX12 desc (type 0x00000002): { header(16); ID3D12Device* device(+16) }

## ffxDispatchDescUpscale (0x00010001) size ~0x1B0:
  +0x00 header; +0x10 commandList; +0x18 color; +0x48 depth; +0x78 motionVectors; +0xA8 exposure;
  +0xD8 reactive; +0x108 transparencyAndComposition; +0x138 output;
  +0x168 jitterOffset(2f); +0x170 motionVectorScale(2f); +0x178 renderSize(2u); +0x180 upscaleSize(2u);
  +0x188 enableSharpening(bool); +0x18C sharpness(f); +0x190 frameTimeDelta(f); +0x194 preExposure(f);
  +0x198 reset(bool); +0x19C cameraNear(f); +0x1A0 cameraFar(f); +0x1A4 cameraFovV(f);
  +0x1A8 viewSpaceToMeters(f); +0x1AC flags(u32)

## ffxCreateContextDescFrameGenerationSwapChainNewDX12 (0x00030005):
  +0x00 header; +0x10 IDXGISwapChain4** swapchain(OUT); +0x18 DXGI_SWAP_CHAIN_DESC* desc;
  +0x20 IDXGIFactory* dxgiFactory; +0x28 ID3D12CommandQueue* gameQueue
  (verified: factory=0x024814623e60, queue=0x024814 9837f0, device from pNext backend)

## ffxConfigureDescFrameGeneration (0x00020002):
  +0x00 header; +0x10 void* swapChain; +0x18 presentCallback; +0x20 presentCbUserCtx;
  +0x28 frameGenerationCallback; +0x30 frameGenCbUserCtx; +0x38 frameGenerationEnabled(bool);
  +0x39 allowAsyncWorkloads(bool); +0x40 HUDLessColor(FfxApiResource,48); +0x70 flags(u32);
  +0x74 onlyPresentGenerated(bool); +0x78 generationRect(4i); +0x88 frameID(u64)

## ffxDispatchDescFrameGenerationPrepare (0x00020004)  <-- DLSS-G camera/depth/MV source:
  +0x00 header; +0x10 frameID(u64); +0x18 flags(u32); +0x20 commandList; +0x28 renderSize(2u);
  +0x30 jitterOffset(2f); +0x38 motionVectorScale(2f); +0x40 frameTimeDelta(f); +0x44 unused_reset(bool);
  +0x48 cameraNear(f); +0x4C cameraFar(f); +0x50 cameraFovV(f); +0x54 viewSpaceToMeters(f);
  +0x58 depth(FfxApiResource,48); +0x88 motionVectors(FfxApiResource,48)

## ffxDispatchDescFrameGeneration (0x00020003):
  +0x00 header; +0x10 commandList; +0x18 presentColor(48); +0x48 outputs[4](4x48=192);
  +0x108 numGeneratedFrames(u32); +0x10C reset(bool); +0x110 backbufferTransferFunction(u32);
  +0x114 minMaxLuminance[2](2f); +0x11C generationRect(4i); +0x130 frameID(u64)

## DLSS-G mapping plan
- depth+MV from PREPARE(0x00020004). MVs are render-res, FULL (include camera motion) -> SL Constants.cameraMotionIncluded=true, mvecScale from desc.
- FFX gives near/far/fov scalars, NOT matrices. depthInverted=true, depth infinite. minMaxLum from 0x00020003.
- HUDLessColor from CONFIGURE(0x00020002); presentColor from DISPATCH(0x00020003).
- Swapchain substitution at NEW_DX12(0x00030005): create SL DLSS-G swapchain from {device,factory,queue,desc,hwnd}; return via *swapchain.
- Satisfy per-frame InterpolationCmdList(0x00030003)/InterpolationTexture(0x00030004) so game's FFX FG dispatch path doesn't crash (SL does interpolation internally at Present).
