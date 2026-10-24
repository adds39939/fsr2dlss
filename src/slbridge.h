// Interface between the FfxApi proxy (proxy.cpp) and the Streamline bridge
// (slbridge.cpp). All functions are safe to call before init (they no-op/return
// false) so the proxy can always fall back to forwarding to the real AMD DLL.
#pragma once
#include <cstdint>

// Live counters the bridge keeps for the in-game overlay. Written on the render/present
// thread, read by the overlay on the present thread; plain fields, no locking needed.
struct BridgeStats {
    float    baseFps        = 0;    // real (game-rendered) frames per second
    float    displayFps     = 0;    // frames reaching the display per second (base x multiplier)
    uint32_t fgApplied      = 0;    // 0 = DLSS-G off, 1 = dynamic, N>=2 = fixed Nx
    uint32_t fgMaxGen       = 1;    // numFramesToGenerateMax reported by the card (1 = 2x only)
    bool     fgDynamicOk    = false;// card reports Dynamic Multi Frame Generation support
    bool     fgGameEnabled  = false;// the game has frame generation turned on
    uint32_t fgStatus       = 0;    // sl::DLSSGStatus bits (0 = ok)
    uint64_t fgVramBytes    = 0;
    int      srMode         = -1;   // sl::DLSSMode in use, -1 = DLSS super-resolution not running
    int      srPreset       = 0;    // sl::DLSSPreset in use
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    bool     srActive       = false;// last upscale ran through DLSS
    bool     frameTagging   = false;// using slSetTagForFrame (false = legacy slSetTag)
    uint32_t slVersion[3]   = {0,0,0};
};
extern BridgeStats g_stats;

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

// Is this ffxContext handle one we own?
bool  slbridge_is_mine(void* handle);

// Called once per frame on the first FfxApi call of the frame (the UPSCALE dispatch, which is
// forwarded to AMD) so the injected Reflex loop can open the frame (eSimulationStart) at the
// real frame start. No-op until Streamline is ready. Safe to call always.
void  slbridge_frame_start(void);

// Intercept the FSR UPSCALE dispatch (0x00010001) and run DLSS super-resolution instead.
// Returns true if DLSS did the upscale (caller must NOT forward to AMD); false to fall back to FSR.
bool  slbridge_upscale(const void* dispatchDesc);

// The HUD-less colour buffer the game passed to FSR's frame-generation configure this frame, if any:
// the native ID3D12Resource, its D3D12_RESOURCE_STATES at present time, and its size. Lets the
// overlay build DLSS-G's UI layer from (back buffer - hud-less) so the game's own HUD is protected too.
bool  slbridge_game_hudless(void** resource, uint32_t* d3d12State, uint32_t* width, uint32_t* height);

// Per-frame handlers for our contexts. Return an ffxReturnCode_t (0 == OK).
uint32_t slbridge_configure(void* handle, const void* desc);
uint32_t slbridge_query(void* handle, void* desc);
uint32_t slbridge_dispatch(void* handle, const void* desc);
void     slbridge_destroy(void* handle);

} // extern "C"
