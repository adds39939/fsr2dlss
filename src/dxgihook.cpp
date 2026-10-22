// dxgihook.cpp - hooks on the DXGI swapchain and D3D12 command queue the game already uses.
//
// The bridge only owns a swapchain while the game runs frame generation (the FSR FG swapchain is
// replaced by Streamline's DLSS-G one). To draw the overlay at all other times we detour the native
// IDXGISwapChain::Present/Present1/ResizeBuffers*/SetColorSpace1 once, process-wide: every DXGI
// swapchain in the process shares the runtime's vtable, so we create a tiny probe swapchain on a
// hidden window, read the function addresses, hook them with MinHook, and throw the probe away. The
// game's direct command queue (needed to execute the overlay's command list) is captured the same way
// from ID3D12CommandQueue::ExecuteCommandLists, per thread and reference-counted.
//
// The probe is built from the game's own device whenever possible (D3D12CreateDevice is hooked from
// DllMain; the FidelityFX backend descriptor also hands us the device), so the queue hook lands in
// the D3D12 runtime module the game actually uses and no device is created before the game's (which
// would break D3D12EnableExperimentalFeatures / debug-layer setup). Only if no device shows up for a
// while do we create a throwaway one.
//
// When the DLSS-G swapchain is live, the game presents through Streamline's proxy (hooked in place by
// slbridge.cpp, which draws the overlay there) and Streamline presents the real + generated frames
// through a native swapchain that lands here; those presents are passed straight through.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdarg>
#include "MinHook.h"
#include "dxgihook.h"
#include "overlay.h"
#include "status.h"

extern "C" void bridge_logs(const char* s);
extern "C" bool mh_ensure_init();              // nvhook.cpp: thread-safe MH_Initialize
extern "C" bool slbridge_owns_present();       // slbridge.cpp: DLSS-G swapchain live -> overlay drawn there
extern "C" void proxy_init_logging();          // proxy.cpp: load config, enable the log if Debug=true
static void DL(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n] = '\n'; buf[n + 1] = 0;
    bridge_logs(buf);
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers1)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetColorSpace1)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
typedef void    (STDMETHODCALLTYPE *PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

static PFN_Present             o_Present        = nullptr;
static PFN_Present1            o_Present1       = nullptr;
static PFN_ResizeBuffers       o_ResizeBuffers  = nullptr;
static PFN_ResizeBuffers1      o_ResizeBuffers1 = nullptr;
static PFN_SetColorSpace1      o_SetColorSpace1 = nullptr;
static PFN_ExecuteCommandLists o_Execute        = nullptr;
static PFN_D3D12CreateDevice   o_D3D12CreateDevice = nullptr;

// ---- per-thread direct queue (reference-counted) -------------------------------------------------------
struct TlsQueue {
    ID3D12CommandQueue* q = nullptr;
    ~TlsQueue() { if (q) q->Release(); }
};
static thread_local TlsQueue t_direct;

ID3D12CommandQueue* dxgihook_direct_queue() { return t_direct.q; }

static void STDMETHODCALLTYPE hk_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (q && q != t_direct.q) {
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            q->AddRef();
            if (t_direct.q) t_direct.q->Release();
            t_direct.q = q;
            if (!g_status.directQueueKnown) { g_status.directQueueKnown = true; DL("[DXGI] direct command queue %p captured (tid=%lu)", (void*)q, GetCurrentThreadId()); }
        }
    }
    o_Execute(q, n, lists);
}

// ---- colour space per swapchain --------------------------------------------------------------------------
struct CsEntry { IDXGISwapChain* sc; int cs; };
static CsEntry g_cs[8] = {};
static CRITICAL_SECTION g_csLock; static bool g_csLockInit = false;
static void csLock()   { if (!g_csLockInit) { InitializeCriticalSection(&g_csLock); g_csLockInit = true; } EnterCriticalSection(&g_csLock); }
static void csUnlock() { LeaveCriticalSection(&g_csLock); }

