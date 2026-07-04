// nvhook.cpp — surgically disable NVIDIA Blackwell hardware flip metering so SL DLSS-G
// falls back to software frame pacing (which actually PRESENTS the interpolated frame,
// even when our late-hook Reflex cadence is degenerate). This is exactly what OptiScaler's
// [NvApi] DisableFlipMetering does: hook nvapi64.dll!nvapi_QueryInterface and return null
// ONLY for NvAPI_D3D12_SetFlipConfig (id 0xF3148C42), forwarding every other query intact.
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
static volatile LONG g_blockedCount = 0;

static void* __cdecl hk_nvapi_QueryInterface(unsigned int id)
{
    if (id == NVAPI_ID_D3D12_SetFlipConfig) {
        LONG c = InterlockedIncrement(&g_blockedCount);
        if (c == 1) NL("[NVHOOK] blocked NvAPI_D3D12_SetFlipConfig (0x%08X) -> DLSS-G software pacing", id);
        return nullptr;
    }
    return o_nvapi_QI ? o_nvapi_QI(id) : nullptr;
}

// Install the flip-metering block. Call BEFORE slInit / before the DLSS-G swapchain is created.
extern "C" bool nvhook_block_flipmetering()
{
    HMODULE nv = GetModuleHandleW(L"nvapi64.dll");
    if (!nv) nv = LoadLibraryW(L"nvapi64.dll");
    if (!nv) { NL("[NVHOOK] nvapi64.dll not present; cannot block flip metering"); return false; }

    void* qi = (void*)GetProcAddress(nv, "nvapi_QueryInterface");
    if (!qi) { NL("[NVHOOK] nvapi_QueryInterface export not found"); return false; }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { NL("[NVHOOK] MH_Initialize failed (%d)", (int)s); return false; }

    s = MH_CreateHook(qi, (void*)&hk_nvapi_QueryInterface, (void**)&o_nvapi_QI);
    if (s != MH_OK) { NL("[NVHOOK] MH_CreateHook failed (%d)", (int)s); return false; }

    s = MH_EnableHook(qi);
    if (s != MH_OK) { NL("[NVHOOK] MH_EnableHook failed (%d)", (int)s); return false; }

    NL("[NVHOOK] flip-metering block armed (nvapi_QueryInterface@%p hooked; orig=%p)", qi, (void*)o_nvapi_QI);
    return true;
}
