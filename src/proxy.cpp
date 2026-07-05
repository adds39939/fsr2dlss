// ============================================================================
// FSR3.1 -> DLSS/DLSS-G bridge  ::  Phase 2 logging proxy
//
// Drop-in replacement for amd_fidelityfx_dx12.dll. Forwards the 5 unified
// FfxApi entry points to the real (renamed) DLL and logs every call: the
// desc 'type', the pNext chain, and a bounded hexdump of each desc. This
// empirically captures exactly how Lies of P drives FSR 3.1 (which effects,
// which desc types, struct layout) so the translation layer can be built
// against ground truth instead of guessed header versions.
// ============================================================================
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "slbridge.h"

// ---- minimal, version-stable FfxApi surface (header is ABI-frozen) ---------
typedef uint32_t ffxReturnCode_t;
typedef void*    ffxContext;
struct ffxApiHeader { uint64_t type; ffxApiHeader* pNext; };
struct ffxAllocationCallbacks { void* pUserData; void* alloc; void* dealloc; };

using PfnCreate    = ffxReturnCode_t (*)(ffxContext*, ffxApiHeader*, const ffxAllocationCallbacks*);
using PfnDestroy   = ffxReturnCode_t (*)(ffxContext*, const ffxAllocationCallbacks*);
using PfnConfigure = ffxReturnCode_t (*)(ffxContext*, const ffxApiHeader*);
using PfnQuery     = ffxReturnCode_t (*)(ffxContext*, ffxApiHeader*);
using PfnDispatch  = ffxReturnCode_t (*)(ffxContext*, const ffxApiHeader*);

static HINSTANCE  g_self  = nullptr;
static HMODULE    g_real  = nullptr;
static PfnCreate    r_create    = nullptr;
static PfnDestroy   r_destroy   = nullptr;
static PfnConfigure r_configure = nullptr;
static PfnQuery     r_query     = nullptr;
static PfnDispatch  r_dispatch  = nullptr;
static volatile LONG g_initState = 0; // 0=uninit 1=initializing 2=ready
static CRITICAL_SECTION g_loglock;
static bool g_lockInit = false;

static const wchar_t* REAL_DLL = L"amd_fidelityfx_dx12.amd.dll";
static const wchar_t* LOG_NAME = L"ffx_bridge.log";

// ---- logging ---------------------------------------------------------------
static void selfDir(wchar_t* out, size_t cch)
{
    GetModuleFileNameW(g_self, out, (DWORD)cch);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) slash[1] = 0;
}

static bool g_logEnabled = false;   // fsr2dlss.ini [Logging] Debug=true; off by default (no log file)

// Minimal INI read (proxy logs before slbridge's full config parser runs). Returns the boolean
// value of the given key in fsr2dlss.ini, or `def` if the key/file is absent.
static bool iniBool(const char* wantKey, bool def)
{
    wchar_t path[MAX_PATH]; selfDir(path, MAX_PATH);
    wcsncat(path, L"fsr2dlss.ini", MAX_PATH - wcslen(path) - 1);
    FILE* f = _wfopen(path, L"rb"); if (!f) return def;
    char line[256]; bool val = def;
    while (fgets(line, sizeof(line), f)) {
        char* c = strpbrk(line, ";#"); if (c) *c = 0;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char* key = line; while (*key == ' ' || *key == '\t') ++key;
        char* ke = key + strlen(key); while (ke > key && (ke[-1]==' '||ke[-1]=='\t'||ke[-1]=='\r'||ke[-1]=='\n')) *--ke = 0;
        if (_stricmp(key, wantKey) != 0) continue;
        char* v = eq + 1; while (*v == ' ' || *v == '\t') ++v;
        val = (!_strnicmp(v,"true",4) || !_strnicmp(v,"on",2) || !_strnicmp(v,"yes",3) || v[0]=='1');
    }
    fclose(f);
    return val;
}

static void logRaw(const char* s)
{
    if (!g_logEnabled) return;   // logging is opt-in (Debug=true)
    wchar_t path[MAX_PATH]; selfDir(path, MAX_PATH);
    wcsncat(path, LOG_NAME, MAX_PATH - wcslen(path) - 1);
    FILE* f = _wfopen(path, L"ab");
    if (!f) return;
    fwrite(s, 1, strlen(s), f);
    fclose(f);
}

static void logf(const char* fmt, ...)
{
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (g_lockInit) EnterCriticalSection(&g_loglock);
    logRaw(buf);
    if (g_lockInit) LeaveCriticalSection(&g_loglock);
}

