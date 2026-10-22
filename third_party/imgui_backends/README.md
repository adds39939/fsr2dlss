# Dear ImGui DX12 backend (patched)

Copy of `backends/imgui_impl_dx12.cpp/.h` from Dear ImGui v1.93.0 (MIT) with one addition:
`ImGui_ImplDX12_SetOutputTransform()`, a pixel-shader output transform so the overlay renders
correctly on HDR10 (PQ) and scRGB swapchains. Search for `[fsr2dlss]` to see the changes.

The ImGui core and the Win32 backend are fetched at build time at the same tag (see
`src/fetch_deps.sh`). Keep this copy in step with that tag when bumping ImGui.
