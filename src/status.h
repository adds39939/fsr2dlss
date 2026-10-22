// Live "is it working?" state of the mod, shown by the overlay's status panel. Written by proxy.cpp
// (what the game is calling), slbridge.cpp (Streamline) and dxgihook.cpp (native swapchain hook);
// plain fields, independent of each other, so no locking.
#pragma once
#include <cstdint>

enum SlState : int { SL_UNTRIED = 0, SL_TRYING = 1, SL_READY = 2, SL_FAILED = 3 };
enum PresentPath : int { PRESENT_NONE = 0, PRESENT_NATIVE = 1, PRESENT_STREAMLINE = 2 };

struct ModStatus {
    // proxy.cpp
    bool     realDllLoaded     = false;   // amd_fidelityfx_dx12.amd.dll found and its exports resolved
    uint32_t ffxCalls          = 0;       // any FidelityFX call from the game
    bool     upscaleContext    = false;   // the game created an FSR upscale context
    bool     fgContext         = false;   // the game created an FSR frame-generation context
    bool     fgSwapchainContext= false;   // the game asked for the FSR frame-generation swapchain
    uint32_t upscaleDispatches = 0;       // FSR upscale passes seen (one per rendered frame while FSR is on)
    uint32_t fgDispatches      = 0;       // FSR frame-generation dispatches seen
    uint64_t lastUpscaleTick   = 0;       // GetTickCount64 of the last upscale dispatch
    uint32_t steamUnhooks      = 0;       // times Steam's ffxConfigure hook was removed
    // slbridge.cpp
    int      slState           = SL_UNTRIED;
    bool     slSwapchainLive   = false;   // the DLSS-G swapchain exists and its Present is hooked
    bool     gameFgEnabled     = false;   // the game's frame-generation toggle (FRAMEGEN configure)
    // dxgihook.cpp
    bool     dxgiHooked        = false;   // native IDXGISwapChain::Present hooked
    bool     dxgiHookFailed    = false;
    uint32_t nativePresents    = 0;       // presents seen through the native hook
    bool     directQueueKnown  = false;   // a D3D12 direct queue was captured for the overlay
    // overlay.cpp
    int      presentPath       = PRESENT_NONE;   // where the overlay is currently drawn
};

extern ModStatus g_status;
