// nvhook.cpp — NvAPI interposer. Optionally disable Blackwell hardware flip metering so DLSS-G
// falls back to its software pacer: return null for NvAPI_D3D12_SetFlipConfig (id 0xF3148C42) when
// FlipMetering != hardware. With FlipMetering=hardware (default) we don't block, and the display
// engine's hardware flip queue paces the generated frames (true 3x/4x MFG).
// Ref: OptiScaler nvapi/NvApiHooks.cpp; NVIDIA nvapi_interface.h { "NvAPI_D3D12_SetFlipConfig", 0xf3148c42 }.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include "MinHook.h"

extern "C" void bridge_logs(const char* s);
static void NL(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n] = '\n'; buf[n + 1] = 0;
    bridge_logs(buf);
}

static const unsigned int NVAPI_ID_D3D12_SetFlipConfig = 0xF3148C42u;

typedef void* (__cdecl* PFN_nvapi_QueryInterface)(unsigned int id);
static PFN_nvapi_QueryInterface o_nvapi_QI = nullptr;
static bool g_blockFlip = true;   // set from config (FlipMetering != hardware)

static void* __cdecl hk_nvapi_QueryInterface(unsigned int id)
{
    if (id == NVAPI_ID_D3D12_SetFlipConfig && g_blockFlip)
        return nullptr;   // hide SetFlipConfig -> DLSS-G uses the software pacer
    return o_nvapi_QI ? o_nvapi_QI(id) : nullptr;
}

extern "C" void nvhook_set_flip_block(bool block) { g_blockFlip = block; }

// Install the nvapi_QueryInterface hook. Call BEFORE slInit / before the DLSS-G swapchain.
extern "C" bool nvhook_install()
{
    HMODULE nv = GetModuleHandleW(L"nvapi64.dll");
    if (!nv) nv = LoadLibraryW(L"nvapi64.dll");
    if (!nv) { NL("[NVHOOK] nvapi64.dll not present"); return false; }

    void* qi = (void*)GetProcAddress(nv, "nvapi_QueryInterface");
    if (!qi) { NL("[NVHOOK] nvapi_QueryInterface export not found"); return false; }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { NL("[NVHOOK] MH_Initialize failed (%d)", (int)s); return false; }

    s = MH_CreateHook(qi, (void*)&hk_nvapi_QueryInterface, (void**)&o_nvapi_QI);
    if (s != MH_OK) { NL("[NVHOOK] MH_CreateHook failed (%d)", (int)s); return false; }

    s = MH_EnableHook(qi);
    if (s != MH_OK) { NL("[NVHOOK] MH_EnableHook failed (%d)", (int)s); return false; }

    NL("[NVHOOK] nvapi_QueryInterface@%p hooked (flipBlock=%d)", qi, (int)g_blockFlip);
    return true;
}
