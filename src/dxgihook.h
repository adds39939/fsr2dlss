// Native DXGI/D3D12 hooks so the overlay exists even when the game is not using frame generation
// (i.e. when the mod does not own the swapchain). Installed from a worker thread started in DllMain.
#pragma once

struct ID3D12CommandQueue;

// Start the install thread (idempotent). Hooks IDXGISwapChain::Present/Present1/ResizeBuffers/
// ResizeBuffers1 and ID3D12CommandQueue::ExecuteCommandLists through their shared runtime vtables.
void dxgihook_start();

// The most recent D3D12 DIRECT command queue seen executing command lists (the game's render queue),
// or null if none yet. Used by the overlay to submit its draw on the native present path.
ID3D12CommandQueue* dxgihook_direct_queue();