// bounded, fault-safe hexdump: never read past the committed page of `p`.
static void hexdump(const void* p, size_t want)
{
    if (!p) { logf("        (null)\n"); return; }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) ||
        (mbi.State != MEM_COMMIT) ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    { logf("        (unreadable %p)\n", p); return; }
    // bytes remaining in this region from p
    size_t avail = (size_t)((char*)mbi.BaseAddress + mbi.RegionSize - (char*)p);
    size_t n = want < avail ? want : avail;
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i += 16) {
        char line[160]; int o = 0;
        o += snprintf(line+o, sizeof(line)-o, "        +%03zx: ", i);
        for (size_t j = 0; j < 16; ++j) {
            if (i+j < n) o += snprintf(line+o, sizeof(line)-o, "%02x ", b[i+j]);
            else          o += snprintf(line+o, sizeof(line)-o, "   ");
        }
        o += snprintf(line+o, sizeof(line)-o, " ");
        for (size_t j = 0; j < 16 && i+j < n; ++j) {
            unsigned char c = b[i+j];
            o += snprintf(line+o, sizeof(line)-o, "%c", (c>=32&&c<127)?c:'.');
        }
        o += snprintf(line+o, sizeof(line)-o, "\n");
        logRaw(line);
    }
}

static const char* effectName(uint64_t type)
{
    switch ((type & 0x00ff0000u)) {
        case 0x00010000u: return "UPSCALE";
        case 0x00020000u: return "FRAMEGEN";
        case 0x00030000u: return "FGSWAPCHAIN";
        case 0x00040000u: return "FGSWAPCHAIN_VK";
        case 0x00050000u: return "DENOISER";
        case 0x00000000u: return "GENERAL";
        default:          return "?";
    }
}

static void logChain(const char* api, const ffxApiHeader* h, size_t dumpSize)
{
    int depth = 0;
    logf("[%s] chain:\n", api);
    while (h && depth < 12) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(h, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
            logf("    #%d <unreadable %p>\n", depth, (void*)h); break;
        }
        logf("    #%d type=0x%016llx effect=%-12s pNext=%p\n",
             depth, (unsigned long long)h->type, effectName(h->type), (void*)h->pNext);
        hexdump(h, dumpSize);
        h = h->pNext;
        ++depth;
    }
}

// --- occurrence cap: log the first CAP instances of each (apiId,type) fully ---
#define LOG_CAP 3
struct OccEntry { int apiId; uint64_t type; unsigned count; };
static OccEntry g_occ[64];
static int g_occN = 0;
static unsigned bump(int apiId, uint64_t type)   // returns post-increment count
{
    for (int i = 0; i < g_occN; ++i)
        if (g_occ[i].apiId == apiId && g_occ[i].type == type) return ++g_occ[i].count;
    if (g_occN < 64) { g_occ[g_occN] = { apiId, type, 1 }; ++g_occN; return 1; }
    return LOG_CAP + 1;
}
static uint64_t peekType(const ffxApiHeader* h)
{
    if (!h) return 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(h, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
    return h->type;
}

// ---- Streamline routing (Phase 4) ------------------------------------------
// Logger handed to slbridge.cpp (it appends its own newline).
extern "C" void bridge_logs(const char* s)
{
    if (g_lockInit) EnterCriticalSection(&g_loglock);
    logRaw(s);
    if (g_lockInit) LeaveCriticalSection(&g_loglock);
}

// FfxApi create desc types we intercept.
static const uint64_t T_SWAPCHAIN_NEW   = 0x00030005;
static const uint64_t T_FRAMEGEN_CREATE = 0x00020001;

// The DX12 backend create desc (type 0x00000002) carries the ID3D12Device at +16.
static void* findDeviceInChain(const ffxApiHeader* h)
{
    int depth = 0;
    while (h && depth < 12) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(h, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        if (h->type == 0x00000002ull)
            return *(void* const*)((const char*)h + 16);
        h = h->pNext; ++depth;
    }
    return nullptr;
}

static bool readable(const void* p, size_t n)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (size_t)((char*)mbi.BaseAddress + mbi.RegionSize - (char*)p) >= n;
}
static void* readPtrAt(const void* base, size_t off)
{
    const void* p = (const char*)base + off;
    return readable(p, sizeof(void*)) ? *(void* const*)p : nullptr;
}
// The ffxContext handle the game holds = *context (context is ffxContext* = void**).
static void* derefCtx(ffxContext* context)
{
    return (context && readable(context, sizeof(void*))) ? *context : nullptr;
}

