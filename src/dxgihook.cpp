// dxgihook.cpp - hooks on the DXGI swapchain and D3D12 command queue the game already uses.
//
// The bridge only owns a swapchain while the game runs frame generation (the FSR FG swapchain is
// replaced by Streamline's DLSS-G one). To draw the overlay at all other times we detour the native
// IDXGISwapChain::Present/Present1/ResizeBuffers* once, process-wide: every DXGI swapchain in the
// process shares the runtime's vtable, so we create a tiny throwaway device + swapchain on a hidden
// window, read the function addresses, hook them with MinHook, and throw the objects away. The game's
// direct command queue (needed to execute the overlay's command list) is captured the same way from
// ID3D12CommandQueue::ExecuteCommandLists.
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
typedef void    (STDMETHODCALLTYPE *PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static PFN_Present             o_Present        = nullptr;
static PFN_Present1            o_Present1       = nullptr;
static PFN_ResizeBuffers       o_ResizeBuffers  = nullptr;
static PFN_ResizeBuffers1      o_ResizeBuffers1 = nullptr;
static PFN_ExecuteCommandLists o_Execute        = nullptr;

static ID3D12CommandQueue* volatile g_directQueue = nullptr;
static ID3D12CommandQueue*          g_lastSeenQueue = nullptr;

ID3D12CommandQueue* dxgihook_direct_queue() { return g_directQueue; }

static void STDMETHODCALLTYPE hk_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (q && q != g_lastSeenQueue && q != g_directQueue) {
        g_lastSeenQueue = q;
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_directQueue = q;
            g_status.directQueueKnown = true;
            DL("[DXGI] direct command queue %p captured", (void*)q);
        }
    }
    o_Execute(q, n, lists);
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    ++g_status.nativePresents;
    if (!(flags & DXGI_PRESENT_TEST) && !slbridge_owns_present()) overlay_render(sc, g_directQueue, false);
    return o_Present(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    ++g_status.nativePresents;
    if (!(flags & DXGI_PRESENT_TEST) && !slbridge_owns_present()) overlay_render(sc, g_directQueue, false);
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
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { DL("[DXGI] MH_CreateHook(%s) failed (%d)", name, (int)s); return false; }
    s = MH_EnableHook(target);
    if (s != MH_OK && s != MH_ERROR_ENABLED) { DL("[DXGI] MH_EnableHook(%s) failed (%d)", name, (int)s); return false; }
    return true;
}

static DWORD WINAPI installThread(LPVOID)
{
    proxy_init_logging();
    if (!mh_ensure_init()) { g_status.dxgiHookFailed = true; return 0; }

    // Throwaway window, device, queue and swapchain: we only want their vtables.
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"fsr2dlss_dxgi_probe";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"fsr2dlss", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDXGIFactory2* factory = nullptr; ID3D12Device* dev = nullptr; ID3D12CommandQueue* q = nullptr; IDXGISwapChain1* sc = nullptr;
    bool ok = false;
    do {
        if (!wnd) { DL("[DXGI] probe window failed err=%lu", GetLastError()); break; }
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { DL("[DXGI] CreateDXGIFactory1 failed"); break; }
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        if (FAILED(hr)) { DL("[DXGI] D3D12CreateDevice failed hr=0x%08lx", (unsigned long)hr); break; }
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) { DL("[DXGI] CreateCommandQueue failed"); break; }
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = 64; d.Height = 64; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(q, wnd, &d, nullptr, nullptr, &sc);
        if (FAILED(hr) || !sc) { DL("[DXGI] probe CreateSwapChainForHwnd failed hr=0x%08lx", (unsigned long)hr); break; }

        void** scv = *(void***)sc;   // IDXGISwapChain: Present=8 ResizeBuffers=13 | IDXGISwapChain1: Present1=22 | IDXGISwapChain3: ResizeBuffers1=39
        void** qv  = *(void***)q;    // ID3D12CommandQueue: ExecuteCommandLists=10
        ok  = hookOne(scv[8],  (void*)&hk_Present,             (void**)&o_Present,        "Present");
        ok &= hookOne(scv[22], (void*)&hk_Present1,            (void**)&o_Present1,       "Present1");
        ok &= hookOne(scv[13], (void*)&hk_ResizeBuffers,       (void**)&o_ResizeBuffers,  "ResizeBuffers");
        ok &= hookOne(scv[39], (void*)&hk_ResizeBuffers1,      (void**)&o_ResizeBuffers1, "ResizeBuffers1");
        ok &= hookOne(qv[10],  (void*)&hk_ExecuteCommandLists, (void**)&o_Execute,        "ExecuteCommandLists");
    } while (false);
    rel(sc); rel(q); rel(dev); rel(factory);
    if (wnd) DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    g_status.dxgiHooked = ok; g_status.dxgiHookFailed = !ok;
    DL("[DXGI] native swapchain hooks %s (Present=%p Present1=%p ResizeBuffers=%p Execute=%p)", ok ? "installed" : "FAILED",
       (void*)o_Present, (void*)o_Present1, (void*)o_ResizeBuffers, (void*)o_Execute);
    return 0;
}

void dxgihook_start()
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0) return;
    HANDLE h = CreateThread(nullptr, 0, installThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
