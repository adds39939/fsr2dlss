// Interface between the FfxApi proxy (proxy.cpp) and the Streamline bridge
// (slbridge.cpp). All functions are safe to call before init (they no-op/return
// false) so the proxy can always fall back to forwarding to the real AMD DLL.
#pragma once
#include <cstdint>

extern "C" {

// One-time Streamline init (manual hooking) against the game's existing device.
// Returns true once DLSS-G is usable. Idempotent.
bool  slbridge_init(void* d3d12Device, const wchar_t* streamlineDir);

// Intercept ffxCreateContext for the FG swapchain (NEW_DX12 0x00030005).
// oldDesc = DXGI_SWAP_CHAIN_DESC*, returns an opaque context handle to hand back
// to the game as ffxContext (and writes the SL swapchain into *outSwapchain), or
// null to signal "couldn't substitute, please forward to AMD".
void* slbridge_create_swapchain(void* oldDesc, void* factory, void* queue, void** outSwapchain);

// Intercept ffxCreateContext for the frame-generation context (0x00020001).
// Returns an opaque context handle (never forwarded to AMD).
void* slbridge_create_framegen(const void* createDescChain);

// Substitute the game's FSR UPSCALE context (0x00010000) with a dummy so NO real AMD FSR
// upscaler session is ever created (Steam may label "FSR" from that live session). We already
// run DLSS on the upscale dispatch, so AMD's upscaler is unused. Returns a dummy handle.
void* slbridge_create_upscale(void);
// True when we should stub the UPSCALE context: opt-in via stub_fsr.flag AND DLSS-SR active
// (else we still need AMD's real upscaler). Read live.
bool  slbridge_want_stub_upscale(void);

// Is this ffxContext handle one we own?
bool  slbridge_is_mine(void* handle);

// Called once per frame on the first FfxApi call of the frame (the UPSCALE dispatch, which is
// forwarded to AMD) so the injected Reflex loop can open the frame (eSimulationStart) at the
// real frame start. No-op until Streamline is ready. Safe to call always.
void  slbridge_frame_start(void);

// Intercept the FSR UPSCALE dispatch (0x00010001) and run DLSS super-resolution instead.
// Returns true if DLSS did the upscale (caller must NOT forward to AMD); false to fall back to FSR.
bool  slbridge_upscale(const void* dispatchDesc);

// Per-frame handlers for our contexts. Return an ffxReturnCode_t (0 == OK).
uint32_t slbridge_configure(void* handle, const void* desc);
uint32_t slbridge_query(void* handle, void* desc);
uint32_t slbridge_dispatch(void* handle, const void* desc);
void     slbridge_destroy(void* handle);

} // extern "C"
