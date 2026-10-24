// ============================================================================
// In-game overlay: Dear ImGui rendered straight into the current swapchain's
// back buffer from a Present hook, on the game's own direct command queue.
//
//  - Always on: fed by slbridge.cpp while the DLSS-G swapchain is live and by
//    dxgihook.cpp's native Present hook otherwise. Binds lazily to whatever
//    swapchain/queue arrives and rebinds when they change.
//  - Toggle: Config::overlayKey (Home), polled with GetAsyncKeyState so it works
//    no matter how the game reads input (raw input, DirectInput, messages).
//  - Input: the swapchain window's WndProc is subclassed. While the menu is open
//    keyboard/mouse messages go to ImGui and are swallowed so the game does not
//    react; SetCursorPos/ClipCursor are hooked (MinHook) so the game cannot
//    re-centre or hide the pointer. ImGui draws its own cursor.
//  - HDR: the bridge tells us the DLSS-G swapchain's colour space; on the native
//    path it is derived from the back-buffer format and the output's colour
//    space. The patched DX12 backend applies the matching output transform.
//  - GPU: one command list, 3 rotating allocators fenced on the queue. Back-buffer
//    references are released every frame so ResizeBuffers is never blocked by us.
//  - Layers: the UI is drawn into its own RGBA texture (premultiplied) and then
//    composited onto the back buffer with a fullscreen blend. On the DLSS-G path a
//    copy of the back buffer taken before that composite (hud-less) and the UI
//    texture are handed to the bridge, which tags them so DLSS-G recomposites the
//    overlay on generated frames instead of interpolating (warping) it.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "MinHook.h"

#include "config.h"
#include "slbridge.h"
#include "status.h"
#include "dxgihook.h"
#include "overlay.h"

extern "C" void bridge_logs(const char* s);
extern "C" bool mh_ensure_init();
static void OL(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n] = '\n'; buf[n + 1] = 0;
    bridge_logs(buf);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- state ----------------------------------------------------------------------------------------
static const UINT kFramesInFlight = 3;
static const UINT kSrvHeapSize    = 64;

struct FrameRes { ID3D12CommandAllocator* alloc = nullptr; UINT64 fenceValue = 0; };

static ID3D12Device*              g_dev        = nullptr;   // AddRef'd
static ID3D12CommandQueue*        g_queue      = nullptr;   // AddRef'd
static IDXGISwapChain*            g_sc         = nullptr;   // not AddRef'd; the hook owner keeps it alive
static bool                       g_slPath     = false;
static HWND                       g_hwnd       = nullptr;
static ID3D12GraphicsCommandList* g_cmd        = nullptr;
static FrameRes                   g_frames[kFramesInFlight];
static UINT                       g_frameIdx   = 0;
static ID3D12Fence*               g_fence      = nullptr;
static HANDLE                     g_fenceEvent = nullptr;
static UINT64                     g_fenceCounter = 0;
static ID3D12DescriptorHeap*      g_rtvHeap    = nullptr;
static ID3D12DescriptorHeap*      g_srvHeap    = nullptr;
static UINT                       g_srvInc     = 0;
static bool                       g_srvUsed[kSrvHeapSize] = {};
static DXGI_FORMAT                g_rtvFormat  = DXGI_FORMAT_UNKNOWN;   // ImGui renders into the UI layer: its format
static UINT                       g_bbW = 0, g_bbH = 0;
// UI layer + hud-less copy + composite pass
static ID3D12Resource*            g_uiTex      = nullptr;
static D3D12_RESOURCE_STATES      g_uiState    = D3D12_RESOURCE_STATE_COMMON;
static DXGI_FORMAT                g_uiFmt      = DXGI_FORMAT_UNKNOWN;
static ID3D12Resource*            g_hudTex     = nullptr;   // COPY_DEST between frames
static DXGI_FORMAT                g_hudFmt     = DXGI_FORMAT_UNKNOWN;
static UINT                       g_layerW = 0, g_layerH = 0;
static D3D12_CPU_DESCRIPTOR_HANDLE g_uiSrvCpu{}; static D3D12_GPU_DESCRIPTOR_HANDLE g_uiSrvGpu{}; static bool g_uiSrvValid = false;
static ID3D12RootSignature*       g_compRs     = nullptr;
static ID3D12PipelineState*       g_compPso    = nullptr;
static DXGI_FORMAT                g_compFmt    = DXGI_FORMAT_UNKNOWN;
static bool                       g_layersValid = false;    // layers produced by the last overlay_render
static bool                       g_gpuReady   = false;
static bool                       g_imguiReady = false;
static bool                       g_dx12Ready  = false;
static bool                       g_failed     = false;    // give up after a hard failure (no spam)
static int                        g_colorMode  = 0;        // 0 sdr, 1 pq, 2 scrgb
static bool                       g_colorExplicit = false; // set by the bridge for the DLSS-G swapchain
static float                      g_appliedScale = 0.0f;

static volatile LONG              g_visible    = 0;
static bool                       g_keyWasDown = false;
static bool                       g_saveOk = false, g_saveTried = false; static ULONGLONG g_saveMsgUntil = 0;

static CRITICAL_SECTION           g_cs;                    // guards ImGui context (WndProc vs present thread)
static bool                       g_csInit     = false;
static WNDPROC                    g_origWndProc = nullptr;
static bool                       g_wndUnicode = true;
static float overlayNits();   // paper white for the current colour mode (defined with deriveColorMode)

