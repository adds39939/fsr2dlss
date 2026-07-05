// nvhook.cpp — NvAPI interposer.
//   1) Optionally disable Blackwell HW flip metering so SL DLSS-G uses the software pacer
//      (return null for NvAPI_D3D12_SetFlipConfig id 0xF3148C42). Config-driven.
//   2) Diagnostics for the Steam-overlay work: log every nvapi_QueryInterface id resolved
//      (so we can see whether Streamline emits the async-frame / latency markers Steam reads).
// Ref: OptiScaler nvapi/NvApiHooks.cpp; NVIDIA nvapi_interface.h { "NvAPI_D3D12_SetFlipConfig", 0xf3148c42 }.
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <evntprov.h>   // ETW: EventRegister / EventWriteTransfer / EventSetInformation (advapi32)
#include <cstdio>
#include <cstdarg>
#include <cstring>
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
static bool g_blockFlip = true;   // set from config (FlipMetering != hardware)

// dedup: log each distinct queried id once
static unsigned g_ids[256]; static int g_nIds = 0;
static bool firstSee(unsigned id)
{
    for (int i = 0; i < g_nIds; ++i) if (g_ids[i] == id) return false;
    if (g_nIds < 256) g_ids[g_nIds++] = id;
    return true;
}

// ---- NvAPI frame-marker logging (understand what the Steam overlay reads) --------
// SetAsyncFrameMarker (0x13C98F73) + SetLatencyMarker (0xD9984C05) both take a params
// struct { NvU32 version@+0; NvU64 frameID@+8; NV_LATENCY_MARKER_TYPE markerType@+16; }.
// Steam counts "native" frames from these (unique frameIDs / SIMULATION_START rate) and
// detects DLSS-G frame gen from OUT_OF_BAND_PRESENT markers. Log the stream to see it.
static const unsigned NVAPI_ID_SetAsyncFrameMarker = 0x13C98F73u;
static const unsigned NVAPI_ID_SetLatencyMarker    = 0xD9984C05u;
typedef long (__cdecl* PFN_marker)(void* handle, void* params);
static PFN_marker o_setAsyncMarker   = nullptr;
static PFN_marker o_setLatencyMarker = nullptr;
static const char* markerName(unsigned t) {
    static const char* n[] = {"SIM_START","SIM_END","RENDER_START","RENDER_END","PRESENT_START","PRESENT_END",
                              "INPUT","FLASH","PING","OOB_RENDER_START","OOB_RENDER_END","OOB_PRESENT_START","OOB_PRESENT_END"};
    return (t < 13) ? n[t] : "?";
}
static volatile LONG g_markCount[16] = {0};
static volatile LONG g_markTotal = 0;
static unsigned long long g_lastFID[16] = {0};
static void logMarker(const char* src, void* p) {
    if (!p) return;
    unsigned long long fid = *(unsigned long long*)((char*)p + 8);
    unsigned mt = *(unsigned*)((char*)p + 16);
    if (mt < 16) { InterlockedIncrement(&g_markCount[mt]); g_lastFID[mt] = fid; }
    long n = InterlockedIncrement(&g_markTotal);
    if (n <= 180)                        // first frames: verbatim per-frame sequence
        NL("[MARK] %-7s %-16s frameID=%llu", src, markerName(mt), fid);
    else if ((n % 600) == 0)             // periodic rate summary
        NL("[MARK] tot=%ld cnt SIM_START=%ld RENDER_START=%ld PRESENT_START=%ld OOB_PRESENT_START=%ld OOB_PRESENT_END=%ld inj=%ld | lastFID sim=%llu pres=%llu oobPres=%llu",
           n, g_markCount[0], g_markCount[2], g_markCount[4], g_markCount[11], g_markCount[12], (long)g_markCount[13],
           g_lastFID[0], g_lastFID[4], g_lastFID[11]);
}

// ---- PCLSTATS ETW emission (Nvidia_PCL provider) — the channel Steam likely actually reads ----
// Steam ignored our in-process NvAPI markers, so mirror the OOB-present markers onto the ETW
// provider Streamline's sl.pcl owns: "PCLStatsTraceLoggingProvider" {0d216f06-...}, TraceLogging
// event "PCLStatsEvent" { UInt32 "Marker"; UInt64 "FrameID"; }. Hand-encoded TraceLogging blobs
// (format from Microsoft TraceLoggingDynamic.h): provider meta = u16 size + name\0; event meta =
// u16 size + tags(0) + name\0 + per-field(name\0 + InType); InTypeUInt32=8, InTypeUInt64=10.
static const GUID g_pclGuid = {0x0d216f06,0x82a6,0x4d49,{0xbc,0x4f,0x8f,0x38,0xae,0x56,0xef,0xab}};
static const unsigned char g_pclProvMeta[] = {                         // size=31
    31,0, 'P','C','L','S','t','a','t','s','T','r','a','c','e','L','o','g','g','i','n','g','P','r','o','v','i','d','e','r',0 };
