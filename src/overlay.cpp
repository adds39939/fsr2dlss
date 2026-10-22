// ============================================================================
// In-game overlay: Dear ImGui rendered straight into the DLSS-G swapchain's
// back buffer from the Present hook, on the game's own present queue.
//
//  - Toggle: Config::overlayKey (Home), polled with GetAsyncKeyState so it works
//    no matter how the game reads input (raw input, DirectInput, messages).
//  - Input: the swapchain window's WndProc is subclassed. While the menu is open
//    keyboard/mouse messages go to ImGui and are swallowed so the game does not
//    react; SetCursorPos/ClipCursor are hooked (MinHook) so the game cannot
//    re-centre or hide the pointer. ImGui draws its own cursor.
//  - HDR: the bridge tells us the swapchain colour space; the patched DX12
//    backend applies the matching output transform (see third_party/imgui_backends).
//  - GPU: one command list, 3 rotating allocators fenced on the present queue.
//    Back-buffer references are released every frame so ResizeBuffers is never
//    blocked by us.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
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
#include "overlay.h"

extern "C" void bridge_logs(const char* s);
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

static ID3D12Device*              g_dev        = nullptr;
static ID3D12CommandQueue*        g_queue      = nullptr;
static IDXGISwapChain3*           g_sc         = nullptr;   // not AddRef'd; owned by the bridge
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
static DXGI_FORMAT                g_rtvFormat  = DXGI_FORMAT_UNKNOWN;
static UINT                       g_bbW = 0, g_bbH = 0;
static bool                       g_imguiReady = false;
static bool                       g_dx12Ready  = false;
static bool                       g_failed     = false;    // give up after a hard failure (no spam)
static int                        g_colorMode  = 0;        // 0 sdr, 1 pq, 2 scrgb
static float                      g_appliedScale = 0.0f;

static volatile LONG              g_visible    = 0;
static bool                       g_keyWasDown = false;
static bool                       g_saveOk = false, g_saveTried = false; static ULONGLONG g_saveMsgUntil = 0;

static CRITICAL_SECTION           g_cs;                    // guards ImGui context (WndProc vs present thread)
static bool                       g_csInit     = false;
static WNDPROC                    g_origWndProc = nullptr;
static bool                       g_wndUnicode = true;

// ---- user32 hooks: keep the game from fighting the overlay cursor ---------------------------------
typedef BOOL (WINAPI* PFN_SetCursorPos)(int, int);
typedef BOOL (WINAPI* PFN_ClipCursor)(const RECT*);
static PFN_SetCursorPos o_SetCursorPos = nullptr;
static PFN_ClipCursor   o_ClipCursor   = nullptr;
static BOOL WINAPI hk_SetCursorPos(int x, int y) { if (g_visible) return TRUE; return o_SetCursorPos ? o_SetCursorPos(x, y) : FALSE; }
static BOOL WINAPI hk_ClipCursor(const RECT* r)  { if (g_visible) return TRUE; return o_ClipCursor ? o_ClipCursor(r) : FALSE; }
static void installCursorHooks()
{
    static bool done = false; if (done) return; done = true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { OL("[OVL] MH_Initialize failed (%d)", (int)s); return; }
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

static void installWndProc()
{
    if (!g_hwnd || g_origWndProc) return;
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
    D3D12_DESCRIPTOR_HEAP_DESC rd{}; rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rd.NumDescriptors = 1;
    if (FAILED(g_dev->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&g_rtvHeap)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC sd{}; sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sd.NumDescriptors = kSrvHeapSize; sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_srvHeap)))) return false;
    g_srvInc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

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
        g_rtvFormat = fmt; g_dx12Ready = true;
        OL("[OVL] ImGui DX12 backend ready (rtv fmt=%u, color mode=%d)", (unsigned)fmt, g_colorMode);
    }
    ImGui_ImplDX12_SetOutputTransform(g_colorMode, g_cfg.overlayNits);
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

// ---- public API -------------------------------------------------------------------------------------------
bool overlay_init(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* sc, void* hwnd)
{
    if (g_dev) overlay_shutdown();
    if (!device || !queue || !sc) return false;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    g_dev = device; g_dev->AddRef();
    g_queue = queue; g_queue->AddRef();
    g_sc = sc;
    g_hwnd = (HWND)hwnd;
    if (!g_hwnd) sc->GetHwnd(&g_hwnd);
    g_failed = false; g_frameIdx = 0;
    if (!createGpuObjects()) { OL("[OVL] failed to create GPU objects"); g_failed = true; return false; }
    DXGI_SWAP_CHAIN_DESC1 d{}; if (SUCCEEDED(sc->GetDesc1(&d))) {
        g_bbW = d.Width; g_bbH = d.Height;
        if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) g_colorMode = 2;
    }
    installWndProc();
    installCursorHooks();
    OL("[OVL] initialised: %ux%u key=%s overlay=%d fps=%d", g_bbW, g_bbH, config_key_name(g_cfg.overlayKey), (int)g_cfg.overlayOn, (int)g_cfg.showFps);
    return true;
}