// ---- user32 hooks: keep the game from fighting the overlay cursor ---------------------------------
typedef BOOL (WINAPI* PFN_SetCursorPos)(int, int);
typedef BOOL (WINAPI* PFN_ClipCursor)(const RECT*);
static PFN_SetCursorPos o_SetCursorPos = nullptr;
static PFN_ClipCursor   o_ClipCursor   = nullptr;
static RECT g_lastClip{}; static bool g_lastClipValid = false;   // the game's clip, re-applied when the menu closes
static BOOL WINAPI hk_SetCursorPos(int x, int y) { if (g_visible) return TRUE; return o_SetCursorPos ? o_SetCursorPos(x, y) : FALSE; }
static BOOL WINAPI hk_ClipCursor(const RECT* r)
{
    if (r) { g_lastClip = *r; g_lastClipValid = true; } else g_lastClipValid = false;
    if (g_visible) return TRUE;
    return o_ClipCursor ? o_ClipCursor(r) : FALSE;
}
static void installCursorHooks()
{
    static bool done = false; if (done) return; done = true;
    if (!mh_ensure_init()) return;
    if (MH_CreateHookApi(L"user32", "SetCursorPos", (void*)&hk_SetCursorPos, (void**)&o_SetCursorPos) == MH_OK) MH_EnableHook((void*)GetProcAddress(GetModuleHandleW(L"user32"), "SetCursorPos"));
    if (MH_CreateHookApi(L"user32", "ClipCursor",   (void*)&hk_ClipCursor,   (void**)&o_ClipCursor)   == MH_OK) MH_EnableHook((void*)GetProcAddress(GetModuleHandleW(L"user32"), "ClipCursor"));
    OL("[OVL] cursor hooks installed (SetCursorPos=%p ClipCursor=%p)", (void*)o_SetCursorPos, (void*)o_ClipCursor);
}

// ---- window procedure --------------------------------------------------------------------------------
static bool isMouseMsg(UINT m) { return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_MOUSELEAVE || m == WM_NCMOUSEMOVE; }
static bool isKeyMsg(UINT m)   { return m >= WM_KEYFIRST && m <= WM_KEYLAST; }   // WM_KEYDOWN..WM_UNICHAR, incl. SYSKEY/CHAR

static LRESULT CALLBACK OverlayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_visible && g_imguiReady) {
        // ImGui works in window client space (the Win32 backend reads the cursor in client coordinates);
        // overlay_render() maps that onto the back buffer through io.DisplayFramebufferScale.
        if (g_csInit) EnterCriticalSection(&g_cs);
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
        if (g_csInit) LeaveCriticalSection(&g_cs);

        // Swallow input while the menu is open. Key/button releases always pass through so the
        // game never sees a key stuck down from before the menu opened.
        bool release = (msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP ||
                        msg == WM_MBUTTONUP || msg == WM_XBUTTONUP);
        if (!release && (isKeyMsg(msg) || isMouseMsg(msg) || msg == WM_INPUT)) return 0;
        if (msg == WM_SETCURSOR) return TRUE;
    }
    return g_wndUnicode ? CallWindowProcW(g_origWndProc, h, msg, wp, lp) : CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

static void uninstallWndProc()
{
    if (!g_hwnd || !g_origWndProc) return;
    if (g_wndUnicode) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc); else SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
    g_origWndProc = nullptr;
}
static void installWndProc(HWND hwnd)
{
    if (hwnd == g_hwnd && g_origWndProc) return;
    uninstallWndProc();
    g_hwnd = hwnd;
    if (!g_hwnd) return;
    g_wndUnicode = IsWindowUnicode(g_hwnd) != FALSE;
    LONG_PTR prev = g_wndUnicode ? SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)&OverlayWndProc)
                                 : SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)&OverlayWndProc);
    g_origWndProc = (WNDPROC)prev;
    OL("[OVL] WndProc subclassed hwnd=%p orig=%p unicode=%d", (void*)g_hwnd, (void*)prev, (int)g_wndUnicode);
}

// ---- ImGui texture descriptors ------------------------------------------------------------------------
static void srvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    for (UINT i = 0; i < kSrvHeapSize; ++i) if (!g_srvUsed[i]) {
        g_srvUsed[i] = true;
        cpu->ptr = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)i * g_srvInc;
        gpu->ptr = g_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)i * g_srvInc;
        return;
    }
    cpu->ptr = 0; gpu->ptr = 0;
}
static void srvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    SIZE_T base = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr < base) return;
    SIZE_T i = (cpu.ptr - base) / g_srvInc;
    if (i < kSrvHeapSize) g_srvUsed[i] = false;
}

// ---- GPU objects ------------------------------------------------------------------------------------------
template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void waitGpuIdle()
{
    if (!g_fence || !g_queue) return;
    UINT64 v = ++g_fenceCounter;
    if (FAILED(g_queue->Signal(g_fence, v))) return;
    if (g_fence->GetCompletedValue() < v) { g_fence->SetEventOnCompletion(v, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 2000); }
}

static bool createGpuObjects()
{
    for (UINT i = 0; i < kFramesInFlight; ++i)
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].alloc)))) return false;
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, nullptr, IID_PPV_ARGS(&g_cmd)))) return false;
    g_cmd->Close();
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC rd{}; rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rd.NumDescriptors = 2;   // [0] back buffer, [1] UI layer
    if (FAILED(g_dev->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&g_rtvHeap)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC sd{}; sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sd.NumDescriptors = kSrvHeapSize; sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_srvHeap)))) return false;
    g_srvInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_gpuReady = true;
    return true;
}

static void shutdownDx12Backend()
{
    if (!g_dx12Ready) return;
    waitGpuIdle();
    ImGui_ImplDX12_Shutdown();
    memset(g_srvUsed, 0, sizeof(g_srvUsed));
    g_dx12Ready = false;
}

static void releaseLayers()
{
    waitGpuIdle();
    if (g_uiSrvValid) { srvFree(nullptr, g_uiSrvCpu, g_uiSrvGpu); g_uiSrvValid = false; }
    rel(g_uiTex); rel(g_hudTex); g_uiFmt = g_hudFmt = DXGI_FORMAT_UNKNOWN; g_layerW = g_layerH = 0; g_layersValid = false;
}
static void releaseComposite() { waitGpuIdle(); rel(g_compPso); rel(g_compRs); g_compFmt = DXGI_FORMAT_UNKNOWN; }

