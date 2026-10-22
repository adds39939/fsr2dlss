// Native DXGI/D3D12 hooks so the overlay exists even when the game is not using frame generation
// (i.e. when the mod does not own the swapchain). Started from DllMain; cheap until a device exists.
#pragma once

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain;

// Hook D3D12CreateDevice and start the worker that installs the swapchain/queue hooks once the game's
// device exists (or after a timeout, with a throwaway device). Idempotent.
void dxgihook_start();

// A device the game is known to use (from the FidelityFX backend descriptor). Lets the hooks install
// right away using the game's own D3D12 runtime module if D3D12CreateDevice was not observed.
void dxgihook_notify_device(ID3D12Device* device);

// The D3D12 DIRECT command queue the calling thread most recently executed command lists on (the
// game's render queue when called from its Present), or null. Reference held by the hook.
ID3D12CommandQueue* dxgihook_direct_queue();

// Colour space the application last set on this swapchain with SetColorSpace1, or -1 if never set.
int dxgihook_colorspace(IDXGISwapChain* swapchain);