void overlay_set_colorspace(int mode)
{
    g_colorMode = mode;
    if (g_dx12Ready) ImGui_ImplDX12_SetOutputTransform(g_colorMode, g_cfg.overlayNits);
}

void overlay_before_resize()
{
    if (!g_dev) return;
    waitGpuIdle();   // our last command list referenced a back buffer; make sure it is done
    // A format change needs a new pipeline state; sizes are re-read every frame.
    g_bbW = g_bbH = 0;
}

void overlay_shutdown()
{
    if (!g_dev) return;
    InterlockedExchange(&g_visible, 0);
    shutdownDx12Backend();
    if (g_imguiReady) { ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); g_imguiReady = false; }
    if (g_hwnd && g_origWndProc) {
        if (g_wndUnicode) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc); else SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
        g_origWndProc = nullptr;
    }
    for (UINT i = 0; i < kFramesInFlight; ++i) { rel(g_frames[i].alloc); g_frames[i].fenceValue = 0; }
    rel(g_cmd); rel(g_fence); rel(g_rtvHeap); rel(g_srvHeap);
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    rel(g_queue); rel(g_dev); g_sc = nullptr; g_hwnd = nullptr;
    g_rtvFormat = DXGI_FORMAT_UNKNOWN;
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
        if (!now && g_imguiReady) { ImGuiIO& io = ImGui::GetIO(); io.ClearInputKeys(); io.ClearInputMouse(); }
    }
    g_keyWasDown = down;
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

static void drawMenu()
{
    const BridgeStats& s = g_stats;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560 * g_cfg.overlayScale, 0), ImGuiCond_FirstUseEver);
    char title[96]; snprintf(title, sizeof(title), "fsr2dlss  -  DLSS %s / Streamline %u.%u.%u###fsr2dlss", s.srActive ? "SR + FG" : "FG", s.slVersion[0], s.slVersion[1], s.slVersion[2]);
    if (!ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }

    // status
    ImGui::TextDisabled("Press %s to close. Changes apply immediately; Save writes fsr2dlss.ini.", config_key_name(g_cfg.overlayKey));
    ImGui::Separator();
    ImGui::Text("Framerate: %.0f fps displayed, %.0f fps rendered", s.displayFps, s.baseFps);
    if (s.fgApplied >= 2)      ImGui::Text("Frame generation: %ux (max %ux on this card)", s.fgApplied, s.fgMaxGen + 1);
    else if (s.fgApplied == 1) ImGui::Text("Frame generation: dynamic (max %ux on this card)", s.fgMaxGen + 1);
    else                       ImGui::Text("Frame generation: off%s", s.fgGameEnabled ? "" : " (turn Frame Generation on in the game's graphics menu)");
    if (s.fgStatus) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "DLSS-G status 0x%x:%s%s%s%s%s", s.fgStatus,
            (s.fgStatus & 1) ? " resolution-too-low" : "", (s.fgStatus & 2) ? " reflex-missing" : "",
            (s.fgStatus & 4) ? " hdr-format" : "", (s.fgStatus & 8) ? " constants-invalid" : "", (s.fgStatus & 16) ? " backbuffer-index" : "");
    }
    if (s.srMode >= 0) ImGui::Text("Super resolution: DLSS %s, preset %s, %ux%u -> %ux%u", srModeName(s.srMode), config_sr_preset_name(s.srPreset), s.renderW, s.renderH, s.outW, s.outH);
    else               ImGui::Text("Super resolution: %s", g_cfg.dlssSR ? "waiting for the game's upscale pass" : "FSR (DLSS disabled)");
    ImGui::Separator();

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
        if (mult > maxMult) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "This card reports %dx as its maximum; %dx will be clamped.", maxMult, mult);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(g_cfg.fgMode != FG_DYNAMIC);
        float tfps = g_cfg.fgDynamicFps;
        if (ImGui::InputFloat("Dynamic target fps (0 = refresh rate)", &tfps, 1.0f, 10.0f, "%.0f")) { if (tfps < 0) tfps = 0; if (tfps > 999) tfps = 999; g_cfg.fgDynamicFps = tfps; changed(); }
        ImGui::EndDisabled();
        static const char* flipNames[3] = { "Software pacer (clean 2x)", "Hardware flip metering (true 3x/4x)", "Auto" };
        if (comboInt("Flip metering (needs restart)", &g_cfg.flipMode, flipNames, 3)) changed();
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
            if (ImGui::SliderFloat("HDR brightness (nits)", &nits, 50.0f, 1000.0f, "%.0f")) { g_cfg.overlayNits = nits; ImGui_ImplDX12_SetOutputTransform(g_colorMode, nits); changed(); }
        }
        if (ImGui::Checkbox("Steam overlay reports DLSS", &g_cfg.steamFix)) changed();
        ImGui::TextDisabled("Toggle key: %s (change ToggleKey in fsr2dlss.ini)", config_key_name(g_cfg.overlayKey));
    }

    ImGui::Separator();
    if (ImGui::Button("Save to fsr2dlss.ini", ImVec2(200 * g_cfg.overlayScale, 0))) { g_saveOk = config_save(); g_saveTried = true; g_saveMsgUntil = GetTickCount64() + 3000; }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(120 * g_cfg.overlayScale, 0))) InterlockedExchange(&g_visible, 0);
    if (g_saveTried && GetTickCount64() < g_saveMsgUntil) { ImGui::SameLine(); if (g_saveOk) ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "Saved."); else ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Save failed (see ffx_bridge.log)."); }
    ImGui::TextDisabled("Flip metering and LegacyTagging apply at the next game start.");
    ImGui::End();
}