static void releaseGpuObjects()
{
    releaseLayers();
    releaseComposite();
    shutdownDx12Backend();
    waitGpuIdle();
    for (UINT i = 0; i < kFramesInFlight; ++i) { rel(g_frames[i].alloc); g_frames[i].fenceValue = 0; }
    rel(g_cmd); rel(g_fence); rel(g_rtvHeap); rel(g_srvHeap);
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    g_gpuReady = false; g_frameIdx = 0;
}

// Fullscreen alpha-blend of the (premultiplied) UI layer onto the back buffer.
static bool createComposite(DXGI_FORMAT rtFmt)
{
    if (g_compPso && g_compFmt == rtFmt) return true;
    releaseComposite();
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER param{}; param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1; param.DescriptorTable.pDescriptorRanges = &range; param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp{}; samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{}; rs.NumParameters = 1; rs.pParameters = &param; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) || !sig) { OL("[OVL] composite root signature failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
    HRESULT hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_compRs));
    sig->Release();
    if (FAILED(hr)) { OL("[OVL] CreateRootSignature failed 0x%08lx", (unsigned long)hr); return false; }

    static const char* src =
        "Texture2D t : register(t0); SamplerState s : register(s0);\n"
        "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(V i) : SV_Target { return t.Sample(s, i.uv); }\n";
    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err))) { OL("[OVL] composite VS: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err))) { OL("[OVL] composite PS: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); vs->Release(); return false; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_compRs;
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;              // UI layer is premultiplied
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1; pso.RTVFormats[0] = rtFmt; pso.SampleDesc.Count = 1; pso.NodeMask = 1;
    hr = g_dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_compPso));
    vs->Release(); ps->Release();
    if (FAILED(hr)) { OL("[OVL] composite PSO failed 0x%08lx (fmt=%u)", (unsigned long)hr, (unsigned)rtFmt); return false; }
    g_compFmt = rtFmt;
    return true;
}

// UI layer (always) and hud-less copy (DLSS-G path) sized to the back buffer.
static bool ensureLayerTextures(UINT w, UINT h, DXGI_FORMAT uiFmt, DXGI_FORMAT bbFmt, bool wantHudless)
{
    if (g_uiTex && (g_layerW != w || g_layerH != h || g_uiFmt != uiFmt)) releaseLayers();
    if (g_hudTex && g_hudFmt != bbFmt) { waitGpuIdle(); rel(g_hudTex); g_hudFmt = DXGI_FORMAT_UNKNOWN; }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (!g_uiTex) {
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = uiFmt; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{}; cv.Format = uiFmt;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&g_uiTex)))) { OL("[OVL] UI layer texture failed"); return false; }
        g_uiState = D3D12_RESOURCE_STATE_RENDER_TARGET; g_uiFmt = uiFmt; g_layerW = w; g_layerH = h;
        if (!g_uiSrvValid) { srvAlloc(nullptr, &g_uiSrvCpu, &g_uiSrvGpu); g_uiSrvValid = g_uiSrvCpu.ptr != 0; }
        if (!g_uiSrvValid) { OL("[OVL] no SRV slot for the UI layer"); return false; }
        g_dev->CreateShaderResourceView(g_uiTex, nullptr, g_uiSrvCpu);
        OL("[OVL] UI layer %ux%u fmt=%u", w, h, (unsigned)uiFmt);
    }
    if (wantHudless && !g_hudTex) {
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = bbFmt; rd.SampleDesc.Count = 1;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_hudTex)))) { OL("[OVL] hud-less copy texture failed"); return false; }
        g_hudFmt = bbFmt;
        OL("[OVL] hud-less layer %ux%u fmt=%u", w, h, (unsigned)bbFmt);
    }
    return true;
}
static DXGI_FORMAT uiFormatFor(int colorMode) { return colorMode == 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM; }

static void applyStyle()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 6.0f; st.FrameRounding = 4.0f; st.GrabRounding = 4.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.94f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.45f, 0.20f, 1.0f);
    st.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.45f, 0.20f, 0.55f);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.58f, 0.26f, 0.8f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.58f, 0.26f, 1.0f);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.95f, 0.55f, 1.0f);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.75f, 0.40f, 1.0f);
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.95f, 0.55f, 1.0f);
    st.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.45f, 0.20f, 0.6f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.58f, 0.26f, 0.9f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.70f, 0.30f, 1.0f);
    float s = g_cfg.overlayScale;
    st.ScaleAllSizes(s);
    st.FontScaleMain = s;
    g_appliedScale = s;
}

static bool initImGui(DXGI_FORMAT fmt)
{
    if (!g_imguiReady) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
        if (!ImGui_ImplWin32_Init(g_hwnd)) { OL("[OVL] ImGui_ImplWin32_Init failed"); return false; }
        applyStyle();
        g_imguiReady = true;
    }
    if (!g_dx12Ready) {
        ImGui_ImplDX12_InitInfo ii{};
        ii.Device = g_dev; ii.CommandQueue = g_queue; ii.NumFramesInFlight = (int)kFramesInFlight;
        ii.RTVFormat = fmt; ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
        ii.SrvDescriptorHeap = g_srvHeap; ii.SrvDescriptorAllocFn = srvAlloc; ii.SrvDescriptorFreeFn = srvFree;
        if (!ImGui_ImplDX12_Init(&ii)) { OL("[OVL] ImGui_ImplDX12_Init failed"); return false; }
        // Compile shaders / build the PSO now: NewFrame() would otherwise do it lazily and only report
        // failure through an assert that is compiled out, leaving a dead command list every frame.
        if (!ImGui_ImplDX12_CreateDeviceObjects()) {
            OL("[OVL] ImGui_ImplDX12_CreateDeviceObjects failed (fmt=%u): %s", (unsigned)fmt, ImGui_ImplDX12_GetLastError());
            ImGui_ImplDX12_Shutdown();
            return false;
        }
        g_rtvFormat = fmt; g_dx12Ready = true;
        OL("[OVL] ImGui DX12 backend ready (rtv fmt=%u, color mode=%d, path=%s)", (unsigned)fmt, g_colorMode, g_slPath ? "streamline" : "native");
    }
    ImGui_ImplDX12_SetOutputTransform(g_colorMode, overlayNits());
    return true;
}