int dxgihook_colorspace(IDXGISwapChain* sc)
{
    int r = -1;
    csLock();
    for (const CsEntry& e : g_cs) if (e.sc == sc) { r = e.cs; break; }
    csUnlock();
    return r;
}
static HRESULT STDMETHODCALLTYPE hk_SetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs)
{
    HRESULT hr = o_SetColorSpace1(sc, cs);
    if (SUCCEEDED(hr)) {
        csLock();
        int slot = -1;
        for (int i = 0; i < 8; ++i) if (g_cs[i].sc == (IDXGISwapChain*)sc) { slot = i; break; }
        if (slot < 0) for (int i = 0; i < 8; ++i) if (!g_cs[i].sc) { slot = i; break; }
        if (slot < 0) slot = 0;   // table full: recycle (8 swapchains at once is already unusual)
        g_cs[slot] = { (IDXGISwapChain*)sc, (int)cs };
        csUnlock();
    }
    return hr;
}

// ---- swapchain hooks ---------------------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    ++g_status.nativePresents;
    if (!(flags & DXGI_PRESENT_TEST) && !slbridge_owns_present()) overlay_render(sc, t_direct.q, false);
    return o_Present(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    ++g_status.nativePresents;
    if (!(flags & DXGI_PRESENT_TEST) && !slbridge_owns_present()) overlay_render(sc, t_direct.q, false);
    return o_Present1(sc, sync, flags, p);
}
static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    overlay_before_resize();
    return o_ResizeBuffers(sc, c, w, h, f, fl);
}
static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers1(IDXGISwapChain3* sc, UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* nodes, IUnknown* const* q)
{
    overlay_before_resize();
    return o_ResizeBuffers1(sc, c, w, h, f, fl, nodes, q);
}

template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static bool hookOne(void* target, void* detour, void** original, const char* name)
{
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) { DL("[DXGI] MH_CreateHook(%s) failed (%d)", name, (int)s); return false; }
    s = MH_EnableHook(target);
    if (s != MH_OK) { DL("[DXGI] MH_EnableHook(%s) failed (%d)", name, (int)s); return false; }
    return true;
}

// ---- probe: install the hooks from a device -----------------------------------------------------------------
static volatile LONG g_probeState = 0;         // 0 = waiting for a device, 1 = probing, 2 = done
static ID3D12Device* volatile g_gameDevice = nullptr;   // AddRef'd, the device to probe with
static HANDLE g_deviceEvent = nullptr;

static void probeWith(ID3D12Device* dev, bool ownDevice)
{
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"fsr2dlss_dxgi_probe";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"fsr2dlss", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDXGIFactory2* factory = nullptr; ID3D12CommandQueue* q = nullptr; IDXGISwapChain1* sc = nullptr;
    bool present = false, rest = true;
    do {
        if (!wnd) { DL("[DXGI] probe window failed err=%lu", GetLastError()); break; }
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { DL("[DXGI] CreateDXGIFactory1 failed"); break; }
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) { DL("[DXGI] probe CreateCommandQueue failed"); break; }
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = 64; d.Height = 64; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = factory->CreateSwapChainForHwnd(q, wnd, &d, nullptr, nullptr, &sc);
        if (FAILED(hr) || !sc) { DL("[DXGI] probe CreateSwapChainForHwnd failed hr=0x%08lx", (unsigned long)hr); break; }

        void** scv = *(void***)sc;   // IDXGISwapChain: Present=8 ResizeBuffers=13 | IDXGISwapChain1: Present1=22 | IDXGISwapChain3: SetColorSpace1=38 ResizeBuffers1=39
        void** qv  = *(void***)q;    // ID3D12CommandQueue: ExecuteCommandLists=10
        bool p0 = hookOne(scv[8],  (void*)&hk_Present,  (void**)&o_Present,  "Present");
        bool p1 = hookOne(scv[22], (void*)&hk_Present1, (void**)&o_Present1, "Present1");
        present = p0 && p1;
        rest &= hookOne(scv[13], (void*)&hk_ResizeBuffers,       (void**)&o_ResizeBuffers,  "ResizeBuffers");
        rest &= hookOne(scv[39], (void*)&hk_ResizeBuffers1,      (void**)&o_ResizeBuffers1, "ResizeBuffers1");
        rest &= hookOne(scv[38], (void*)&hk_SetColorSpace1,      (void**)&o_SetColorSpace1, "SetColorSpace1");
        rest &= hookOne(qv[10],  (void*)&hk_ExecuteCommandLists, (void**)&o_Execute,        "ExecuteCommandLists");
    } while (false);
    rel(sc); rel(q); rel(factory);
    if (wnd) DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    g_status.dxgiHooked = present; g_status.dxgiHookFailed = !present;
    DL("[DXGI] native swapchain hooks %s%s (device=%p %s; Present=%p Present1=%p ResizeBuffers=%p SetColorSpace1=%p Execute=%p)",
       present ? "installed" : "FAILED", rest ? "" : " (some secondary hooks failed)", (void*)dev, ownDevice ? "throwaway" : "game's",
       (void*)o_Present, (void*)o_Present1, (void*)o_ResizeBuffers, (void*)o_SetColorSpace1, (void*)o_Execute);
}

