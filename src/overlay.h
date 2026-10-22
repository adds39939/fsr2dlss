// In-game configuration overlay (Dear ImGui on the game's D3D12 queue and swapchain).
//
// The overlay exists at all times. Two hooks feed it: slbridge.cpp's in-place Present hook on the
// DLSS-G swapchain while frame generation is live, and dxgihook.cpp's native IDXGISwapChain::Present
// hook otherwise. Either calls overlay_render() right before the real Present with the swapchain and
// the D3D12 direct queue to submit on; the overlay (re)binds itself to whatever swapchain shows up.
// It toggles on Config::overlayKey (Home by default), edits g_cfg live, shows the mod's status
// (FSR calls from the game, Streamline state, which swapchain is hooked) and an FPS counter with the
// displayed and base (pre-frame-gen) framerates.
#pragma once

struct IDXGISwapChain;
struct ID3D12CommandQueue;

// Draw the overlay/FPS counter into the swapchain's current back buffer. Call on the presenting
// thread right before the real Present. queue may be null (nothing is drawn until one is known).
// slPath says whether this is the DLSS-G (Streamline) swapchain, whose colour space the bridge
// reports via overlay_set_colorspace(); on the native path the colour space is derived.
void overlay_render(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue, bool slPath);

// Tell the overlay how the DLSS-G swapchain is interpreted so the UI is encoded correctly.
// 0 = SDR (sRGB), 1 = HDR10 PQ (R10G10B10A2 + ST.2084), 2 = scRGB linear (FP16).
void overlay_set_colorspace(int mode);

// Call before forwarding IDXGISwapChain::ResizeBuffers so no back-buffer work is outstanding.
void overlay_before_resize();

// A swapchain the overlay may be bound to is going away; it rebinds on the next overlay_render().
void overlay_swapchain_destroyed(void* swapchain);

// Release everything.
void overlay_shutdown();