// Derive the colour space of a swapchain we were not told about (native path). The swapchain cannot
// be asked, so use what the game last passed to SetColorSpace1 (recorded by dxgihook.cpp); without
// that, a 10-bit back buffer is SDR (the DXGI default colour space) and FP16 is scRGB. On an SDR
// output DWM maps scRGB 1.0 to SDR white, so paper white is 80 nits there instead of the configured
// HDR value.
static float g_nitsOverride = 0.0f;   // 0 = Config::overlayNits
static float overlayNits() { return g_nitsOverride > 0 ? g_nitsOverride : g_cfg.overlayNits; }
static bool outputIsHdr(IDXGISwapChain* sc)
{
    IDXGIOutput* out = nullptr; bool hdr = false;
    if (SUCCEEDED(sc->GetContainingOutput(&out)) && out) {
        IDXGIOutput6* o6 = nullptr;
        if (SUCCEEDED(out->QueryInterface(IID_PPV_ARGS(&o6))) && o6) {
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(o6->GetDesc1(&d)) && d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) hdr = true;
            o6->Release();
        }
        out->Release();
    }
    return hdr;
}
static int deriveColorMode(IDXGISwapChain* sc, DXGI_FORMAT fmt)
{
    g_nitsOverride = 0.0f;
    int cs = dxgihook_colorspace(sc);
    if (cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return 1;
    if (cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)    { if (!outputIsHdr(sc)) g_nitsOverride = 80.0f; return 2; }
    if (cs >= 0) return 0;   // explicitly SDR
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) { if (!outputIsHdr(sc)) g_nitsOverride = 80.0f; return 2; }
    return 0;
}

// Bind to the swapchain/queue a Present hook handed us; rebuild whatever changed.
static bool bind(IDXGISwapChain* sc, ID3D12CommandQueue* queue, bool slPath, const DXGI_SWAP_CHAIN_DESC1& d)
{
    ID3D12Device* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false;
    if (dev != g_dev) {
        releaseGpuObjects();
        rel(g_dev); g_dev = dev;              // keep the reference GetDevice gave us
        if (!createGpuObjects()) { OL("[OVL] failed to create GPU objects"); g_failed = true; return false; }
    } else dev->Release();
    if (queue != g_queue) {
        shutdownDx12Backend();                // the backend uploads textures on this queue
        rel(g_queue); g_queue = queue; g_queue->AddRef();
    }
    HWND hwnd = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) { sc1->GetHwnd(&hwnd); sc1->Release(); }
    if (!hwnd) { DXGI_SWAP_CHAIN_DESC d0{}; if (SUCCEEDED(sc->GetDesc(&d0))) hwnd = d0.OutputWindow; }
    if (hwnd != g_hwnd) {
        // Renderer backend first (it needs the ImGui context), then the platform backend and context.
        if (g_imguiReady) {
            shutdownDx12Backend();
            if (g_csInit) EnterCriticalSection(&g_cs);
            ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); g_imguiReady = false;
            if (g_csInit) LeaveCriticalSection(&g_cs);
        }
        installWndProc(hwnd);
    }
    installCursorHooks();
    g_sc = sc; g_slPath = slPath;
    if (!g_colorExplicit || !slPath) g_colorMode = deriveColorMode(sc, d.Format);
    g_status.presentPath = slPath ? PRESENT_STREAMLINE : PRESENT_NATIVE;
    OL("[OVL] bound to swapchain %p (%s path) %ux%u fmt=%u queue=%p hwnd=%p color=%d", (void*)sc, slPath ? "streamline" : "native", d.Width, d.Height, (unsigned)d.Format, (void*)queue, (void*)hwnd, g_colorMode);
    return true;
}

// ---- public API -------------------------------------------------------------------------------------------
void overlay_set_colorspace(int mode)
{
    g_colorMode = mode; g_colorExplicit = true; g_nitsOverride = 0.0f;
    if (g_dx12Ready) ImGui_ImplDX12_SetOutputTransform(g_colorMode, overlayNits());
}

void overlay_before_resize()
{
    if (!g_gpuReady) return;
    waitGpuIdle();   // our last command list referenced a back buffer; make sure it is done
    g_bbW = g_bbH = 0;
}

void overlay_swapchain_destroyed(void* sc)
{
    if (sc && sc == (void*)g_sc) {
        waitGpuIdle();
        g_sc = nullptr; g_colorExplicit = false;
        g_status.presentPath = PRESENT_NONE;
    }
}

void overlay_shutdown()
{
    InterlockedExchange(&g_visible, 0);
    // Unsubclass first so the window thread stops entering ImGui, then destroy the context under the lock.
    uninstallWndProc();
    if (g_csInit) EnterCriticalSection(&g_cs);
    releaseGpuObjects();
    if (g_imguiReady) { ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); g_imguiReady = false; }
    if (g_csInit) LeaveCriticalSection(&g_cs);
    rel(g_queue); rel(g_dev); g_sc = nullptr; g_hwnd = nullptr;
    g_rtvFormat = DXGI_FORMAT_UNKNOWN;
    g_status.presentPath = PRESENT_NONE;
}