static DWORD WINAPI probeThread(LPVOID)
{
    proxy_init_logging();
    if (!mh_ensure_init()) { g_status.dxgiHookFailed = true; return 0; }

    // Hook D3D12CreateDevice so we learn about the game's device as soon as it exists.
    if (HMODULE d3d12 = LoadLibraryW(L"d3d12.dll")) {
        void* fn = (void*)GetProcAddress(d3d12, "D3D12CreateDevice");
        extern HRESULT WINAPI hk_D3D12CreateDevice(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
        if (fn && MH_CreateHook(fn, (void*)&hk_D3D12CreateDevice, (void**)&o_D3D12CreateDevice) == MH_OK) MH_EnableHook(fn);
    }

    // Wait for a game device (D3D12CreateDevice hook or the FidelityFX backend descriptor). After a
    // while assume the game created its device before we loaded and use a throwaway one.
    const DWORD timeoutMs = 20000;
    if (!g_gameDevice) WaitForSingleObject(g_deviceEvent, timeoutMs);
    if (InterlockedCompareExchange(&g_probeState, 1, 0) != 0) return 0;

    ID3D12Device* dev = g_gameDevice; bool own = false;
    if (!dev) {
        HRESULT hr = o_D3D12CreateDevice ? o_D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))
                                         : D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        if (FAILED(hr) || !dev) { DL("[DXGI] no game device seen and D3D12CreateDevice failed hr=0x%08lx", (unsigned long)hr); g_status.dxgiHookFailed = true; InterlockedExchange(&g_probeState, 2); return 0; }
        own = true;
        DL("[DXGI] no game device seen within %lu ms; probing with a throwaway device", timeoutMs);
    }
    probeWith(dev, own);
    dev->Release(); g_gameDevice = nullptr;
    InterlockedExchange(&g_probeState, 2);
    return 0;
}

static void offerDevice(ID3D12Device* dev)
{
    if (!dev || g_probeState != 0 || g_gameDevice) return;
    dev->AddRef();
    if (InterlockedCompareExchangePointer((PVOID volatile*)&g_gameDevice, dev, nullptr) != nullptr) { dev->Release(); return; }
    if (g_deviceEvent) SetEvent(g_deviceEvent);
}

HRESULT WINAPI hk_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out)
{
    HRESULT hr = o_D3D12CreateDevice(adapter, fl, riid, out);
    if (SUCCEEDED(hr) && out && *out && g_probeState == 0) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(IID_PPV_ARGS(&dev))) && dev) { offerDevice(dev); dev->Release(); }
    }
    return hr;
}

void dxgihook_notify_device(ID3D12Device* device) { offerDevice(device); }

void dxgihook_start()
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0) return;
    g_deviceEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE h = CreateThread(nullptr, 0, probeThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