static void drawFps()
{
    const BridgeStats& s = g_stats;
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
            if (s.fgApplied >= 2) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "%ux", s.fgApplied); }
            else                  { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "dyn %.1fx", s.baseFps > 0 ? s.displayFps / s.baseFps : 0.0f); }
        } else {
            ImGui::Text("%.0f fps", s.baseFps);
            ImGui::SameLine(); ImGui::TextDisabled("|  FG off");
        }
        if (s.srMode >= 0) { ImGui::SameLine(); ImGui::TextDisabled("|  DLSS %s", srModeName(s.srMode)); }
    }
    ImGui::End();
}

// ---- per-frame render ---------------------------------------------------------------------------------------
void overlay_render(IDXGISwapChain3* sc)
{
    if (!g_dev || g_failed || !sc) return;
    pollToggleKey();
    bool visible = g_visible != 0;
    if (!visible && !g_cfg.showFps) return;

    // Back-buffer description (size/format can change under us on ResizeBuffers).
    DXGI_SWAP_CHAIN_DESC1 d{};
    if (FAILED(sc->GetDesc1(&d)) || !d.Width || !d.Height) return;
    g_bbW = d.Width; g_bbH = d.Height;
    if (g_dx12Ready && d.Format != g_rtvFormat) { OL("[OVL] back-buffer format changed %u -> %u, recreating backend", (unsigned)g_rtvFormat, (unsigned)d.Format); shutdownDx12Backend(); }
    if (!initImGui(d.Format)) { g_failed = true; return; }
    if (g_appliedScale != g_cfg.overlayScale) applyStyle();

    // Wait for the allocator we are about to reuse.
    FrameRes& fr = g_frames[g_frameIdx];
    if (fr.fenceValue && g_fence->GetCompletedValue() < fr.fenceValue) {
        g_fence->SetEventOnCompletion(fr.fenceValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 1000);
    }

    ID3D12Resource* bb = nullptr;
    UINT bbIndex = sc->GetCurrentBackBufferIndex();
    if (FAILED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb))) || !bb) return;

    if (FAILED(fr.alloc->Reset()) || FAILED(g_cmd->Reset(fr.alloc, nullptr))) { bb->Release(); return; }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_dev->CreateRenderTargetView(bb, nullptr, rtv);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cmd->ResourceBarrier(1, &b);
    g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
    g_cmd->SetDescriptorHeaps(1, heaps);

    if (g_csInit) EnterCriticalSection(&g_cs);
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
    if (g_csInit) LeaveCriticalSection(&g_cs);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmd->ResourceBarrier(1, &b);
    if (SUCCEEDED(g_cmd->Close())) {
        ID3D12CommandList* lists[] = { g_cmd };
        g_queue->ExecuteCommandLists(1, lists);
        fr.fenceValue = ++g_fenceCounter;
        g_queue->Signal(g_fence, fr.fenceValue);
    }
    bb->Release();   // the swapchain keeps the buffer alive; never hold a reference across frames
    g_frameIdx = (g_frameIdx + 1) % kFramesInFlight;
}