// ---- toggle -------------------------------------------------------------------------------------------------
static void pollToggleKey()
{
    if (!g_cfg.overlayOn) { if (g_visible) InterlockedExchange(&g_visible, 0); return; }
    bool down = (GetAsyncKeyState(g_cfg.overlayKey) & 0x8000) != 0;
    bool fg = GetForegroundWindow() == g_hwnd;
    if (down && !g_keyWasDown && fg) {
        LONG now = g_visible ? 0 : 1;
        InterlockedExchange(&g_visible, now);
        OL("[OVL] overlay %s", now ? "opened" : "closed");
        // Free the pointer from any clip the game applied before the menu opened, and put the game's
        // clip back when it closes.
        if (now) { if (o_ClipCursor) o_ClipCursor(nullptr); else ClipCursor(nullptr); }
        else if (g_lastClipValid) { if (o_ClipCursor) o_ClipCursor(&g_lastClip); else ClipCursor(&g_lastClip); }
        if (!now && g_imguiReady) { ImGuiIO& io = ImGui::GetIO(); io.ClearInputKeys(); io.ClearInputMouse(); }
    }
    g_keyWasDown = down;
}

// ---- rates for the status panel / native-path FPS ------------------------------------------------------------
static float g_upscaleRate = 0;   // FSR upscale passes per second the game issues
static void updateRates(bool slPath)
{
    static ULONGLONG s_last = 0; static uint32_t s_lastUp = 0; static unsigned s_presents = 0, s_lastPresents = 0;
    ++s_presents;
    ULONGLONG now = GetTickCount64();
    if (!s_last) { s_last = now; s_lastUp = g_status.upscaleDispatches; s_lastPresents = s_presents; return; }
    ULONGLONG dt = now - s_last;
    if (dt < 500) return;
    g_upscaleRate = (g_status.upscaleDispatches - s_lastUp) * 1000.0f / (float)dt;
    if (!slPath) {
        // No DLSS-G: the bridge is not counting, so the presents we see are the whole story.
        float fps = (s_presents - s_lastPresents) * 1000.0f / (float)dt;
        g_stats.baseFps = fps; g_stats.displayFps = fps; g_stats.fgApplied = 0;
    }
    s_last = now; s_lastUp = g_status.upscaleDispatches; s_lastPresents = s_presents;
}

// ---- UI -----------------------------------------------------------------------------------------------------
static void changed() { g_cfg.generation++; }

