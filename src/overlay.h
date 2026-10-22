// In-game configuration overlay (Dear ImGui on the game's D3D12 queue and swapchain).
//
// slbridge.cpp owns the DLSS-G swapchain and its Present hook; it hands the overlay the device, the
// game's present queue, the swapchain and the window at creation, then calls overlay_render() right
// before every real Present. The overlay toggles on the key in Config::overlayKey (Home by default),
// edits g_cfg live, and shows an FPS counter with the displayed and base (pre-frame-gen) framerates.
#pragma once

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain3;

// Call once when the DLSS-G swapchain is created. hwnd is the swapchain's output window.
bool overlay_init(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapchain, void* hwnd);

// Tell the overlay how the swapchain is interpreted so the UI is encoded correctly.
// 0 = SDR (sRGB), 1 = HDR10 PQ (R10G10B10A2 + ST.2084), 2 = scRGB linear (FP16).
void overlay_set_colorspace(int mode);

// Draw the overlay/FPS counter into the current back buffer. Call on the presenting thread right
// before the real Present (before the PCL present markers). Cheap no-op when nothing is visible.
void overlay_render(IDXGISwapChain3* swapchain);

// Call before forwarding IDXGISwapChain::ResizeBuffers so no back-buffer references are outstanding.
void overlay_before_resize();

// Release everything (swapchain destroyed).
void overlay_shutdown();