static const unsigned char g_pclEvtMeta[] = {                          // size=34
    34,0, 0, 'P','C','L','S','t','a','t','s','E','v','e','n','t',0,
    'M','a','r','k','e','r',0, 8, 'F','r','a','m','e','I','D',0, 10 };
static REGHANDLE g_pclReg = 0;
static void pclInit() {
    if (g_pclReg) return;
    if (EventRegister(&g_pclGuid, nullptr, nullptr, &g_pclReg) != ERROR_SUCCESS) { g_pclReg = 0; return; }
    EventSetInformation(g_pclReg, (EVENT_INFO_CLASS)2 /*EventProviderSetTraits*/,
                        (void*)g_pclProvMeta, (ULONG)sizeof(g_pclProvMeta));
    NL("[PCL] registered PCLStatsTraceLoggingProvider (ETW) reg=%llu", (unsigned long long)g_pclReg);
}
static void pclMarker(unsigned marker, unsigned long long frameID) {
    if (!g_pclReg) return;
    EVENT_DESCRIPTOR desc; memset(&desc, 0, sizeof(desc));
    desc.Channel = 11;  // WINEVENT_CHANNEL_TRACELOGGING
    desc.Level   = 5;   // WINEVENT_LEVEL_VERBOSE (TraceLoggingWrite default)
    UINT32 m = marker; UINT64 f = frameID;
    EVENT_DATA_DESCRIPTOR d[4];
    d[0].Ptr = (ULONGLONG)(ULONG_PTR)g_pclProvMeta; d[0].Size = (ULONG)sizeof(g_pclProvMeta); d[0].Reserved = 2; // provider meta
    d[1].Ptr = (ULONGLONG)(ULONG_PTR)g_pclEvtMeta;  d[1].Size = (ULONG)sizeof(g_pclEvtMeta);  d[1].Reserved = 1; // event meta
    d[2].Ptr = (ULONGLONG)(ULONG_PTR)&m; d[2].Size = 4; d[2].Reserved = 0;
    d[3].Ptr = (ULONGLONG)(ULONG_PTR)&f; d[3].Size = 8; d[3].Reserved = 0;
    EventWriteTransfer(g_pclReg, &desc, nullptr, nullptr, 4, d);
}

// ---- OOB-present marker injection (restore the DLSS-G signature under HW flip metering) -----
// On Blackwell hardware flip metering the generated frames bypass Streamline's software pacer,
// so SL emits <1 OUT_OF_BAND_PRESENT marker/real-frame instead of one per presented frame. Steam
// reads that stream to identify NVIDIA frame-gen + split native/total, so it mis-labels ("FSR")
// and mis-counts native. These markers are pure latency metadata (NOT the present itself), so we
// synthesize the missing ones: per real frame, emit g_oobInject OUT_OF_BAND_PRESENT START/END pairs
// reusing the REAL frameID, on SL's registered OOB-present queue. Genuine OOB markers are dropped so
// the per-frame count is deterministic. Does NOT touch the actual flip metering (true 3x preserved).
static const unsigned MK_PRESENT_START = 4u, MK_OOB_PRESENT_START = 11u, MK_OOB_PRESENT_END = 12u;
static volatile LONG   g_oobInject   = 0;          // OOB pairs to emit per real frame (0 = off)
static void*           g_oobQueue    = nullptr;    // SL's out-of-band present command queue
static unsigned        g_oobVersion  = 0x00010058; // NV_ASYNC_FRAME_MARKER_PARAMS_VER1 (fallback)
static unsigned long long g_oobPresentFID = 0;     // synthetic presentFrameID, increments per emit
static unsigned long long g_lastEmitFrame = ~0ull; // dedup: emit once per real frameID
extern "C" void nvhook_set_oob_inject(int n) { g_oobInject = (n > 0 && n <= 8) ? n : 0; if (g_oobInject) pclInit(); }