static bool comboInt(const char* label, int* v, const char* const* names, int count, const char* tooltip = nullptr)
{
    bool ch = false;
    int cur = (*v >= 0 && *v < count) ? *v : 0;
    if (ImGui::BeginCombo(label, names[cur])) {
        for (int i = 0; i < count; ++i) {
            bool sel = (i == cur);
            if (ImGui::Selectable(names[i], sel)) { if (i != *v) { *v = i; ch = true; } }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return ch;
}

static const char* srModeName(int m)
{
    switch (m) { case 1: return "Performance"; case 2: return "Balanced"; case 3: return "Quality"; case 4: return "Ultra Performance"; case 5: return "Ultra Quality"; case 6: return "DLAA"; default: return "off"; }
}

static const ImVec4 kOk(0.5f, 1.0f, 0.6f, 1), kWarn(1.0f, 0.75f, 0.3f, 1), kBad(1.0f, 0.45f, 0.4f, 1), kDim(0.6f, 0.6f, 0.6f, 1);
static void row(const char* label, const ImVec4& col, const char* fmt, ...)
{
    char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    ImGui::TextDisabled("%s", label); ImGui::SameLine(170 * g_cfg.overlayScale); ImGui::TextColored(col, "%s", buf);
}

static void drawStatus()
{
    const ModStatus& st = g_status;
    const BridgeStats& s = g_stats;
    bool fsrLive = st.lastUpscaleTick && (GetTickCount64() - st.lastUpscaleTick) < 1000;

    if (st.realDllLoaded) row("Proxy", kOk, "loaded, original amd_fidelityfx_dx12.amd.dll found");
    else                  row("Proxy", kBad, "original DLL missing: rename the game's amd_fidelityfx_dx12.dll to amd_fidelityfx_dx12.amd.dll");

    if (!st.ffxCalls)           row("Game FSR", kWarn, "no FidelityFX calls yet: set Upscaling to FSR in the game's graphics menu");
    else if (fsrLive)           row("Game FSR", kOk,   "upscaling active, %.0f passes/s -> %s", g_upscaleRate, s.srActive ? "DLSS Super Resolution" : (g_cfg.dlssSR ? "FSR (DLSS not running yet)" : "FSR (DLSS disabled below)"));
    else if (st.upscaleContext) row("Game FSR", kWarn, "FSR context exists but no upscale pass this second (menu, loading, or FSR switched off)");
    else                        row("Game FSR", kWarn, "%u FidelityFX calls, no upscale context yet", st.ffxCalls);

    if (!st.fgContext && !st.fgSwapchainContext) row("Game frame gen", kWarn, "off (the game has not created its frame-generation context)");
    else row("Game frame gen", st.gameFgEnabled ? kOk : kWarn, "%s, FSR frame-gen swapchain %s", st.gameFgEnabled ? "on" : "off (toggle)",
             st.slSwapchainLive ? "replaced by DLSS-G" : (st.fgSwapchainContext ? "running on AMD FSR (DLSS-G substitution did not happen)" : "not created"));

    switch (st.slState) {
        case SL_READY:  row("Streamline", kOk,   "ready, %u.%u.%u, %s tagging", s.slVersion[0], s.slVersion[1], s.slVersion[2], s.frameTagging ? "frame-based" : "legacy"); break;
        case SL_FAILED: row("Streamline", kBad,  "FAILED to initialise, see streamline\\sl.log and ffx_bridge.log"); break;
        case SL_TRYING: row("Streamline", kWarn, "initialising"); break;
        default:        row("Streamline", kWarn, "not started (starts with the game's first FSR context)"); break;
    }

    if (st.presentPath == PRESENT_STREAMLINE)      row("Swapchain", kOk,   "DLSS-G swapchain hooked%s", s.fgApplied ? " (Streamline presents real + generated frames)" : " (frame generation currently off)");
    else if (st.presentPath == PRESENT_NATIVE)      row("Swapchain", st.slSwapchainLive ? kWarn : kOk, "game's own swapchain hooked (no frame generation on this path)");
    else                                            row("Swapchain", kWarn, "nothing hooked yet");
    if (st.dxgiHookFailed) row("DXGI hook", kBad, "native Present hook failed (overlay only available with frame generation on)");

    if (s.fgStatus) row("DLSS-G status", kWarn, "0x%x:%s%s%s%s%s", s.fgStatus,
        (s.fgStatus & 1) ? " resolution-too-low" : "", (s.fgStatus & 2) ? " reflex-missing" : "",
        (s.fgStatus & 4) ? " hdr-format" : "", (s.fgStatus & 8) ? " constants-invalid" : "", (s.fgStatus & 16) ? " backbuffer-index" : "");
    if (g_cfg.steamFix) row("Steam overlay", kDim, "FSR detector removed %u time(s)", st.steamUnhooks);
    if (g_cfg.fgPreset > 0) row("FG preset", kDim, "requesting %s via the driver profile (confirm in streamline\\sl.log: \"render preset\")", config_fg_preset_name(g_cfg.fgPreset));
    if (s.fgApplied && g_cfg.uiLayer) row("UI layer", g_layersValid ? kOk : kWarn, g_layersValid ? "hud-less + UI tagged, overlay composited after frame generation" : "not produced this frame");
}

static void drawMenu()
{
    const BridgeStats& s = g_stats;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620 * g_cfg.overlayScale, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("fsr2dlss###fsr2dlss", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }

    ImGui::TextDisabled("Press %s to close. Changes apply immediately; Save writes fsr2dlss.ini.", config_key_name(g_cfg.overlayKey));
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawStatus();
        ImGui::Spacing();
        ImGui::Text("Framerate: %.0f fps displayed, %.0f fps rendered", s.displayFps, s.baseFps);
        if (s.fgApplied >= 2)      ImGui::Text("Frame generation: %ux (max %ux on this card)", s.fgApplied, s.fgMaxGen + 1);
        else if (s.fgApplied == 1) ImGui::Text("Frame generation: dynamic (max %ux on this card)", s.fgMaxGen + 1);
        else                       ImGui::Text("Frame generation: off");
        if (s.srMode >= 0) ImGui::Text("Super resolution: DLSS %s, preset %s, %ux%u -> %ux%u", srModeName(s.srMode), config_sr_preset_name(s.srPreset), s.renderW, s.renderH, s.outW, s.outH);
        else               ImGui::Text("Super resolution: %s", g_cfg.dlssSR ? "DLSS not running (waiting for the game's upscale pass)" : "FSR (DLSS disabled)");
    }

    // DLSS SR
    if (ImGui::CollapsingHeader("DLSS Super Resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Replace FSR upscaling with DLSS", &g_cfg.dlssSR)) changed();
        static const char* qn[SR_QUALITY_COUNT] = { "Auto (from the in-game FSR preset)", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };
        ImGui::BeginDisabled(!g_cfg.dlssSR);
        if (comboInt("Quality mode", &g_cfg.srQuality, qn, SR_QUALITY_COUNT,
                     "The game still renders at the resolution of its FSR preset.\nThis picks which DLSS mode (and therefore default model) is used for that input.")) changed();
        static const int   presetVals[]  = { 0, 11, 10, 12, 13, 5, 6 };
        static const char* presetNames[] = { "Default (NVIDIA's choice per mode)", "K  - transformer, best quality", "J  - transformer, less ghosting, more flicker",
                                             "L  - sharper and more stable, heaviest", "M  - like L, cheaper", "E  - legacy CNN (deprecated)", "F  - legacy CNN (deprecated)" };
        int pi = 0; for (int i = 0; i < 7; ++i) if (presetVals[i] == g_cfg.srPreset) pi = i;
        if (comboInt("Model preset", &pi, presetNames, 7, "DLSS 310.x presets. K is the default transformer model; L and M are the newest models.")) { g_cfg.srPreset = presetVals[pi]; changed(); }
        ImGui::EndDisabled();
    }

    // FG
    if (ImGui::CollapsingHeader("DLSS Frame Generation", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!g_status.gameFgEnabled) ImGui::TextColored(kWarn, "Turn Frame Generation on in the game's graphics menu for these to take effect.");
        static const char* fgNames[FG_MODE_COUNT] = { "Off (present real frames only)", "Fixed multiplier", "Dynamic Multi Frame Generation" };
        int mode = g_cfg.fgMode;
        if (comboInt("Preset", &mode, fgNames, FG_MODE_COUNT,
                     "Fixed: always generate (multiplier - 1) frames per rendered frame.\nDynamic: DLSS-G picks the multiplier each frame to hit the target framerate.")) {
            if (mode == FG_DYNAMIC && !s.fgDynamicOk) mode = FG_FIXED;
            g_cfg.fgMode = mode; changed();
        }
        if (!s.fgDynamicOk) ImGui::TextDisabled("Dynamic MFG is not reported as supported by this card/driver.");
        int maxMult = (int)s.fgMaxGen + 1; if (maxMult < 2) maxMult = 2; if (maxMult > 4) maxMult = 4;
        ImGui::BeginDisabled(g_cfg.fgMode != FG_FIXED);
        int mult = g_cfg.fgMultiplier;
        if (ImGui::SliderInt("Multiplier", &mult, 2, 4, "%dx")) { g_cfg.fgMultiplier = mult; changed(); }
        if (mult > maxMult) ImGui::TextColored(kWarn, "This card reports %dx as its maximum; %dx will be clamped.", maxMult, mult);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(g_cfg.fgMode != FG_DYNAMIC);
        float tfps = g_cfg.fgDynamicFps;
        if (ImGui::InputFloat("Dynamic target fps (0 = refresh rate)", &tfps, 1.0f, 10.0f, "%.0f")) { if (tfps < 0) tfps = 0; if (tfps > 999) tfps = 999; g_cfg.fgDynamicFps = tfps; changed(); }
        ImGui::EndDisabled();
        static const char* fgPresetNames[4] = { "Default (NVIDIA's choice)", "A", "B", "C" };
        int fgp = (g_cfg.fgPreset >= 0 && g_cfg.fgPreset <= 3) ? g_cfg.fgPreset : 0;
        if (comboInt("Model preset", &fgp, fgPresetNames, 4,
                     "DLSS Frame Generation model preset (the NVIDIA App's frame-gen preset). Applied by answering\nNGX's driver-profile query; frame generation restarts briefly to pick it up.")) {
            g_cfg.fgPreset = fgp; changed();
        }
        if (g_cfg.fgPreset > 3) ImGui::TextDisabled("Preset %s set in fsr2dlss.ini.", config_fg_preset_name(g_cfg.fgPreset));
        static const char* flipNames[3] = { "Software pacer (clean 2x)", "Hardware flip metering (true 3x/4x)", "Auto" };
        if (comboInt("Flip metering (needs restart)", &g_cfg.flipMode, flipNames, 3)) changed();
        if (ImGui::Checkbox("Keep this overlay out of frame generation (UI layer)", &g_cfg.uiLayer)) changed();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Tags a hud-less copy of the frame and the overlay as DLSS-G's UI layer,\nso generated frames get the overlay composited instead of warped.");
        ImGui::BeginDisabled(!g_cfg.uiLayer);
        if (ImGui::Checkbox("DLSS-G UI recomposition", &g_cfg.uiRecompose)) changed();
        ImGui::EndDisabled();
    }

    // Overlay
    if (ImGui::CollapsingHeader("Overlay", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Show FPS counter", &g_cfg.showFps)) changed();
        static const char* corners[4] = { "Top-left", "Top-right", "Bottom-left", "Bottom-right" };
        if (comboInt("FPS counter position", &g_cfg.fpsCorner, corners, 4)) changed();
        float sc = g_cfg.overlayScale;
        if (ImGui::SliderFloat("UI scale", &sc, 0.5f, 3.0f, "%.2f")) { g_cfg.overlayScale = sc; changed(); }
        if (g_colorMode != 0) {
            float nits = g_cfg.overlayNits;
            if (ImGui::SliderFloat("HDR brightness (nits)", &nits, 50.0f, 1000.0f, "%.0f")) { g_cfg.overlayNits = nits; ImGui_ImplDX12_SetOutputTransform(g_colorMode, overlayNits()); changed(); }
        }
        if (ImGui::Checkbox("Steam overlay reports DLSS", &g_cfg.steamFix)) changed();
        if (ImGui::Checkbox("Write ffx_bridge.log (Debug)", &g_cfg.debug)) changed();
        ImGui::TextDisabled("Toggle key: %s (change ToggleKey in fsr2dlss.ini)", config_key_name(g_cfg.overlayKey));
    }

    ImGui::Separator();
    if (ImGui::Button("Save to fsr2dlss.ini", ImVec2(200 * g_cfg.overlayScale, 0))) { g_saveOk = config_save(); g_saveTried = true; g_saveMsgUntil = GetTickCount64() + 3000; }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(120 * g_cfg.overlayScale, 0))) InterlockedExchange(&g_visible, 0);
    if (g_saveTried && GetTickCount64() < g_saveMsgUntil) { ImGui::SameLine(); if (g_saveOk) ImGui::TextColored(kOk, "Saved."); else ImGui::TextColored(kBad, "Save failed (see ffx_bridge.log)."); }
    ImGui::TextDisabled("Flip metering, Debug and LegacyTagging apply at the next game start.");
    ImGui::End();
}

