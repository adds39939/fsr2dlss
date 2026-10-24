// nvhook.cpp - small NvAPI interposer. It only exists to control Blackwell flip metering: when
// FlipMetering != hardware we hide NvAPI_D3D12_SetFlipConfig (id 0xF3148C42) from Streamline, which
// makes DLSS-G fall back to its software pacer (clean 2x). With FlipMetering=hardware (default) we
// let the call through and the display engine's hardware flip queue paces the generated frames
// (true 3x/4x MFG). We hook nvapi_QueryInterface to intercept that one function id.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include "MinHook.h"
#include "config.h"

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

// MH_Initialize is not thread-safe and three modules (nvhook, overlay, dxgihook) install hooks from
// different threads, so everyone goes through this guard.
extern "C" bool mh_ensure_init()
{
    static volatile LONG state = 0;   // 0 = not done, 1 = in progress, 2 = ok, 3 = failed
    if (state == 2) return true;
    if (state == 3) return false;
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        MH_STATUS s = MH_Initialize();
        bool ok = (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED);
        if (!ok) NL("[MH] MH_Initialize failed (%d)", (int)s);
        InterlockedExchange(&state, ok ? 2 : 3);
    } else {
        while (state == 1) Sleep(0);
    }
    return state == 2;
}

static const unsigned int NVAPI_ID_D3D12_SetFlipConfig = 0xF3148C42u;
static const unsigned int NVAPI_ID_DRS_GetSetting       = 0x73BF8338u;   // NvAPI_DRS_GetSetting (nvapi_interface.h)

// ---- DLSS-G model preset via the driver profile --------------------------------------------------
// The frame-generation model preset (A/B/C...) has no Streamline or NGX API: NGX reads the NVIDIA App's
// per-game driver-profile override through NvAPI_DRS_GetSetting (also under Proton via dxvk-nvapi).
// We answer those two queries ourselves with the configured preset, so the user picks the model in
// fsr2dlss.ini / the overlay instead of the NVIDIA App. Setting ids and values: NvApiDriverSettings.h.
static const unsigned int DRS_NGX_DLSS_FG_OVERRIDE_ID                         = 0x10E41E03u;   // 0 off, 1 on
static const unsigned int DRS_NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID = 0x10E41DF1u;   // 0 off, 1 = A, 2 = B, 3 = C ...

#pragma pack(push, 8)
struct NvDrsBinarySetting { unsigned int valueLength; unsigned char valueData[4096]; };
struct NvDrsSetting {                           // NVDRS_SETTING_V1 (nvapi.h)
    unsigned int   version;
    unsigned short settingName[2048];
    unsigned int   settingId;
    unsigned int   settingType;                 // 0 = NVDRS_DWORD_TYPE
    unsigned int   settingLocation;             // 0 = NVDRS_CURRENT_PROFILE_LOCATION
    unsigned int   isCurrentPredefined;
    unsigned int   isPredefinedValid;
    union { unsigned int u32PredefinedValue; NvDrsBinarySetting binaryPredefinedValue; unsigned short wszPredefinedValue[2048]; unsigned long long u64PredefinedValue; };
    union { unsigned int u32CurrentValue;    NvDrsBinarySetting binaryCurrentValue;    unsigned short wszCurrentValue[2048];    unsigned long long u64CurrentValue; };
};
#pragma pack(pop)
static_assert(sizeof(NvDrsSetting) == 12328, "NVDRS_SETTING_V1 layout");
static const unsigned int NVDRS_SETTING_VER1 = (unsigned int)sizeof(NvDrsSetting) | (1u << 16);

typedef int (__cdecl* PFN_NvAPI_DRS_GetSetting)(void* hSession, void* hProfile, unsigned int settingId, NvDrsSetting* pSetting);
static PFN_NvAPI_DRS_GetSetting o_DRS_GetSetting = nullptr;

static int __cdecl hk_NvAPI_DRS_GetSetting(void* hSession, void* hProfile, unsigned int settingId, NvDrsSetting* pSetting)
{
    int r = o_DRS_GetSetting ? o_DRS_GetSetting(hSession, hProfile, settingId, pSetting) : -1;
    if (!pSetting) return r;
    int preset = g_cfg.fgPreset;
    if (preset <= 0) return r;   // default: leave the driver profile alone
    if (settingId != DRS_NGX_DLSS_FG_OVERRIDE_ID && settingId != DRS_NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID) return r;
    unsigned int value = (settingId == DRS_NGX_DLSS_FG_OVERRIDE_ID) ? 1u : (unsigned int)preset;
    if (r != 0) {   // not in the profile: synthesise the whole record
        memset(pSetting, 0, sizeof(*pSetting));
        pSetting->version = NVDRS_SETTING_VER1;
        pSetting->settingId = settingId;
        pSetting->settingType = 0;
        pSetting->settingLocation = 0;
        pSetting->isPredefinedValid = 1;
        pSetting->u32PredefinedValue = 0;
    }
    pSetting->isCurrentPredefined = 0;
    pSetting->u32CurrentValue = value;
    static int logged = 0;
    if (logged < 4) { ++logged; NL("[NVHOOK] DRS GetSetting(0x%08X) -> %u (DLSS-G preset %s, driver said %d)", settingId, value, config_fg_preset_name(preset), r); }
    return 0;   // NVAPI_OK
}

typedef void* (__cdecl* PFN_nvapi_QueryInterface)(unsigned int id);
static PFN_nvapi_QueryInterface o_nvapi_QI = nullptr;
static bool g_blockFlip = true;   // set from config (FlipMetering != hardware)

static void* __cdecl hk_nvapi_QueryInterface(unsigned int id)
{
    if (id == NVAPI_ID_D3D12_SetFlipConfig && g_blockFlip)
        return nullptr;   // hide SetFlipConfig -> DLSS-G uses the software pacer
    void* fn = o_nvapi_QI ? o_nvapi_QI(id) : nullptr;
    if (id == NVAPI_ID_DRS_GetSetting && fn) {
        // Hand out our detour; NGX core resolves NvAPI_DRS_GetSetting through this same export.
        if (!o_DRS_GetSetting) { o_DRS_GetSetting = (PFN_NvAPI_DRS_GetSetting)fn; NL("[NVHOOK] NvAPI_DRS_GetSetting@%p intercepted for the DLSS-G preset", fn); }
        return (void*)&hk_NvAPI_DRS_GetSetting;
    }
    return fn;
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

    if (!mh_ensure_init()) return false;

    MH_STATUS s = MH_CreateHook(qi, (void*)&hk_nvapi_QueryInterface, (void**)&o_nvapi_QI);
    if (s != MH_OK) { NL("[NVHOOK] MH_CreateHook failed (%d)", (int)s); return false; }

    s = MH_EnableHook(qi);
    if (s != MH_OK) { NL("[NVHOOK] MH_EnableHook failed (%d)", (int)s); return false; }

    NL("[NVHOOK] nvapi_QueryInterface@%p hooked (flipBlock=%d)", qi, (int)g_blockFlip);
    return true;
}