// Resolve SetAsyncFrameMarker through the LIVE nvapi_QueryInterface (the export address = top of
// the inline-hook chain, where Steam's overlay sits). o_setAsyncMarker is our MinHook trampoline =
// the CLEAN original, which jumps past every hook, so calls on it are invisible to Steam. To make
// Steam see our injected markers we must call the pointer the live (Steam-hooked) QI returns.
static long __cdecl hk_setAsyncMarker(void* q, void* p);  // fwd decl (defined below)
static PFN_marker g_steamMarker = nullptr;
static bool       g_steamMarkerTried = false;
static void resolveSteamMarker() {
    g_steamMarkerTried = true;
    HMODULE nv = GetModuleHandleW(L"nvapi64.dll");
    if (!nv) return;
    typedef void* (__cdecl* PFN_QI)(unsigned);
    PFN_QI liveQI = (PFN_QI)GetProcAddress(nv, "nvapi_QueryInterface");
    if (!liveQI) return;
    g_steamMarker = (PFN_marker)liveQI(NVAPI_ID_SetAsyncFrameMarker);  // runs Steam's QI hook -> Steam-wrapped ptr
    NL("[NVHOOK] marker ptrs: steamVisible=%p ourThunk=%p cleanOrig=%p", (void*)g_steamMarker, (void*)&hk_setAsyncMarker, (void*)o_setAsyncMarker);
}

static void emitOOBRun(unsigned long long fid) {
    if (!g_steamMarkerTried) resolveSteamMarker();
    // Prefer the Steam-visible pointer (live hook chain); it may equal our own thunk if Steam wraps
    // via QI (then our thunk re-enters and drops harmlessly) — still routes through Steam's wrapper.
    PFN_marker mk = g_steamMarker ? g_steamMarker : o_setAsyncMarker;
    for (int i = 0; i < (int)g_oobInject; ++i) {
        pclMarker(MK_OOB_PRESENT_START, fid);   // ETW belt-and-suspenders (Steam doesn't subscribe, harmless)
        pclMarker(MK_OOB_PRESENT_END,   fid);
        if (mk && g_oobQueue) {
            unsigned char buf[96]; memset(buf, 0, sizeof(buf));
            *(unsigned*)          (buf + 0)  = g_oobVersion;
            *(unsigned long long*)(buf + 8)  = fid;
            *(unsigned long long*)(buf + 24) = ++g_oobPresentFID;
            buf[32] = 1;                                          // vendorInternal = true
            *(unsigned*)(buf + 16) = MK_OOB_PRESENT_START; mk(g_oobQueue, buf);
            *(unsigned*)(buf + 16) = MK_OOB_PRESENT_END;   mk(g_oobQueue, buf);
        }
        InterlockedIncrement(&g_markCount[13]);
    }
}

static long __cdecl hk_setAsyncMarker(void* q, void* p) {
    logMarker("async", p);
    if (g_oobInject > 0 && p) {
        unsigned mt = *(unsigned*)((char*)p + 16);
        unsigned long long fid = *(unsigned long long*)((char*)p + 8);
        if (mt == MK_OOB_PRESENT_START) { g_oobQueue = q; g_oobVersion = *(unsigned*)p; return 0; } // capture + drop genuine
        if (mt == MK_OOB_PRESENT_END)   { return 0; }                                                // drop genuine
        long r = o_setAsyncMarker ? o_setAsyncMarker(q, p) : 0;
        // Trigger once per real frame (dedup by frameID) — ETW path doesn't need the NvAPI queue.
        if (mt == MK_PRESENT_START && fid != g_lastEmitFrame) { g_lastEmitFrame = fid; emitOOBRun(fid); }
        return r;
    }
    return o_setAsyncMarker ? o_setAsyncMarker(q, p) : 0;
}
static long __cdecl hk_setLatencyMarker(void* d, void* p) { logMarker("latency", p); return o_setLatencyMarker ? o_setLatencyMarker(d, p) : 0; }

static void* __cdecl hk_nvapi_QueryInterface(unsigned int id)
{
    if (id == NVAPI_ID_D3D12_SetFlipConfig && g_blockFlip) {
        if (firstSee(id)) NL("[NVHOOK] QI 0x%08X = SetFlipConfig -> BLOCKED (software pacing)", id);
        InterlockedIncrement(&g_blockedCount);
        return nullptr;
    }
    void* r = o_nvapi_QI ? o_nvapi_QI(id) : nullptr;
    // Wrap the frame-marker functions with logging thunks (see what Steam reads).
    if (id == NVAPI_ID_SetAsyncFrameMarker && r) {
        o_setAsyncMarker = (PFN_marker)r;
        if (firstSee(id)) NL("[NVHOOK] QI 0x%08X = SetAsyncFrameMarker -> %p (logging)", id, r);
        return (void*)&hk_setAsyncMarker;
    }
    if (id == NVAPI_ID_SetLatencyMarker && r) {
        o_setLatencyMarker = (PFN_marker)r;
        if (firstSee(id)) NL("[NVHOOK] QI 0x%08X = SetLatencyMarker -> %p (logging)", id, r);
        return (void*)&hk_setLatencyMarker;
    }
    if (firstSee(id)) NL("[NVHOOK] QI 0x%08X -> %p", id, (void*)r);
    return r;
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