static void drawFps()
{
    const BridgeStats& s = g_stats;
    const ModStatus& st = g_status;
    ImGuiIO& io = ImGui::GetIO();
    const float pad = 10.0f * g_cfg.overlayScale;
    int c = g_cfg.fpsCorner;
    ImVec2 pos((c == 1 || c == 3) ? io.DisplaySize.x - pad : pad, (c >= 2) ? io.DisplaySize.y - pad : pad);
    ImVec2 pivot((c == 1 || c == 3) ? 1.0f : 0.0f, (c >= 2) ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##fsr2dlss_fps", nullptr, fl)) {
        if (s.fgApplied >= 1 && s.displayFps > 0 && s.displayFps > s.baseFps * 1.05f) {
            ImGui::Text("%.0f fps", s.displayFps);
            ImGui::SameLine(); ImGui::TextDisabled("|");
            ImGui::SameLine(); ImGui::Text("base %.0f", s.baseFps);
            ImGui::SameLine(); ImGui::TextDisabled("|");
            if (s.fgApplied >= 2) { ImGui::SameLine(); ImGui::TextColored(kOk, "%ux", s.fgApplied); }
            else                  { ImGui::SameLine(); ImGui::TextColored(kOk, "dyn %.1fx", s.baseFps > 0 ? s.displayFps / s.baseFps : 0.0f); }
        } else {
            ImGui::Text("%.0f fps", s.baseFps);
            ImGui::SameLine(); ImGui::TextDisabled("|  FG off");
        }
        bool fsrLive = st.lastUpscaleTick && (GetTickCount64() - st.lastUpscaleTick) < 1000;
        if (s.srMode >= 0 && fsrLive) { ImGui::SameLine(); ImGui::TextDisabled("|  DLSS %s", srModeName(s.srMode)); }
        else if (!fsrLive)            { ImGui::SameLine(); ImGui::TextDisabled("|  no FSR"); }
    }
    ImGui::End();
}