static volatile LONG g_slState = 0; // 0=untried 1=trying 2=ready 3=failed
static bool ensureSL(const ffxApiHeader* desc)
{
    if (g_slState == 2) return true;
    if (g_slState == 3) return false;
    void* dev = findDeviceInChain(desc);
    if (!dev) return false;
    if (InterlockedCompareExchange(&g_slState, 1, 0) != 0) {
        while (g_slState == 1) Sleep(0);
        return g_slState == 2;
    }
    wchar_t dir[MAX_PATH]; selfDir(dir, MAX_PATH);
    wcsncat(dir, L"streamline", MAX_PATH - wcslen(dir) - 1);
    bool ok = slbridge_init(dev, dir);
    InterlockedExchange(&g_slState, ok ? 2 : 3);
    return ok;
}

// ---- lazy init of the real DLL --------------------------------------------
static void doInit()
{
    if (!g_lockInit) { InitializeCriticalSection(&g_loglock); g_lockInit = true; }
    g_logEnabled = iniBool("Debug", false);   // opt-in file logging (fsr2dlss.ini [Logging] Debug=true)
    wchar_t path[MAX_PATH]; selfDir(path, MAX_PATH);
    wcsncat(path, REAL_DLL, MAX_PATH - wcslen(path) - 1);
    g_real = LoadLibraryW(path);
    logf("\n==================== ffx_bridge proxy init ====================\n");
    if (!g_real) {
        logf("FATAL: could not load real DLL '%ls' err=%lu\n", path, GetLastError());
        return;
    }
    r_create    = (PfnCreate)   GetProcAddress(g_real, "ffxCreateContext");
    r_destroy   = (PfnDestroy)  GetProcAddress(g_real, "ffxDestroyContext");
    r_configure = (PfnConfigure)GetProcAddress(g_real, "ffxConfigure");
    r_query     = (PfnQuery)    GetProcAddress(g_real, "ffxQuery");
    r_dispatch  = (PfnDispatch) GetProcAddress(g_real, "ffxDispatch");
    logf("loaded real DLL '%ls'  create=%p destroy=%p configure=%p query=%p dispatch=%p\n",
         path, (void*)r_create, (void*)r_destroy, (void*)r_configure, (void*)r_query, (void*)r_dispatch);
}
static void startUnhookThread();   // fwd decl (defined near DllMain)
static void ensure()
{
    startUnhookThread();   // idempotent; begins watching for Steam's ffxConfigure hook
    if (g_initState == 2) return;
    if (InterlockedCompareExchange(&g_initState, 1, 0) == 0) {
        doInit();
        InterlockedExchange(&g_initState, 2);
    } else {
        while (g_initState != 2) Sleep(0);
    }
}

// Big dumps so a single struct is fully captured (ffxDispatchDescUpscale ~0x1B0).
#define DUMP_SZ 0x200

// ---- exported wrappers -----------------------------------------------------
extern "C" {

__declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxApiHeader* desc, const ffxAllocationCallbacks* memCb)
{
    ensure();
    uint64_t t = peekType(desc);
    if (bump(0, t) <= LOG_CAP) {
        logf("\n>>> ffxCreateContext(context=%p, desc=%p, memCb=%p)\n", (void*)context, (void*)desc, (void*)memCb);
        if (desc) logChain("CreateContext", desc, DUMP_SZ);
    }

    // Initialise Streamline on the first context that carries a device.
    bool slReady = (t == T_SWAPCHAIN_NEW || t == T_FRAMEGEN_CREATE || t == 0x00010000)
                       ? ensureSL(desc) : (g_slState == 2);

    if (slReady && t == T_SWAPCHAIN_NEW) {
        void** outSc  = (void**)readPtrAt(desc, 0x10);
        void*  oldDsc =          readPtrAt(desc, 0x18);
        void*  fac    =          readPtrAt(desc, 0x20);
        void*  queue  =          readPtrAt(desc, 0x28);
        void*  ctx = slbridge_create_swapchain(oldDsc, fac, queue, outSc);
        if (ctx) { if (context) *context = ctx; return 0; }
        logf("[route] swapchain substitution failed; forwarding NEW_DX12 to AMD\n");
    }
    if (slReady && t == T_FRAMEGEN_CREATE) {
        void* ctx = slbridge_create_framegen(desc);
        if (ctx) { if (context) *context = ctx; return 0; }
    }
    return r_create ? r_create(context, desc, memCb) : 1;
}

__declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb)
{
    ensure();
    void* h = derefCtx(context);
    if (slbridge_is_mine(h)) { logf(">>> ffxDestroyContext(MINE %p)\n", h); slbridge_destroy(h); return 0; }
    return r_destroy ? r_destroy(context, memCb) : 1;
}

__declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxApiHeader* desc)
{
    ensure();
    void* h = derefCtx(context);
    if (slbridge_is_mine(h)) return slbridge_configure(h, desc);
    if (bump(2, peekType(desc)) <= LOG_CAP) {
        logf("\n>>> ffxConfigure(context=%p -> %p)\n", (void*)context, h);
        if (desc) logChain("Configure", desc, DUMP_SZ);
    }
    return r_configure ? r_configure(context, desc) : 1;
}

__declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext* context, ffxApiHeader* desc)
{
    ensure();
    void* h = derefCtx(context);
    if (slbridge_is_mine(h)) return slbridge_query(h, desc);
    bool full = bump(3, peekType(desc)) <= LOG_CAP;
    if (full) {
        logf("\n>>> ffxQuery(context=%p -> %p)\n", (void*)context, h);
        if (desc) logChain("Query", desc, DUMP_SZ);
    }
    ffxReturnCode_t rc = r_query ? r_query(context, desc) : 1;
    if (full && desc) { logf("    [post-query desc bytes] rc=%u\n", rc); hexdump(desc, DUMP_SZ); }
    return rc;
}

__declspec(dllexport) ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxApiHeader* desc)
{
    ensure();
    uint64_t dt = peekType(desc);
    // The UPSCALE dispatch (0x00010001) is the first FfxApi call of each frame. Handle it FIRST,
    // before is_mine routing, so it always runs through DLSS-SR even when the upscale context is
    // ours (stubbed). Open the injected Reflex frame, then try DLSS: if it runs, SKIP AMD's FSR;
    // if it declines, fall through (forwards to AMD when the context is real).
    if (dt == 0x00010001 && g_slState == 2) {
        slbridge_frame_start();
        if (slbridge_upscale(desc)) return 0;   // 0 == FFX_API_RETURN_OK; DLSS did the upscale
    }
    void* h = derefCtx(context);
    if (slbridge_is_mine(h)) return slbridge_dispatch(h, desc);
    if (bump(4, dt) <= LOG_CAP) {
        logf("\n>>> ffxDispatch(context=%p -> %p)\n", (void*)context, h);
        if (desc) logChain("Dispatch", desc, DUMP_SZ);
    }
    return r_dispatch ? r_dispatch(context, desc) : 1;
}

} // extern "C"

// ---- Un-hook Steam's FSR-FG detector ----------------------------------------------------------
// Steam's overlay (GameOverlayRenderer64.dll) inline-hooks our ffxConfigure export to detect
// "FSR frame generation" (confirmed verbatim in its strings: it hooks ffxConfigure on
// amd_fidelityfx_dx12.dll, and slGetNewFrameToken/slDLSSGSetOptions on Streamline for DLSS). When
// it sees both, FSR wins the label. We own this DLL, so restore our clean ffxConfigure prologue to
// remove Steam's hook -> it stops seeing the game's FG-configure -> the DLSS-G signal wins -> "DLSS"
// label, while native stays correct (one slGetNewFrameToken/frame via the shared SR token).
static unsigned char g_cfgClean[16];
static void*         g_cfgAddr  = nullptr;
static bool          g_cfgSaved = false;
// SteamOverlayFix (fsr2dlss.ini, default true): remove Steam's ffxConfigure FSR detector so the
// overlay reports DLSS (paired with slbridge's own-SR-frame-token to activate Steam's DLSS-G detector).
static bool steamFixOn()
{
    static int cached = -1;
    if (cached < 0) cached = iniBool("SteamOverlayFix", true) ? 1 : 0;
    return cached != 0;
}
static void restoreFfxConfigure()
{
    if (!g_cfgSaved || !g_cfgAddr) return;
    if (memcmp(g_cfgAddr, g_cfgClean, sizeof(g_cfgClean)) == 0) return;   // not (re)hooked
    DWORD old;
    if (VirtualProtect(g_cfgAddr, sizeof(g_cfgClean), PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(g_cfgAddr, g_cfgClean, sizeof(g_cfgClean));
        VirtualProtect(g_cfgAddr, sizeof(g_cfgClean), old, &old);
        FlushInstructionCache(GetCurrentProcess(), g_cfgAddr, sizeof(g_cfgClean));
        logf("[UNHOOK] removed Steam's inline hook on ffxConfigure (FSR-FG detection suppressed)\n");
    }
}
static DWORD WINAPI unhookThread(LPVOID)
{
    for (;;) { if (steamFixOn()) restoreFfxConfigure(); Sleep(100); }
}
static void startUnhookThread()
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0)
        CloseHandle(CreateThread(nullptr, 0, unhookThread, nullptr, 0, nullptr));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst; DisableThreadLibraryCalls(inst);
        // Save our clean ffxConfigure prologue NOW (before Steam gets a chance to hook it).
        g_cfgAddr = (void*)GetProcAddress(inst, "ffxConfigure");
        if (g_cfgAddr) { memcpy(g_cfgClean, g_cfgAddr, sizeof(g_cfgClean)); g_cfgSaved = true; }
    }
    return TRUE;
}