// ---- per-frame render ---------------------------------------------------------------------------------------
void overlay_render(IDXGISwapChain* sc, ID3D12CommandQueue* queue, bool slPath)
{
    if (g_failed || !sc || !queue) return;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }

    // Back-buffer description (size/format can change under us on ResizeBuffers).
    IDXGISwapChain1* sc1 = nullptr;
    DXGI_SWAP_CHAIN_DESC1 d{};
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) || !sc1) return;
    HRESULT dh = sc1->GetDesc1(&d); sc1->Release();
    if (FAILED(dh) || !d.Width || !d.Height) return;

    if (sc != g_sc || queue != g_queue || slPath != g_slPath) { if (!bind(sc, queue, slPath, d)) return; }
    updateRates(slPath);
    pollToggleKey();
    bool visible = g_visible != 0;
    if (!visible && !g_cfg.showFps) return;

    // Layers for DLSS-G only make sense when frame generation is actually running on this swapchain.
    bool wantLayers = slPath && g_cfg.uiLayer && g_stats.fgApplied > 0;
    g_layersValid = false;
    if (!visible && !g_cfg.showFps && !wantLayers) return;

    g_bbW = d.Width; g_bbH = d.Height;
    if (!slPath && d.Format != g_compFmt) g_colorMode = deriveColorMode(sc, d.Format);
    DXGI_FORMAT uiFmt = uiFormatFor(g_colorMode);
    if (g_dx12Ready && uiFmt != g_rtvFormat) { OL("[OVL] UI layer format changed %u -> %u, recreating backend", (unsigned)g_rtvFormat, (unsigned)uiFmt); shutdownDx12Backend(); }
    if (!initImGui(uiFmt)) { g_failed = true; return; }
    if (!ensureLayerTextures(g_bbW, g_bbH, uiFmt, d.Format, wantLayers)) { g_failed = true; return; }
    if (!createComposite(d.Format)) { g_failed = true; return; }
    if (g_appliedScale != g_cfg.overlayScale) applyStyle();

    // Wait for the allocator we are about to reuse.
    FrameRes& fr = g_frames[g_frameIdx];
    if (fr.fenceValue && g_fence->GetCompletedValue() < fr.fenceValue) {
        g_fence->SetEventOnCompletion(fr.fenceValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 1000);
    }

    ID3D12Resource* bb = nullptr;
    UINT bbIndex = 0;
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) { bbIndex = sc3->GetCurrentBackBufferIndex(); sc3->Release(); }
    if (FAILED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb))) || !bb) return;

    if (FAILED(fr.alloc->Reset()) || FAILED(g_cmd->Reset(fr.alloc, nullptr))) { bb->Release(); return; }

    UINT rtvInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvBb = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvUi = { rtvBb.ptr + rtvInc };
    g_dev->CreateRenderTargetView(bb, nullptr, rtvBb);
    g_dev->CreateRenderTargetView(g_uiTex, nullptr, rtvUi);

    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (from == to) return;
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from; b.Transition.StateAfter = to;
        g_cmd->ResourceBarrier(1, &b);
    };

    // 1. Hud-less copy of the finished game frame (before the overlay goes in). Stays COPY_DEST.
    if (wantLayers && g_hudTex) {
        barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_cmd->CopyResource(g_hudTex, bb);
        barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else {
        barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    // 2. Draw the UI into its own (cleared, premultiplied) layer.
    barrier(g_uiTex, g_uiState, D3D12_RESOURCE_STATE_RENDER_TARGET); g_uiState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const float clear[4] = { 0, 0, 0, 0 };
    g_cmd->ClearRenderTargetView(rtvUi, clear, 0, nullptr);
    g_cmd->OMSetRenderTargets(1, &rtvUi, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
    g_cmd->SetDescriptorHeaps(1, heaps);

    EnterCriticalSection(&g_cs);
    ImGuiIO& io = ImGui::GetIO();
    if (visible) io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard);
    else         io.ConfigFlags |=  (ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard);
    io.MouseDrawCursor = visible;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();   // sets io.DisplaySize from the window's client rect
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) io.DisplaySize = ImVec2((float)g_bbW, (float)g_bbH);
    // UI space is the client rect (so mouse coordinates line up); the back buffer may be a different
    // size (borderless scaling, DPI), so scale the viewport/scissor to it.
    io.DisplayFramebufferScale = ImVec2((float)g_bbW / io.DisplaySize.x, (float)g_bbH / io.DisplaySize.y);
    ImGui::NewFrame();
    if (g_cfg.showFps) drawFps();
    if (visible) drawMenu();
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd);
    LeaveCriticalSection(&g_cs);

    // 3. Composite the UI layer onto the back buffer.
    barrier(g_uiTex, g_uiState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_uiState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_cmd->OMSetRenderTargets(1, &rtvBb, FALSE, nullptr);
    D3D12_VIEWPORT vp{ 0, 0, (float)g_bbW, (float)g_bbH, 0, 1 }; D3D12_RECT scissor{ 0, 0, (LONG)g_bbW, (LONG)g_bbH };
    g_cmd->RSSetViewports(1, &vp); g_cmd->RSSetScissorRects(1, &scissor);
    g_cmd->SetGraphicsRootSignature(g_compRs);
    g_cmd->SetPipelineState(g_compPso);
    g_cmd->SetGraphicsRootDescriptorTable(0, g_uiSrvGpu);
    g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmd->DrawInstanced(3, 1, 0, 0);
    barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_layersValid = wantLayers && g_hudTex != nullptr;
    if (SUCCEEDED(g_cmd->Close())) {
        ID3D12CommandList* lists[] = { g_cmd };
        g_queue->ExecuteCommandLists(1, lists);
        fr.fenceValue = ++g_fenceCounter;
        g_queue->Signal(g_fence, fr.fenceValue);
    }
    bb->Release();   // the swapchain keeps the buffer alive; never hold a reference across frames
    g_frameIdx = (g_frameIdx + 1) % kFramesInFlight;
}

bool overlay_fg_layers(OverlayLayers* out)
{
    if (!out || !g_layersValid || !g_uiTex || !g_hudTex) return false;
    out->hudless = g_hudTex; out->hudlessState = (unsigned)D3D12_RESOURCE_STATE_COPY_DEST;
    out->ui = g_uiTex;       out->uiState = (unsigned)g_uiState;
    out->width = g_layerW;   out->height = g_layerH;
    return true;
}
