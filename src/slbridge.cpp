// ============================================================================
// Streamline bridge — Phase 4.
//   Milestone B: substitute the AMD FSR3 frame-generation swapchain with a
//   Streamline DLSS-G-capable swapchain, intercept the FG context + the
//   per-frame interpolation queries, keep the game alive. DLSS-G mode is OFF
//   here (passthrough) to validate the substitution in isolation. Milestone C
//   adds the per-frame depth/MV/HUDless tagging, constants and Reflex and flips
//   DLSS-G on.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cctype>

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"
#include "slbridge.h"

extern "C" void bridge_logs(const char* s);
extern "C" bool nvhook_install();             // nvhook.cpp — hook nvapi_QueryInterface (markers + flip block)
extern "C" void nvhook_set_flip_block(bool);  // block SetFlipConfig (software pacing) or not
extern "C" void nvhook_set_oob_inject(int);   // synthesize N OUT_OF_BAND_PRESENT pairs/frame (Steam DLSS-G signature); 0=off
static void L(const char* fmt, ...)
{
    char buf[1400];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n] = '\n'; buf[n + 1] = 0;
    bridge_logs(buf);
}

// ---- mod directory + fsr2dlss.ini config ------------------------------------
// All runtime files (fsr2dlss.ini, toggles) live next to this DLL, resolved from the DLL path
// so there are no hardcoded absolute paths.
static wchar_t g_modDir[MAX_PATH] = L"";
static void computeModDir()
{
    if (g_modDir[0]) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&computeModDir, &self);
    wchar_t path[MAX_PATH] = L"";
    if (self) GetModuleFileNameW(self, path, MAX_PATH);
    wcsncpy(g_modDir, path, MAX_PATH - 1);
    wchar_t* slash = wcsrchr(g_modDir, L'\\');
    if (slash) slash[1] = 0; else g_modDir[0] = 0;
}
// Not reentrant — use the returned pointer immediately.
static const wchar_t* modPath(const wchar_t* name)
{
    static wchar_t buf[MAX_PATH];
    computeModDir();
    _snwprintf(buf, MAX_PATH, L"%s%s", g_modDir, name);
    return buf;
}
// Current primary-display refresh rate in Hz (for the MFG base frame cap = refresh / multiplier).
static uint32_t getRefreshHz()
{
    DEVMODEW dm; memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return dm.dmDisplayFrequency;
    return 0;
}

struct Config {
    int  fgMultiplier = 2;      // 2/3/4 -> numFramesToGenerate = mult-1 (clamped to card max)
    int  flipMode     = 0;      // 0=software (block SetFlipConfig) | 1=hardware | 2=auto
    bool dlssSR       = true;   // false -> pass upscale through to FSR
    bool hudlessTags  = false;  // best-effort HUDLessColor/UI tags (game bakes UI, so experimental)
    char hostVer[16]  = "2.7.1";// SL host SDK version reported to slInit
    int  fpsCap       = 0;      // (legacy, unused for auto-cap) manual Reflex cap via fg_fpscap.txt still works
    int  dlssgStruct  = 0;      // DLSSGOptions structVersion: 0=auto(by host ver) | 1/3/5 = force
    bool loaded       = false;
};
static Config g_cfg;

static char* trimStr(char* s)
{
    while (*s==' '||*s=='\t'||*s=='\r'||*s=='\n') ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e = 0;
    return s;
}
static bool cfgBool(const char* v) { return !_stricmp(v,"true")||!strcmp(v,"1")||!_stricmp(v,"on")||!_stricmp(v,"yes"); }

static void loadConfig()
{
    if (g_cfg.loaded) return;
    g_cfg.loaded = true;
    // Legacy: sl_hostver.txt still honored if present (INI overrides it).
    { FILE* vf = _wfopen(modPath(L"sl_hostver.txt"), L"rb");
      if (vf) { char b[16]={0}; fread(b,1,sizeof(b)-1,vf); fclose(vf); char* t=trimStr(b); if (*t) { strncpy(g_cfg.hostVer,t,sizeof(g_cfg.hostVer)-1); g_cfg.hostVer[sizeof(g_cfg.hostVer)-1]=0; } } }
    FILE* f = _wfopen(modPath(L"fsr2dlss.ini"), L"rb");
    if (!f) { L("[CFG] no fsr2dlss.ini next to the mod; using defaults (host=%s)", g_cfg.hostVer); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* c = strpbrk(line, ";#"); if (c) *c = 0;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char* key = trimStr(line);
        char* val = trimStr(eq + 1);
        for (char* p = key; *p; ++p) *p = (char)tolower((unsigned char)*p);
        if      (!strcmp(key,"multiplier"))   { int m=atoi(val); if (m>=2 && m<=4) g_cfg.fgMultiplier=m; }
        else if (!strcmp(key,"flipmetering"))   g_cfg.flipMode = !_stricmp(val,"hardware")?1 : (!_stricmp(val,"auto")?2:0);
        else if (!strcmp(key,"dlss"))           g_cfg.dlssSR = cfgBool(val);
        else if (!strcmp(key,"hudlesstags"))    g_cfg.hudlessTags = cfgBool(val);
        else if (!strcmp(key,"hostversion"))  { strncpy(g_cfg.hostVer,val,sizeof(g_cfg.hostVer)-1); g_cfg.hostVer[sizeof(g_cfg.hostVer)-1]=0; }
        else if (!strcmp(key,"fpscap"))       { if (!_stricmp(val,"off")) g_cfg.fpsCap=-1; else if (!_stricmp(val,"auto")) g_cfg.fpsCap=0; else g_cfg.fpsCap=atoi(val); }
        else if (!strcmp(key,"dlssgstruct"))  { if (!_stricmp(val,"auto")) g_cfg.dlssgStruct=0; else { int v=atoi(val); if (v==1||v==3||v==5) g_cfg.dlssgStruct=v; } }
    }
    fclose(f);
    L("[CFG] fsr2dlss.ini: multiplier=%dx flipMetering=%d dlssSR=%d hudlessTags=%d dlssgStruct=%d host=%s",
      g_cfg.fgMultiplier, g_cfg.flipMode, (int)g_cfg.dlssSR, (int)g_cfg.hudlessTags, g_cfg.dlssgStruct, g_cfg.hostVer);
}

// ---- minimal FfxApi types we parse (layouts verified from runtime) ----------
struct FfxHeader { uint64_t type; FfxHeader* pNext; };
struct FfxResDesc { uint32_t type, format, width, height, depth, mipCount, flags, usage; };
struct FfxResource { void* resource; FfxResDesc description; uint32_t state; };
struct FfxQInterpCmdList { FfxHeader header; void** pOutCommandList; };
struct FfxQInterpTexture { FfxHeader header; FfxResource* pOutTexture; };

static const uint64_t T_SWAPCHAIN_NEW   = 0x00030005;
static const uint64_t T_SWAPCHAIN_QCMD  = 0x00030003;
static const uint64_t T_SWAPCHAIN_QTEX  = 0x00030004;
static const uint64_t T_FRAMEGEN_CREATE = 0x00020001;

// ---- SL function pointers ---------------------------------------------------
static PFun_slInit*                   p_slInit;
static PFun_slShutdown*               p_slShutdown;
static PFun_slSetD3DDevice*           p_slSetD3DDevice;
static PFun_slUpgradeInterface*       p_slUpgradeInterface;
static PFun_slIsFeatureSupported*     p_slIsFeatureSupported;
static PFun_slGetFeatureRequirements* p_slGetFeatureRequirements;
static PFun_slGetFeatureFunction*     p_slGetFeatureFunction;
static PFun_slSetTag*                 p_slSetTag;
static PFun_slSetConstants*           p_slSetConstants;
static PFun_slGetNewFrameToken*       p_slGetNewFrameToken;
static PFun_slSetFeatureLoaded*       p_slSetFeatureLoaded;
// feature funcs
static PFun_slDLSSGSetOptions*        p_slDLSSGSetOptions;
static PFun_slDLSSGGetState*          p_slDLSSGGetState;
static PFun_slReflexSetMarker*        p_slReflexSetMarker;   // 2.2 only (deprecated in 2.11)
static PFun_slReflexSetOptions*       p_slReflexSetOptions;
static PFun_slReflexSleep*            p_slReflexSleep;
// DLSS super-resolution (replaces FSR upscale)
static PFun_slEvaluateFeature*        p_slEvaluateFeature = nullptr;
static PFun_slDLSSSetOptions*         p_slDLSSSetOptions  = nullptr;
static uint32_t g_srFrameIdx  = 0x40000000; // SR frame-token space, separate from DLSS-G's
static uint32_t g_srLastOutW  = 0, g_srLastOutH = 0; static int g_srLastMode = -1;
static bool     g_srDisabled  = false;

// 2.11+: per-frame latency markers live in sl.pcl (slReflexSetMarker is gone).
static const sl::Feature kFeaturePCL = 4;
typedef sl::Result (*PFnPCLSetMarker)(uint32_t marker, const sl::FrameToken& frame);
static PFnPCLSetMarker p_slPCLSetMarker = nullptr;

// 2.11+: slSetTag is deprecated; tags must be bound to a frame token via
// slSetTagForFrame (+ PreferenceFlags::eUseFrameBasedResourceTagging = 1<<7),
// otherwise they "hang" and get invalidated before Present -> DLSS-G can't generate.
typedef sl::Result (*PFnSetTagForFrame)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, void*);
static PFnSetTagForFrame p_slSetTagForFrame = nullptr;
static const unsigned long long SL_FLAG_FRAME_TAGGING = (1ull << 7);
enum { PCL_SimStart = 0, PCL_SimEnd = 1, PCL_RenderStart = 2, PCL_RenderEnd = 3, PCL_PresentStart = 4, PCL_PresentEnd = 5 };
static sl::FrameToken* g_curToken = nullptr;   // current frame token, shared with the Present wrapper
static volatile LONG  g_presentMarks = 0;      // # present-marker pairs emitted by the wrapper
static unsigned long  g_presentTid   = 0;      // thread that calls Present (should == feedTid)
static bool           g_wrapperLive   = false; // has the game ever called our wrapper's Present?

static bool          g_slReady   = false;
static bool          g_slFrameTag = true;   // frame-based tagging: slSetTagForFrame + eUseFrameBasedResourceTagging (2.7.30+ only)
static uint32_t      g_dlssgOptVer = 1;     // DLSSGOptions structVersion to send (decoupled from tagging; see slInit)
static ID3D12Device* g_device    = nullptr;
static const char* resStr(sl::Result r);

// ---- per-frame feeding state (Milestone C) ----------------------------------
#include <cmath>
struct FfxResourceLite { void* res; uint32_t format, width, height, state; };
static FfxResourceLite g_hudless = {};   // from FRAMEGEN configure 0x00020002
struct BridgeCtx;                        // fwd
static BridgeCtx* g_scCtx = nullptr;     // the active SL swapchain context
static bool     g_fgEnabled = false;
static uint64_t g_frameID   = 0;
static bool     g_reflexSet = false;
static uint32_t g_dlssgMode = 0;         // 0=off 1=on (last requested)
static unsigned g_frameCount = 0;
static bool     g_diagShowInterpOnly = false; // DIAGNOSTIC: present only generated frames
static uint32_t g_maxPresented   = 0;         // max numFramesActuallyPresented ever seen
static uint32_t g_doubledFrames  = 0;         // # frames where presented >= 2
static int      g_lastAppliedMode = -1;       // last DLSSGMode applied (-1 = never)
static uint32_t g_frameIdx       = 0;         // monotonic SL frame index (one token per frame)
static uint32_t g_curFrameID     = 0xFFFFFFFF; // game's frameID for the current SL token (OptiScaler model)
static bool     g_nativeToken    = false;      // sr_native_token.flag: open the frame token at UPSCALE so DLSS-SR + DLSS-G share it (native pattern)
static bool     g_frameTokenReady = false;     // frame_start opened the token for this frame; feedDLSSG reuses it
static unsigned g_presentCount   = 0;         // presents seen (warmup gate for the Reflex sleep)
static const unsigned WARMUP_PRESENTS = 120;  // skip the throttling sleep during fragile SL/swapchain init
static bool     g_lastAppliedInterp = false;  // last interp-only flag applied
// Dev toggles (no rebuild), all resolved next to the mod DLL: fg_interp.flag (show only generated
// frames), fg_fpscap.txt (target FPS -> Reflex frame cap).
static uint32_t g_frameLimitUs = 0;           // Reflex frame cap in microseconds (0 = uncapped)
static uint32_t readFpsCapUs()
{
    FILE* f = _wfopen(modPath(L"fg_fpscap.txt"), L"rb");
    if (!f) return 0;
    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!n) return 0;
    int fps = atoi(buf);
    return (fps > 0 && fps < 1000) ? (uint32_t)(1000000 / fps) : 0;
}
static bool     g_csSet = false;              // swapchain color space applied yet?
static unsigned long g_feedTid = 0;           // thread that runs feedDLSSG (sets g_curToken)
static bool          g_lastHudTagged = false; // did we tag HUDLessColor this frame?

// FfxApiResourceState bitmask -> D3D12_RESOURCE_STATES (best-effort).
static uint32_t ffxStateToD3D12(uint32_t s)
{
    uint32_t d = 0;
    if (s & (1u<<0)) d |= D3D12_RESOURCE_STATE_COMMON;
    if (s & (1u<<1)) d |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & (1u<<2)) d |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s & (1u<<3)) d |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (s & (1u<<4)) d |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s & (1u<<5)) d |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (s & (1u<<7)) d |= D3D12_RESOURCE_STATE_PRESENT;
    if (s & (1u<<8)) d |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (s & (1u<<9)) d |= D3D12_RESOURCE_STATE_DEPTH_READ;
    return d ? d : D3D12_RESOURCE_STATE_COMMON;
}

static sl::Resource makeSlTex(void* native, uint32_t ffxState, uint32_t w, uint32_t h, uint32_t /*ffxFmt*/)
{
    sl::Resource r(sl::ResourceType::eTex2d, native, ffxStateToD3D12(ffxState));
    // nativeFormat left 0: SL queries the real DXGI format from the ID3D12Resource.
    r.width = w; r.height = h; r.mipLevels = 1; r.arrayLayers = 1;
    return r;
}

// Reversed-Z infinite-far perspective (row-major, row-vector); depthInverted=true.
static sl::float4x4 buildProjRH(float fovY, float aspect, float zNear)
{
    float f = 1.0f / tanf(fovY * 0.5f);
    sl::float4x4 m;
    m.setRow(0, sl::float4(f / aspect, 0, 0, 0));
    m.setRow(1, sl::float4(0, f, 0, 0));
    m.setRow(2, sl::float4(0, 0, 0, -1));
    m.setRow(3, sl::float4(0, 0, zNear, 0));
    return m;
}
static sl::float4x4 ident4()
{
    sl::float4x4 m;
    m.setRow(0, sl::float4(1,0,0,0)); m.setRow(1, sl::float4(0,1,0,0));
    m.setRow(2, sl::float4(0,0,1,0)); m.setRow(3, sl::float4(0,0,0,1));
    return m;
}

// ---- bridge contexts --------------------------------------------------------
enum CtxKind { CTX_SWAPCHAIN = 1, CTX_FRAMEGEN = 2, CTX_UPSCALE = 3 };
struct BridgeCtx {
    uint32_t magic;            // 'FBRG'
    CtxKind  kind;
    // swapchain
    IDXGISwapChain4*           sc4       = nullptr;
    ID3D12CommandAllocator*    dummyAlloc= nullptr;
    ID3D12GraphicsCommandList* dummyList = nullptr;
    bool                       listOpen  = false;
    ID3D12Resource*            dummyTex  = nullptr;
    ID3D12Resource*            uiTex     = nullptr;   // transparent UIColorAndAlpha (R8G8B8A8)
    uint32_t                   dispW = 0, dispH = 0;
    FfxResDesc                 texDesc{};
    // framegen
    bool fgEnabled = false;
};
static const uint32_t CTX_MAGIC = 0x47524246; // 'FBRG'

static BridgeCtx* g_ctxs[16];
static int        g_nctx = 0;
static CRITICAL_SECTION g_csCtx; static bool g_csInit = false;
static void ctxLock()   { if (!g_csInit) { InitializeCriticalSection(&g_csCtx); g_csInit = true; } EnterCriticalSection(&g_csCtx); }
static void ctxUnlock() { LeaveCriticalSection(&g_csCtx); }

static void registerCtx(BridgeCtx* c) { ctxLock(); if (g_nctx < 16) g_ctxs[g_nctx++] = c; ctxUnlock(); }
static void unregisterCtx(BridgeCtx* c) {
    ctxLock();
    for (int i = 0; i < g_nctx; ++i) if (g_ctxs[i] == c) { g_ctxs[i] = g_ctxs[--g_nctx]; break; }
    ctxUnlock();
}

extern "C" bool slbridge_is_mine(void* handle)
{
    if (!handle) return false;
    ctxLock();
    bool found = false;
    for (int i = 0; i < g_nctx; ++i) if (g_ctxs[i] == handle) { found = true; break; }
    ctxUnlock();
    return found && ((BridgeCtx*)handle)->magic == CTX_MAGIC;
}

// ---- init -------------------------------------------------------------------
static void* GP(HMODULE h, const char* n) { return (void*)GetProcAddress(h, n); }

extern "C" bool slbridge_init(void* deviceV, const wchar_t* slDir)
{
    if (g_slReady) return true;
    g_device = (ID3D12Device*)deviceV;
    loadConfig();
    L("[SL] init begin dir=%ls device=%p", slDir, deviceV);

    // Flip metering: by default (software / auto) block Blackwell HW flip metering BEFORE slInit so
    // DLSS-G uses the software pacer, which actually presents the interpolated frame under injection
    // (HW flip metering drops it with our late-hook cadence). Independent Flip is unaffected.
    // FlipMetering=hardware skips the block to try HW metering (experimental; currently doesn't
    // insert frames under injection on Blackwell — see docs). See nvhook.cpp.
    nvhook_set_flip_block(g_cfg.flipMode != 1);   // software/auto -> block; hardware -> don't
    nvhook_install();                             // always hook nvapi (needed for Steam marker work too)
    if (g_cfg.flipMode == 1) L("[SL] FlipMetering=hardware -> NOT blocking SetFlipConfig (experimental under injection)");

    wchar_t interp[MAX_PATH];
    _snwprintf(interp, MAX_PATH, L"%s\\sl.interposer.dll", slDir);
    HMODULE h = LoadLibraryW(interp);
    if (!h) { L("[SL] LoadLibrary FAILED err=%lu", GetLastError()); return false; }

    p_slInit                   = (PFun_slInit*)                  GP(h, "slInit");
    p_slShutdown               = (PFun_slShutdown*)              GP(h, "slShutdown");
    p_slSetD3DDevice           = (PFun_slSetD3DDevice*)          GP(h, "slSetD3DDevice");
    p_slUpgradeInterface       = (PFun_slUpgradeInterface*)      GP(h, "slUpgradeInterface");
    p_slIsFeatureSupported     = (PFun_slIsFeatureSupported*)    GP(h, "slIsFeatureSupported");
    p_slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GP(h, "slGetFeatureRequirements");
    p_slGetFeatureFunction     = (PFun_slGetFeatureFunction*)    GP(h, "slGetFeatureFunction");
    p_slSetTag                 = (PFun_slSetTag*)                GP(h, "slSetTag");
    p_slSetTagForFrame         = (PFnSetTagForFrame)            GP(h, "slSetTagForFrame");
    p_slSetConstants           = (PFun_slSetConstants*)          GP(h, "slSetConstants");
    p_slGetNewFrameToken       = (PFun_slGetNewFrameToken*)      GP(h, "slGetNewFrameToken");
    p_slSetFeatureLoaded       = (PFun_slSetFeatureLoaded*)      GP(h, "slSetFeatureLoaded");
    p_slEvaluateFeature        = (PFun_slEvaluateFeature*)       GP(h, "slEvaluateFeature");
    if (!p_slInit || !p_slSetD3DDevice || !p_slUpgradeInterface) { L("[SL] missing core exports"); return false; }

    static const sl::Feature feats[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, kFeaturePCL };
    sl::Preferences pref{};
    pref.logLevel          = sl::LogLevel::eVerbose;
    pref.pathsToPlugins    = &slDir;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = slDir;
    pref.featuresToLoad    = feats;
    pref.numFeaturesToLoad = 4;
    // Host SDK version to report, from fsr2dlss.ini (g_cfg.hostVer, "major.minor.patch", default 2.7.1)
    // so we can swap Streamline plugin sets WITHOUT rebuilding. TWO independent thresholds:
    //  - frame-based tagging (slSetTagForFrame + eUseFrameBasedResourceTagging): added 2.7.30. Older
    //    plugins (2.2-2.7.2x) use the global slSetTag software-pacer path that engages under injection.
    //  - DLSSGOptions structVersion: v5 (numFramesToGenerate MFG + queueParallelismMode) matters from
    //    2.7.2+ (where HARDWARE flip metering exists). 2.7.1 stays on v1 (its known-good 2x path).
    //    INI DlssgStruct=1/3/5 forces it (escape hatch if a plugin rejects v5).
    uint32_t vMaj = 2, vMin = 7, vPat = 1;
    sscanf(g_cfg.hostVer, "%u.%u.%u", &vMaj, &vMin, &vPat);
    g_slFrameTag = (vMaj > 2) || (vMaj == 2 && (vMin > 7 || (vMin == 7 && vPat >= 30)));
    bool optV5   = (vMaj > 2) || (vMaj == 2 && (vMin > 7 || (vMin == 7 && vPat >= 2)));
    g_dlssgOptVer = g_cfg.dlssgStruct ? (uint32_t)g_cfg.dlssgStruct : (optV5 ? 5u : 1u);
    const uint64_t hostVer = ((uint64_t)vMaj << 48) | ((uint64_t)vMin << 32) | ((uint64_t)vPat << 16) | 0xfedcULL;

    pref.flags             = (sl::PreferenceFlags)((unsigned long long)sl::PreferenceFlags::eUseManualHooking | (g_slFrameTag ? SL_FLAG_FRAME_TAGGING : 0ull));
    pref.engine            = sl::EngineType::eCustom;
    pref.engineVersion     = "1.0.0";
    pref.projectId         = "{E57EBA73-277D-4D76-8C29-60AB8155FA36}";
    pref.applicationId     = 141959980;
    pref.renderAPI         = sl::RenderAPI::eD3D12;

    sl::Result r = p_slInit(pref, hostVer);
    L("[SL] slInit(host=%u.%u.%u frameTag=%d dlssgOptV=%u) -> %d (%s)", vMaj, vMin, vPat, (int)g_slFrameTag, g_dlssgOptVer, (int)r, resStr(r));
    if (r != sl::Result::eOk) return false;
    r = p_slSetD3DDevice(deviceV);
    L("[SL] slSetD3DDevice -> %d (%s)", (int)r, resStr(r));

    LUID luid = g_device->GetAdapterLuid();
    sl::AdapterInfo ai{}; ai.deviceLUID = (uint8_t*)&luid; ai.deviceLUIDSizeInBytes = sizeof(luid);
    sl::Result rg = p_slIsFeatureSupported(sl::kFeatureDLSS_G, ai);
    L("[SL] DLSS_G supported -> %d (%s)", (int)rg, resStr(rg));
    if (rg != sl::Result::eOk) { L("[SL] DLSS_G not supported; bridge disabled"); return false; }
    // Activate DLSS super-resolution too, so slGetFeatureFunction can hand back its options fn.
    sl::Result rd = p_slIsFeatureSupported(sl::kFeatureDLSS, ai);
    L("[SL] DLSS(SR) supported -> %d (%s)", (int)rd, resStr(rd));

    if (p_slGetFeatureFunction) {
        p_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)p_slDLSSGSetOptions);
        p_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState",   (void*&)p_slDLSSGGetState);
        p_slGetFeatureFunction(sl::kFeatureDLSS,   "slDLSSSetOptions",  (void*&)p_slDLSSSetOptions);
        p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetMarker", (void*&)p_slReflexSetMarker);
        p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions",(void*&)p_slReflexSetOptions);
        p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep",     (void*&)p_slReflexSleep);
        p_slGetFeatureFunction(kFeaturePCL,        "slPCLSetMarker",    (void*&)p_slPCLSetMarker);
    }
    L("[SL] feature funcs: dlssgSetOpt=%p reflexMarker=%p pclMarker=%p reflexOpt=%p sleep=%p",
      (void*)p_slDLSSGSetOptions, (void*)p_slReflexSetMarker, (void*)p_slPCLSetMarker, (void*)p_slReflexSetOptions, (void*)p_slReflexSleep);

    g_slReady = true;
    L("[SL] init complete, bridge ready");
    return true;
}

// ---- swapchain wrapper: brackets Present with PCL present markers ------------
// DLSS-G paces generated frames off the ePresentStart/ePresentEnd markers, which
// must wrap the real Present call. The game calls Present on whatever we hand it,
// so we hand it this thin proxy that forwards everything to the SL swapchain.
class SwapChainProxy : public IDXGISwapChain4
{
    IDXGISwapChain4* m_inner;
    volatile LONG    m_ref;
public:
    SwapChainProxy(IDXGISwapChain4* in) : m_inner(in), m_ref(1) {}

    void presentMarker(int which) {
        g_presentTid = GetCurrentThreadId();
        if (!g_curToken) return;
        if      (p_slPCLSetMarker)    p_slPCLSetMarker(which == 0 ? PCL_PresentStart : PCL_PresentEnd, *g_curToken);
        else if (p_slReflexSetMarker) p_slReflexSetMarker(which == 0 ? sl::ReflexMarker::ePresentStart : sl::ReflexMarker::ePresentEnd, *g_curToken);
        else return;
        if (which == 0) InterlockedIncrement(&g_presentMarks);
    }
    HRESULT STDMETHODCALLTYPE Present(UINT s, UINT f) override {
        logPresent("Present", s, f);
        presentMarker(0);
        HRESULT hr = m_inner->Present(s, f);
        presentMarker(1);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT s, UINT f, const DXGI_PRESENT_PARAMETERS* p) override {
        logPresent("Present1", s, f);
        presentMarker(0);
        HRESULT hr = m_inner->Present1(s, f, p);
        presentMarker(1);
        return hr;
    }
    void logPresent(const char* who, UINT s, UINT f) {
        static unsigned n = 0;
        if (!g_wrapperLive) { g_wrapperLive = true; L("[PRESENT] FIRST wrapper present call -> wrapper is live"); }
        if ((n++ % 600) == 0) L("[PRESENT] %s #%u presentTid=%lu syncInterval=%u flags=0x%x token=%p marker=%p reflexMk=%p (feedTid=%lu)",
                                who, n, GetCurrentThreadId(), s, f, (void*)g_curToken, (void*)p_slPCLSetMarker, (void*)p_slReflexSetMarker, g_feedTid);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, __uuidof(IDXGIObject)) ||
            IsEqualGUID(riid, __uuidof(IDXGIDeviceSubObject)) || IsEqualGUID(riid, __uuidof(IDXGISwapChain)) ||
            IsEqualGUID(riid, __uuidof(IDXGISwapChain1)) || IsEqualGUID(riid, __uuidof(IDXGISwapChain2)) ||
            IsEqualGUID(riid, __uuidof(IDXGISwapChain3)) || IsEqualGUID(riid, __uuidof(IDXGISwapChain4))) {
            AddRef(); *ppv = static_cast<IDXGISwapChain4*>(this); return S_OK;
        }
        return m_inner->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { m_inner->Release(); delete this; }
        return (ULONG)r;
    }
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return m_inner->SetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override { return m_inner->SetPrivateDataInterface(n, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return m_inner->GetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override { return m_inner->GetParent(riid, pp); }
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** pp) override { return m_inner->GetDevice(riid, pp); }
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID riid, void** pp) override { return m_inner->GetBuffer(i, riid, pp); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fs, IDXGIOutput* o) override { return m_inner->SetFullscreenState(fs, o); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fs, IDXGIOutput** o) override { return m_inner->GetFullscreenState(fs, o); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override { return m_inner->GetDesc(d); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT c, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl) override { return m_inner->ResizeBuffers(c, w, h, fmt, fl); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* p) override { return m_inner->ResizeTarget(p); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override { return m_inner->GetContainingOutput(o); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* st) override { return m_inner->GetFrameStatistics(st); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { return m_inner->GetLastPresentCount(c); }
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { return m_inner->GetDesc1(d); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { return m_inner->GetFullscreenDesc(d); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { return m_inner->GetHwnd(h); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** pp) override { return m_inner->GetCoreWindow(riid, pp); }
    BOOL    STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return m_inner->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return m_inner->GetRestrictToOutput(o); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return m_inner->SetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return m_inner->GetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return m_inner->SetRotation(r); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return m_inner->GetRotation(r); }
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return m_inner->SetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return m_inner->GetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT m) override { return m_inner->SetMaximumFrameLatency(m); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* m) override { return m_inner->GetMaximumFrameLatency(m); }
    HANDLE  STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return m_inner->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return m_inner->SetMatrixTransform(m); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return m_inner->GetMatrixTransform(m); }
    // IDXGISwapChain3
    UINT    STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return m_inner->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE cs, UINT* f) override { return m_inner->CheckColorSpaceSupport(cs, f); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE cs) override { return m_inner->SetColorSpace1(cs); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT c, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl, const UINT* nodes, IUnknown* const* q) override { return m_inner->ResizeBuffers1(c, w, h, fmt, fl, nodes, q); }
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* d) override { return m_inner->SetHDRMetaData(t, s, d); }
};

// ---- in-place Present vtable hook (used instead of the wrapper class above) --
// OptiScaler/Nukem never wrap SL's swapchain in a separate COM object — handing
// the game a different pointer can break SL's swapchain identity / flip-metering
// binding (and is a COM-lifetime crash risk). Instead we give the game SL's REAL
// swapchain and detour Present/Present1 in place to bracket them with PCL markers.
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_Present  g_origPresent  = nullptr;
static PFN_Present1 g_origPresent1 = nullptr;

// Emit one PCL/Reflex marker on the given token (sl.pcl preferred; sl.reflex fallback).
// PCL_* enum values match sl::ReflexMarker for 0..5, so the cast is safe.
static void mark(int m, sl::FrameToken* tok)
{
    if (!tok) return;
    if      (p_slPCLSetMarker)    p_slPCLSetMarker((uint32_t)m, *tok);
    else if (p_slReflexSetMarker) p_slReflexSetMarker((sl::ReflexMarker)m, *tok);
}
// Create the FrameToken for the NEXT frame (one token per frame, explicit monotonic index).
static bool newToken()
{
    if (!p_slGetNewFrameToken) return false;
    uint32_t idx = ++g_frameIdx;
    sl::FrameToken* t = nullptr;
    if (p_slGetNewFrameToken(t, &idx) != sl::Result::eOk || !t) return false;
    g_curToken = t;
    return true;
}

static sl::Result setDlssgOptions(uint32_t mode, bool interpOnly);  // defined below

// Reflex present loop, part 1 — just BEFORE the real Present(): assert options + ePresentStart.
static void presentPre()
{
    g_presentTid = GetCurrentThreadId();
    if (!g_wrapperLive) { g_wrapperLive = true; L("[PRESENT] FIRST hooked present -> live (tid=%lu)", g_presentTid); }
    if (!g_curToken) return;
    // Assert DLSS-G options EVERY frame on the PRESENT thread, right before Present
    // (ProgrammingGuideDLSS_G:483-491 + OptiScaler DLSSG_Dx12.cpp:355-366 driven from the present hook).
    {
        int wantMode = g_fgEnabled ? 1 : 0;
        sl::Result orr = setDlssgOptions((uint32_t)wantMode, g_diagShowInterpOnly);
        if (wantMode != g_lastAppliedMode || g_diagShowInterpOnly != g_lastAppliedInterp) {
            L("[FGC] slDLSSGSetOptions(mode=%d interpOnly=%d v5 qpm=block) -> %d (%s)", wantMode, (int)g_diagShowInterpOnly, (int)orr, resStr(orr));
            g_lastAppliedMode = wantMode; g_lastAppliedInterp = g_diagShowInterpOnly;
        }
    }
    // Reflex options BEFORE Present. useMarkersToOptimize=false: this game is single-threaded and
    // emits no genuine sim/submit markers, so telling Reflex to optimize off them corrupts the pacer
    // and drops the generated frame (OptiScaler DLSSG_Dx12.cpp:373-375; sl_reflex.h field doc).
    if (p_slReflexSetOptions) {
        sl::ReflexOptions ro{}; ro.mode = sl::ReflexMode::eLowLatency; ro.frameLimitUs = g_frameLimitUs;
        ro.useMarkersToOptimize = false;
        p_slReflexSetOptions(ro);
    }
    // Close the render-submit interval, then open present (full marker cadence for the SW pacer).
    mark(PCL_RenderEnd,    g_curToken);
    mark(PCL_PresentStart, g_curToken);
    InterlockedIncrement(&g_presentMarks);
}
// Reflex present loop, part 2 — just AFTER the real Present() returns (the true frame
// boundary, single-threaded render/present thread). This is the SAFE place for the sleep
// (mid-dispatch sleep crashed the game). Sequence: ePresentEnd(thisFrame) -> new token for
// the next frame -> ReflexSetOptions(+frameLimitUs) -> slReflexSleep(nextToken) which
// throttles the start of the next real frame and gives flip metering its cadence.
static void presentPost()
{
    if (!g_curToken) return;
    mark(PCL_PresentEnd, g_curToken);   // token advances in feedDLSSG keyed by the game's frameID
    // slReflexSleep after ePresentEnd on the present thread (OptiScaler hooks_FG_Hooks.cpp:1263-1264).
    if (g_presentCount++ >= WARMUP_PRESENTS && p_slReflexSleep) p_slReflexSleep(*g_curToken);
}
static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* This, UINT s, UINT f)
{
    presentPre();
    HRESULT hr = g_origPresent(This, s, f);
    presentPost();
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* This, UINT s, UINT f, const DXGI_PRESENT_PARAMETERS* p)
{
    presentPre();
    HRESULT hr = g_origPresent1(This, s, f, p);
    presentPost();
    return hr;
}
// IDXGISwapChain vtable: Present=8, IDXGISwapChain1::Present1=22. Idempotent.
static void hookSwapchainPresent(IDXGISwapChain4* sc)
{
    void** vtbl = *(void***)sc;
    DWORD oldp;
    if (vtbl[8] != (void*)&Hook_Present && VirtualProtect(&vtbl[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp)) {
        g_origPresent = (PFN_Present)vtbl[8];
        vtbl[8] = (void*)&Hook_Present;
        VirtualProtect(&vtbl[8], sizeof(void*), oldp, &oldp);
    }
    if (vtbl[22] != (void*)&Hook_Present1 && VirtualProtect(&vtbl[22], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp)) {
        g_origPresent1 = (PFN_Present1)vtbl[22];
        vtbl[22] = (void*)&Hook_Present1;
        VirtualProtect(&vtbl[22], sizeof(void*), oldp, &oldp);
    }
    // Which module owns the original Present we detoured? If it's dxgi.dll (native) rather than
    // sl.interposer.dll, we're bypassing SL's DLSS-G present entirely -> no interpolation.
    HMODULE mod = nullptr; char modName[MAX_PATH] = {0};
    if (g_origPresent && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            (LPCSTR)g_origPresent, &mod) && mod) {
        GetModuleFileNameA(mod, modName, MAX_PATH);
    }
    L("[SC] hooked Present(orig=%p) Present1(orig=%p) on sc4=%p  origPresentModule=%s",
      (void*)g_origPresent, (void*)g_origPresent1, (void*)sc, modName[0] ? modName : "<unknown>");
}

// SL 2.11 DLSSGOptions is kStructVersion5; our sl2.2 header only defines v1 (no
// queueParallelismMode). Lay out v5 by hand so we can set eBlockPresentingClientQueue,
// which OptiScaler sets and without which the generated frame is dropped.
// GUID fac5f1cb-2dfd-4f36-a1e6-3a9e865256c5.
#pragma pack(push, 8)
struct DLSSGOptions5 {
    void*    next = nullptr;
    uint32_t g0 = 0xfac5f1cb; uint16_t g1 = 0x2dfd, g2 = 0x4f36; uint8_t g3[8] = {0xa1,0xe6,0x3a,0x9e,0x86,0x52,0x56,0xc5};
    uint64_t structVersion = 5;
    uint32_t mode = 0;                 // DLSSGMode: 0=eOff 1=eOn
    uint32_t numFramesToGenerate = 1;
    uint32_t flags = 0;                // eShowOnlyInterpolatedFrame = 1<<0
    uint32_t dynamicResWidth = 0, dynamicResHeight = 0;
    uint32_t numBackBuffers = 0;
    uint32_t mvecDepthWidth = 0, mvecDepthHeight = 0;
    uint32_t colorWidth = 0, colorHeight = 0;
    uint32_t colorBufferFormat = 0, mvecBufferFormat = 0, depthBufferFormat = 0, hudLessBufferFormat = 0, uiBufferFormat = 0;
    void*    onErrorCallback = nullptr;
    uint32_t bReserved15 = 2;          // Boolean::eInvalid
    uint32_t queueParallelismMode = 0; // DLSSGQueueParallelismMode::eBlockPresentingClientQueue
    uint32_t enableUserInterfaceRecomposition = 0; // Boolean::eFalse
    float    dynamicTargetFrameRate = 0;
};
#pragma pack(pop)

// Hand-laid DLSSGState v2 so we can read numFramesToGenerateMax (the card's MFG cap).
// GUID cc8ac8e1-a179-44f5-97fa-e74112f9bc61.
#pragma pack(push, 8)
struct DLSSGState2 {
    void*    next = nullptr;
    uint32_t g0 = 0xcc8ac8e1; uint16_t g1 = 0xa179, g2 = 0x44f5; uint8_t g3[8] = {0x97,0xfa,0xe7,0x41,0x12,0xf9,0xbc,0x61};
    uint64_t structVersion = 2;
    uint64_t estimatedVRAMUsageInBytes = 0;
    uint32_t status = 0;
    uint32_t minWidthOrHeight = 0;
    uint32_t numFramesActuallyPresented = 0;
    uint32_t numFramesToGenerateMax = 0;
};
#pragma pack(pop)

static uint32_t g_dlssgMaxGen = 1;   // numFramesToGenerateMax reported by the card (1 = 2x only)
static sl::Result setDlssgOptions(uint32_t mode, bool interpOnly)
{
    if (!p_slDLSSGSetOptions) return sl::Result::eErrorNotInitialized;
    DLSSGOptions5 o;
    o.structVersion = g_dlssgOptVer;   // decoupled from tagging: v5 for 2.7.2+ (MFG/flip metering), v1 for 2.7.1
    o.mode = mode;
    // Multi-frame gen: multiplier N -> N-1 generated frames. Clamp to the card's reported max
    // (populated from DLSSGState in feedDLSSG); defaults to 1 (2x) which is safe on any card.
    uint32_t want = (uint32_t)(g_cfg.fgMultiplier > 1 ? g_cfg.fgMultiplier - 1 : 1);
    if (g_dlssgMaxGen >= 1 && want > g_dlssgMaxGen) want = g_dlssgMaxGen;
    o.numFramesToGenerate = want;
    o.flags = interpOnly ? 1u : 0u;
    o.queueParallelismMode = 0;   // eBlockPresentingClientQueue (v3+, ignored at v1)
    sl::ViewportHandle vp{0};
    return p_slDLSSGSetOptions(vp, *(const sl::DLSSGOptions*)&o);
}

// ---- swapchain substitution -------------------------------------------------
extern "C" void* slbridge_create_swapchain(void* oldDescV, void* factoryV, void* queueV, void** outSwapchain)
{
    if (!g_slReady) { L("[SC] not ready, cannot substitute"); return nullptr; }
    DXGI_SWAP_CHAIN_DESC* od = (DXGI_SWAP_CHAIN_DESC*)oldDescV;
    if (!od || !factoryV || !queueV || !outSwapchain) { L("[SC] null args"); return nullptr; }

    L("[SC] NEW_DX12: %ux%u fmt=%u bufCount=%u swapEffect=%u flags=0x%x hwnd=%p windowed=%d",
      od->BufferDesc.Width, od->BufferDesc.Height, od->BufferDesc.Format, od->BufferCount,
      od->SwapEffect, od->Flags, (void*)od->OutputWindow, od->Windowed);

    // Upgrade the game's factory to an SL proxy (manual hooking).
    IDXGIFactory* factory = (IDXGIFactory*)factoryV;
    sl::Result ur = p_slUpgradeInterface((void**)&factory);
    L("[SC] slUpgradeInterface(factory) -> %d (%s) proxyFactory=%p", (int)ur, resStr(ur), (void*)factory);
    if (ur != sl::Result::eOk) return nullptr;

    // Do NOT upgrade the command queue — OptiScaler proves the native present queue
    // is passed to CreateSwapChain as-is; SL's proxy swapchain owns presentation.
    ID3D12CommandQueue* queue = (ID3D12CommandQueue*)queueV;

    IDXGIFactory2* f2 = nullptr;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&f2))) || !f2) { L("[SC] QI IDXGIFactory2 failed"); return nullptr; }

    DXGI_SWAP_CHAIN_DESC1 d1{};
    d1.Width       = od->BufferDesc.Width;
    d1.Height      = od->BufferDesc.Height;
    d1.Format      = od->BufferDesc.Format;
    d1.Stereo      = FALSE;
    d1.SampleDesc  = od->SampleDesc;           // FG requires {1,0}
    d1.BufferUsage = od->BufferUsage;
    d1.BufferCount = od->BufferCount;
    d1.Scaling     = DXGI_SCALING_STRETCH;
    d1.SwapEffect  = od->SwapEffect;           // flip model expected
    d1.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    d1.Flags       = od->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
    fs.RefreshRate      = od->BufferDesc.RefreshRate;
    fs.ScanlineOrdering = od->BufferDesc.ScanlineOrdering;
    fs.Scaling          = od->BufferDesc.Scaling;
    fs.Windowed         = od->Windowed;

    // Tell SL to load DLSS-G for this swapchain BEFORE creating it, so the proxy factory
    // builds a DLSS-G-capable (interpolating) swapchain rather than a passthrough one
    // (OptiScaler DLSSG_Dx12.cpp:210 does this right before CreateSwapChainForHwnd).
    if (p_slSetFeatureLoaded) {
        sl::Result fr = p_slSetFeatureLoaded(sl::kFeatureDLSS_G, true);
        L("[SC] slSetFeatureLoaded(DLSS_G,true) -> %d (%s)", (int)fr, resStr(fr));
    }

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = f2->CreateSwapChainForHwnd((IUnknown*)queue, od->OutputWindow, &d1, &fs, nullptr, &sc1);
    f2->Release();
    if (FAILED(hr) || !sc1) { L("[SC] CreateSwapChainForHwnd FAILED hr=0x%08lx", (unsigned long)hr); return nullptr; }

    IDXGISwapChain4* sc4 = nullptr;
    if (FAILED(sc1->QueryInterface(IID_PPV_ARGS(&sc4))) || !sc4) { L("[SC] QI IDXGISwapChain4 failed"); sc1->Release(); return nullptr; }
    sc1->Release();

    BridgeCtx* c = new BridgeCtx();
    c->magic = CTX_MAGIC; c->kind = CTX_SWAPCHAIN; c->sc4 = sc4;
    c->texDesc = { /*type TEX2D*/2, 0 /*fill below*/, d1.Width, d1.Height, 1, 1, 0, /*usage RT|UAV*/3 };
    // map the DXGI format roughly into an FfxApiSurfaceFormat-ish value isn't needed for B; store raw later.

    // Dummy command list (returned for InterpolationCommandList queries).
    g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c->dummyAlloc));
    if (c->dummyAlloc) {
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->dummyAlloc, nullptr, IID_PPV_ARGS(&c->dummyList));
        if (c->dummyList) { c->dummyList->Close(); c->listOpen = false; }
    }
    // Dummy interpolation texture (display-size, swapchain format).
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = d1.Width; rd.Height = d1.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = d1.Format; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT thr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&c->dummyTex));
        c->texDesc.format = (uint32_t)d1.Format; // store DXGI format; B doesn't use it semantically
        if (FAILED(thr)) L("[SC] dummy interp texture create hr=0x%08lx", (unsigned long)thr);
    }
    // Transparent UIColorAndAlpha surface (R8G8B8A8 - 8-bit alpha). DLSS-G in 2.11
    // requires a UI tag; the game bakes UI into the backbuffer so we feed an empty one.
    c->dispW = d1.Width; c->dispH = d1.Height;
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = d1.Width; rd.Height = d1.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // {0,0,0,0} transparent
        HRESULT uhr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(&c->uiTex));
        if (FAILED(uhr)) L("[SC] uiTex create hr=0x%08lx", (unsigned long)uhr);
    }

    // Hand the game SL's REAL swapchain (preserves SL identity / flip-metering bind)
    // and detour its Present/Present1 in place for the PCL present markers. Two refs:
    // one for the game (Released when it destroys the swapchain), one kept in c->sc4.
    hookSwapchainPresent(sc4);
    sc4->AddRef();
    *outSwapchain = static_cast<IDXGISwapChain4*>(sc4);
    registerCtx(c);
    g_scCtx = c;
    L("[SC] substituted SL swapchain sc4=%p (handed to game, Present hooked in place) ctx=%p", (void*)sc4, (void*)c);

    // Leave DLSS-G OFF initially (passthrough); feedDLSSG turns it on.
    { sl::Result orr = setDlssgOptions(0, false); L("[SC] slDLSSGSetOptions(eOff v5) -> %d (%s)", (int)orr, resStr(orr)); }
    return c;
}

extern "C" void* slbridge_create_framegen(const void* /*createDescChain*/)
{
    if (!g_slReady) return nullptr;
    BridgeCtx* c = new BridgeCtx();
    c->magic = CTX_MAGIC; c->kind = CTX_FRAMEGEN;
    registerCtx(c);
    L("[FG] framegen context created ctx=%p (handled by SL, not AMD)", (void*)c);
    return c;
}

extern "C" void* slbridge_create_upscale(void)
{
    if (!g_slReady) return nullptr;
    BridgeCtx* c = new BridgeCtx();
    c->magic = CTX_MAGIC; c->kind = CTX_UPSCALE;
    registerCtx(c);
    L("[SR] upscale context SUBSTITUTED (dummy; no real AMD FSR upscaler session created) ctx=%p", (void*)c);
    return c;
}

extern "C" bool slbridge_want_stub_upscale(void)
{
    // Opt-in (stub_fsr.flag) + only when DLSS-SR is on (we replace AMD's upscaler); else we still
    // need the real AMD upscaler as the fallback. Read the flag live (cheap, few calls at startup).
    static int scan = 0; static bool s_stub = false;
    if ((scan++ % 8) == 0)
        s_stub = (GetFileAttributesW(modPath(L"stub_fsr.flag")) != INVALID_FILE_ATTRIBUTES);
    return s_stub && g_cfg.dlssSR;
}

// ---- desc field readers (descs are valid game memory) -----------------------
static uint32_t rdU32(const void* b, size_t o){ return *(const uint32_t*)((const char*)b+o); }
static uint64_t rdU64(const void* b, size_t o){ return *(const uint64_t*)((const char*)b+o); }
static float    rdF32(const void* b, size_t o){ return *(const float*)((const char*)b+o); }
static void*    rdPtr(const void* b, size_t o){ return *(void* const*)((const char*)b+o); }

static const uint64_t T_FG_CONFIGURE = 0x00020002;
static const uint64_t T_FG_PREPARE   = 0x00020004;
static const uint64_t T_FG_DISPATCH  = 0x00020003;

// Frame start: called from the proxy on the first FfxApi call of the frame (UPSCALE dispatch).
// Opens the Reflex frame with eSimulationStart on this frame's token (the token was created at
// the previous frame's post-present; bootstrap it on the very first frame).
extern "C" void slbridge_frame_start()
{
    // Native single-token mode (sr_native_token.flag): open the Reflex frame HERE, at the UPSCALE
    // dispatch (the frame's first FfxApi call), so DLSS-SR evaluates on the SAME frame token that
    // DLSS-G will use — the pattern a native DLSS-SR+DLSS-G title emits, which lets the Steam
    // overlay register the DLSS upscaler on the current frame (correct label) with one native frame
    // per real frame (correct count). feedDLSSG reuses this token. Default (flag absent): stay a
    // no-op and let feedDLSSG open the frame (proven-working 3x path; DLSS-SR rides the prior token).
    if (!g_slReady || !p_slGetNewFrameToken) return;
    static int scan = 0;
    if ((scan++ % 120) == 0)
        g_nativeToken = (GetFileAttributesW(modPath(L"sr_native_token.flag")) != INVALID_FILE_ATTRIBUTES);
    if (!g_nativeToken) return;
    uint32_t idx = ++g_frameIdx;
    sl::FrameToken* t = nullptr;
    if (p_slGetNewFrameToken(t, &idx) == sl::Result::eOk && t) {
        g_curToken = t;
        g_frameTokenReady = true;
        mark(PCL_SimStart, t);   // frame opens on this token
    }
}

// Feed DLSS-G from the FRAMEGEN PREPARE dispatch (depth + MV + camera scalars).
static void feedDLSSG(const void* d)
{
    if (!g_slReady || !p_slDLSSGSetOptions || !p_slSetTag || !p_slSetConstants || !p_slGetNewFrameToken) return;

    void*    cmdList = rdPtr(d, 0x20);
    uint32_t renderW = rdU32(d, 0x28), renderH = rdU32(d, 0x2C);
    float    jitX = rdF32(d, 0x30), jitY = rdF32(d, 0x34);
    float    mvsX = rdF32(d, 0x38), mvsY = rdF32(d, 0x3C);
    float    camNear = rdF32(d, 0x48), camFar = rdF32(d, 0x4C), camFov = rdF32(d, 0x50);
    void*    depthRes = rdPtr(d, 0x58); uint32_t depthFmt=rdU32(d,0x64), depthW=rdU32(d,0x68), depthH=rdU32(d,0x6C), depthSt=rdU32(d,0x80);
    void*    mvRes    = rdPtr(d, 0x88); uint32_t mvFmt=rdU32(d,0x94), mvW=rdU32(d,0x98), mvH=rdU32(d,0x9C), mvSt=rdU32(d,0xB0);

    // DLSS-G requires the app to poll the current back-buffer index every frame.
    if (g_scCtx && g_scCtx->sc4) g_scCtx->sc4->GetCurrentBackBufferIndex();

    sl::ViewportHandle vp{0};
    ++g_frameCount;
    // Key the frame token by the GAME's frameID (OptiScaler model). The game runs PREPARE
    // more than once per presented frame; SL forbids setting common constants twice for the
    // same frame. Advance the token only when the game's frameID changes, and feed once.
    uint32_t gameFrameID = (uint32_t)rdU64(d, 0x10);
    if (gameFrameID == g_curFrameID) return;   // already fed this game frame
    bool openedHere = false;
    if (g_frameTokenReady) {
        // Native single-token mode: frame_start (UPSCALE) already opened this frame's token so
        // DLSS-SR + DLSS-G share it. Reuse it; its SimStart was already emitted there.
        g_frameTokenReady = false;
        g_curFrameID = gameFrameID;
    } else {
        uint32_t idx = gameFrameID ? gameFrameID : ++g_frameIdx;
        sl::FrameToken* t = nullptr;
        if (p_slGetNewFrameToken(t, &idx) != sl::Result::eOk || !t) return;
        g_curToken = t; g_curFrameID = gameFrameID;
        openedHere = true;
    }
    sl::FrameToken* token = g_curToken;
    g_feedTid = GetCurrentThreadId();
    // With HW flip metering BLOCKED (nvhook), DLSS-G runs on the SOFTWARE pacer — the same path
    // that GENERATED (presented=2) on SL 2.2. That pacer needs the full sim/render marker cadence,
    // so re-add the sim + render-submit-start markers here (matches the 2.2-working config).
    // eRenderSubmitEnd is emitted in presentPre just before the real Present.
    if (openedHere) mark(PCL_SimStart, token);   // in native mode frame_start already emitted SimStart
    mark(PCL_SimEnd,      token);
    mark(PCL_RenderStart, token);

    sl::Resource depthR = makeSlTex(depthRes, depthSt, depthW, depthH, depthFmt);
    sl::Resource mvR    = makeSlTex(mvRes,    mvSt,    mvW,    mvH,    mvFmt);
    sl::Resource hudR;
    // OptiScaler sets the tag extent explicitly (DLSSG_Dx12.cpp:1083-1086); leaving it {0,0,0,0}
    // triggers SL's "Invalid backbuffer resource extent (0 x 0)" reset every frame.
    sl::Extent depthExt{0, 0, depthW, depthH};
    sl::Extent mvExt   {0, 0, mvW,    mvH};
    sl::Extent hudExt  {0, 0, g_hudless.width, g_hudless.height};
    sl::ResourceTag tags[3]; int nt = 0;
    tags[nt++] = sl::ResourceTag(&depthR, sl::kBufferTypeDepth,         sl::ResourceLifecycle::eValidUntilPresent, &depthExt);
    tags[nt++] = sl::ResourceTag(&mvR,    sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &mvExt);
    if (g_hudless.res) {
        hudR = makeSlTex(g_hudless.res, g_hudless.state, g_hudless.width, g_hudless.height, g_hudless.format);
        tags[nt++] = sl::ResourceTag(&hudR, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &hudExt);
    }
    if (p_slSetTagForFrame) p_slSetTagForFrame(*token, vp, tags, nt, (void*)cmdList);
    else                    p_slSetTag(vp, tags, nt, (sl::CommandBuffer*)cmdList);

    // Reversed-Z / infinite far: the game passes near=FLT_MAX, far=10 -> real near plane is the
    // finite value. Pick the finite one as zNear and treat the huge one as infinity.
    float zNear = (camNear > 1.0e9f) ? camFar : camNear;
    float zFar  = (camNear > 1.0e9f || camFar > 1.0e9f) ? 100000.0f : camFar;

    // FFX motionVectorScale == renderSize means the game's MVs are in UV space already, so
    // Streamline's mvecScale must be {1,1} (NOT the FFX scale, which blew up reprojection -> red).
    float slMvX = 1.0f, slMvY = 1.0f;

    float aspect = renderH ? (float)renderW / (float)renderH : 1.7778f;
    sl::Constants c;
    c.cameraViewToClip = buildProjRH(camFov, aspect, zNear);
    c.clipToCameraView = ident4();
    c.clipToLensClip   = ident4();
    c.clipToPrevClip   = ident4();
    c.prevClipToClip   = ident4();
    c.jitterOffset = sl::float2(jitX, jitY);
    c.mvecScale    = sl::float2(slMvX, slMvY);
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(0,0,0); c.cameraUp = sl::float3(0,1,0);
    c.cameraRight = sl::float3(1,0,0); c.cameraFwd = sl::float3(0,0,1);
    c.cameraNear = zNear; c.cameraFar = zFar; c.cameraFOV = camFov; c.cameraAspectRatio = aspect;
    c.depthInverted        = sl::Boolean::eTrue;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D      = sl::Boolean::eFalse;
    c.reset                = sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated   = sl::Boolean::eFalse;
    c.motionVectorsJittered  = sl::Boolean::eFalse;
    p_slSetConstants(c, *token, vp);

    // Dev diagnostic toggle (no rebuild): presence of the flag file forces
    // eShowOnlyInterpolatedFrame, which presents ONLY generated frames. If the
    // screen then shows a moving image, generation reaches the display; if it is
    // black/frozen, generated frames are produced but never presented.
    if ((g_frameCount % 120) == 1) {
        g_diagShowInterpOnly = (GetFileAttributesW(modPath(L"fg_interp.flag")) != INVALID_FILE_ATTRIBUTES);
        // No automatic base-rate cap: an artificial Reflex frameLimitUs caps the FINAL (total)
        // present rate, not the base render rate, so it can only hurt. Let Reflex/the flip queue
        // pace natively. Only the manual dev override (fg_fpscap.txt) still applies, if present.
        g_frameLimitUs = readFpsCapUs();
        // Steam DLSS-G marker fix (no rebuild): steam_dlss.flag present -> synthesize OOB-present
        // markers so the Steam overlay recognizes DLSS-G under HW flip metering (which otherwise
        // emits too few). File content (a number) overrides the per-frame OOB pair count for tuning;
        // empty file -> use the effective multiplier (numFramesToGenerate+1).
        { int n = 0;
          FILE* sf = _wfopen(modPath(L"steam_dlss.flag"), L"rb");
          if (sf) { char b[16] = {0}; size_t k = fread(b, 1, sizeof(b) - 1, sf); fclose(sf);
                    int v = k ? atoi(b) : 0;
                    uint32_t mult = (g_cfg.fgMultiplier > 1) ? (uint32_t)g_cfg.fgMultiplier : 2;
                    if (g_dlssgMaxGen >= 1 && mult > g_dlssgMaxGen + 1) mult = g_dlssgMaxGen + 1;
                    n = (v > 0) ? v : (int)mult; }
          nvhook_set_oob_inject(n);
        }
    }

    // slDLSSGSetOptions is asserted in presentPre() every frame on the PRESENT thread, right before
    // the real Present. NVIDIA requires options be set on the presenting thread or SL "cannot
    // guarantee which Present call will pick up the updated options" (ProgrammingGuideDLSS_G:483-491).
    g_dlssgMode = g_fgEnabled ? 1u : 0u;

    // Query state EVERY frame (so numFramesActuallyPresented is per-frame, not a
    // 240-frame-stale sample) and track the max/any doubling ever observed.
    if (p_slDLSSGGetState) {
        DLSSGState2 st{};
        p_slDLSSGGetState(vp, *(sl::DLSSGState*)&st, nullptr);
        if (st.numFramesToGenerateMax >= 1 && st.numFramesToGenerateMax <= 8) g_dlssgMaxGen = st.numFramesToGenerateMax;
        if (st.numFramesActuallyPresented > g_maxPresented) g_maxPresented = st.numFramesActuallyPresented;
        if (st.numFramesActuallyPresented >= 2) ++g_doubledFrames;
        // Measure the TRUE base (real-frame) render rate: g_frameCount ticks once per real game
        // frame, so d(frameCount)/d(wallclock) is the base fps. total displayed = base * multiplier
        // (compare against PresentMon). This settles base-vs-total for the MFG pacing question.
        static ULONGLONG s_lastTick = 0; static uint32_t s_lastFrame = 0; static float s_baseFps = 0;
        ULONGLONG nowMs = GetTickCount64();
        if (s_lastTick && nowMs > s_lastTick) s_baseFps = (g_frameCount - s_lastFrame) * 1000.0f / (float)(nowMs - s_lastTick);
        if (g_frameCount <= 3 || (g_frameCount % 240) == 5) {
            L("[FGC] f=%u baseFps=%.1f (x%dreq -> %.0f total) feedTid=%lu fgEn=%d mode=%u mult=%dx maxGen=%u status=0x%x presented=%u maxPres=%u dblFrames=%u vram=%lluMB minWH=%u render=%ux%u zNear=%.1f hudTagged=%d interpOnly=%d | wrapperLive=%d presentTid=%lu presentMarks=%ld",
              g_frameCount, s_baseFps, g_cfg.fgMultiplier, s_baseFps * g_cfg.fgMultiplier,
              g_feedTid, (int)g_fgEnabled, g_dlssgMode, g_cfg.fgMultiplier, g_dlssgMaxGen, (unsigned)st.status, st.numFramesActuallyPresented,
              g_maxPresented, g_doubledFrames, (unsigned long long)(st.estimatedVRAMUsageInBytes >> 20), st.minWidthOrHeight, renderW, renderH,
              zNear, (int)(g_lastHudTagged), (int)g_diagShowInterpOnly,
              (int)g_wrapperLive, g_presentTid, (long)g_presentMarks);
            s_lastTick = nowMs; s_lastFrame = g_frameCount;
        }
    }
}

// ---- DLSS super-resolution: replace the FSR upscale dispatch (0x00010001) -----
// Returns true if DLSS-SR did the upscale (proxy then SKIPS forwarding to AMD's FSR); false to
// fall back to FSR. Reads the FfxApi upscale desc (offsets from CONTRACT.md), runs SL DLSS on the
// game's command list writing the upscaled result into the game's own output buffer, which the
// downstream DLSS-G/present path then consumes. Independent viewport (vp1) + token space from FG.
extern "C" bool slbridge_upscale(const void* d)
{
    if (!g_slReady || !p_slGetNewFrameToken || !p_slSetConstants || !d) return false;
    static unsigned scan = 0;
    if ((scan++ % 120) == 0) g_srDisabled = !g_cfg.dlssSR || (GetFileAttributesW(modPath(L"dlss_sr_off.flag")) != INVALID_FILE_ATTRIBUTES);
    if (g_srDisabled) return false;

    void*    cmdList = rdPtr(d, 0x10);
    void*    colorRes= rdPtr(d, 0x18); uint32_t colorFmt=rdU32(d,0x24), colorW=rdU32(d,0x28), colorH=rdU32(d,0x2C), colorSt=rdU32(d,0x40);
    void*    depthRes= rdPtr(d, 0x48); uint32_t depthFmt=rdU32(d,0x54), depthW=rdU32(d,0x58), depthH=rdU32(d,0x5C), depthSt=rdU32(d,0x70);
    void*    mvRes   = rdPtr(d, 0x78); uint32_t mvFmt=rdU32(d,0x84), mvW=rdU32(d,0x88), mvH=rdU32(d,0x8C), mvSt=rdU32(d,0xA0);
    void*    outRes  = rdPtr(d, 0x138); uint32_t outFmt=rdU32(d,0x144), outW=rdU32(d,0x148), outH=rdU32(d,0x14C), outSt=rdU32(d,0x160);
    float    jitX=rdF32(d,0x168), jitY=rdF32(d,0x16C);
    uint32_t renderW=rdU32(d,0x178), renderH=rdU32(d,0x17C);
    static int srDiag = 0;
    if (srDiag < 3) { ++srDiag;
        L("[SR] call#%d: dlssOpt=%p eval=%p | color=%p %ux%u fmt=%u | depth=%p %ux%u | mv=%p %ux%u | out=%p %ux%u fmt=%u | render=%ux%u cmd=%p",
          srDiag, (void*)p_slDLSSSetOptions, (void*)p_slEvaluateFeature, colorRes, colorW, colorH, colorFmt,
          depthRes, depthW, depthH, mvRes, mvW, mvH, outRes, outW, outH, outFmt, renderW, renderH, cmdList); }
    if (!p_slDLSSSetOptions || !p_slEvaluateFeature) return false;
    if (!colorRes || !outRes || !depthRes || !mvRes || !renderW || !renderH || !outW || !outH) return false;
    if (!cmdList) return false;

    sl::ViewportHandle vp{1};

    // Choose a DLSS quality preset from the render:output ratio; only re-set options on change.
    float ratio = (float)renderW / (float)outW;
    sl::DLSSMode mode = sl::DLSSMode::eMaxQuality;
    if      (ratio > 0.92f) mode = sl::DLSSMode::eDLAA;
    else if (ratio > 0.62f) mode = sl::DLSSMode::eMaxQuality;
    else if (ratio > 0.54f) mode = sl::DLSSMode::eBalanced;
    else if (ratio > 0.45f) mode = sl::DLSSMode::eMaxPerformance;
    else                    mode = sl::DLSSMode::eUltraPerformance;
    if (outW != g_srLastOutW || outH != g_srLastOutH || (int)mode != g_srLastMode) {
        sl::DLSSOptions o;
        o.mode = mode; o.outputWidth = outW; o.outputHeight = outH;
        o.colorBuffersHDR = sl::Boolean::eTrue;   // game renders HDR10 PQ
        o.useAutoExposure = sl::Boolean::eTrue;   // no separate exposure tag needed
        o.sharpness = 0.0f;
        sl::Result orr = p_slDLSSSetOptions(vp, o);
        L("[SR] slDLSSSetOptions mode=%d render=%ux%u out=%ux%u ratio=%.2f -> %d (%s)", (int)mode, renderW, renderH, outW, outH, ratio, (int)orr, resStr(orr));
        g_srLastOutW = outW; g_srLastOutH = outH; g_srLastMode = (int)mode;
    }

    // Share DLSS-G's frame token instead of minting a separate SR token. A second
    // slGetNewFrameToken stream per real frame makes Reflex — and the Steam overlay reading
    // its markers — count TWO native frames per frame (native shows 2x, e.g. 150 for a real 75).
    // DLSS-SR is a separate viewport (vp1) on the SAME real frame, so it belongs on the same
    // token as DLSS-G (vp0). g_curToken advances once per real frame in feedDLSSG, so SR still
    // gets a distinct monotonic token each frame (its temporal history stays correct). Fall back
    // to a private token only before DLSS-G has produced its first one.
    // Diagnostic A/B toggle (no rebuild): sr_separate_token.flag forces the OLD separate SR
    // token stream (Steam shows "DLSS" label but doubles the native count) so its NvAPI marker
    // stream can be compared against sharing DLSS-G's token (correct count, "FSR" label).
    static int s_srTokScan = 0; static bool s_srSeparate = false;
    if ((s_srTokScan++ % 120) == 0)
        s_srSeparate = (GetFileAttributesW(modPath(L"sr_separate_token.flag")) != INVALID_FILE_ATTRIBUTES);
    sl::FrameToken* t = g_curToken;
    if (s_srSeparate || !t) {
        uint32_t idx = ++g_srFrameIdx;
        if (p_slGetNewFrameToken(t, &idx) != sl::Result::eOk || !t) return false;
    }

    sl::Constants c;
    c.cameraViewToClip = ident4(); c.clipToCameraView = ident4();
    c.clipToLensClip = ident4(); c.clipToPrevClip = ident4(); c.prevClipToClip = ident4();
    c.jitterOffset = sl::float2(jitX, jitY);
    c.mvecScale    = sl::float2(1.0f, 1.0f);   // MVs are UV-space (motionVectorScale==renderSize)
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(0,0,0); c.cameraUp = sl::float3(0,1,0); c.cameraRight = sl::float3(1,0,0); c.cameraFwd = sl::float3(0,0,1);
    c.cameraNear = 0.1f; c.cameraFar = 10000.0f; c.cameraFOV = 1.0f; c.cameraAspectRatio = (float)renderW / (float)renderH;
    c.depthInverted        = sl::Boolean::eTrue;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D      = sl::Boolean::eFalse;
    c.reset                = sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated   = sl::Boolean::eFalse;
    c.motionVectorsJittered  = sl::Boolean::eFalse;
    p_slSetConstants(c, *t, vp);

    sl::Resource inC = makeSlTex(colorRes, colorSt, colorW, colorH, colorFmt);
    sl::Resource dep = makeSlTex(depthRes, depthSt, depthW, depthH, depthFmt);
    sl::Resource mvR = makeSlTex(mvRes,    mvSt,    mvW,    mvH,    mvFmt);
    sl::Resource out = makeSlTex(outRes,   outSt,   outW,   outH,   outFmt);
    sl::Extent inExt{0, 0, renderW, renderH}, outExt{0, 0, outW, outH};
    sl::ResourceTag tags[4];
    tags[0] = sl::ResourceTag(&inC, sl::kBufferTypeScalingInputColor,  sl::ResourceLifecycle::eValidUntilEvaluate, &inExt);
    tags[1] = sl::ResourceTag(&dep, sl::kBufferTypeDepth,              sl::ResourceLifecycle::eValidUntilEvaluate, &inExt);
    tags[2] = sl::ResourceTag(&mvR, sl::kBufferTypeMotionVectors,      sl::ResourceLifecycle::eValidUntilEvaluate, &inExt);
    tags[3] = sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outExt);
    p_slSetTag(vp, tags, 4, (sl::CommandBuffer*)cmdList);

    const sl::BaseStructure* inputs[] = { (const sl::BaseStructure*)&vp };
    sl::Result er = p_slEvaluateFeature(sl::kFeatureDLSS, *t, inputs, 1, (sl::CommandBuffer*)cmdList);
    static int okLog = 0, errLog = 0;
    if (er == sl::Result::eOk) { if (okLog < 1) { ++okLog; L("[SR] DLSS upscale ACTIVE (render %ux%u -> %ux%u)", renderW, renderH, outW, outH); } }
    else if (errLog < 8)       { ++errLog; L("[SR] slEvaluateFeature(DLSS) -> %d (%s); falling back to FSR", (int)er, resStr(er)); }
    return er == sl::Result::eOk;
}

// ---- per-frame handlers for our contexts ------------------------------------
extern "C" uint32_t slbridge_configure(void* handle, const void* descV)
{
    (void)handle;
    const FfxHeader* h = (const FfxHeader*)descV;
    if (h && h->type == T_FG_CONFIGURE) {
        bool wasEnabled  = g_fgEnabled;
        g_fgEnabled      = *(const unsigned char*)((const char*)descV + 0x38) != 0;
        g_hudless.res    = rdPtr(descV, 0x40);
        g_hudless.format = rdU32(descV, 0x4C);
        g_hudless.width  = rdU32(descV, 0x50);
        g_hudless.height = rdU32(descV, 0x54);
        g_hudless.state  = rdU32(descV, 0x68);
        g_frameID        = rdU64(descV, 0x88);
        static int cfgLog = 0;
        if (cfgLog < 3 || wasEnabled != g_fgEnabled) {
            ++cfgLog;
            L("[CFG] FRAMEGEN configure: fgEnabled=%d hudlessRes=%p (%ux%u fmt=%u)", (int)g_fgEnabled, g_hudless.res, g_hudless.width, g_hudless.height, g_hudless.format);
        }
    }
    return 0;
}

extern "C" uint32_t slbridge_query(void* handle, void* descV)
{
    BridgeCtx* c = (BridgeCtx*)handle;
    FfxHeader* h = (FfxHeader*)descV;
    if (!c || !h) return 0;
    if (h->type == T_SWAPCHAIN_QCMD) {
        // Hand back a throwaway command list for the game to record FG into.
        FfxQInterpCmdList* q = (FfxQInterpCmdList*)descV;
        if (c->dummyList) {
            c->dummyList->Close();            // ensure closed (ignore error if already)
            if (c->dummyAlloc) c->dummyAlloc->Reset();
            c->dummyList->Reset(c->dummyAlloc, nullptr);
            c->listOpen = true;
            if (q->pOutCommandList) *q->pOutCommandList = c->dummyList;
        }
        return 0;
    }
    if (h->type == T_SWAPCHAIN_QTEX) {
        FfxQInterpTexture* q = (FfxQInterpTexture*)descV;
        if (q->pOutTexture) {
            q->pOutTexture->resource    = c->dummyTex;
            q->pOutTexture->description = c->texDesc;
            q->pOutTexture->state       = 1; // COMMON
        }
        return 0;
    }
    return 0;
}

extern "C" uint32_t slbridge_dispatch(void* handle, const void* descV)
{
    (void)handle;
    const FfxHeader* h = (const FfxHeader*)descV;
    if (h && h->type == T_FG_PREPARE) feedDLSSG(descV);

    if (h && h->type == T_FG_DISPATCH) {
        // (a) Set the swapchain color space once from the backbuffer transfer function.
        if (!g_csSet && g_scCtx && g_scCtx->sc4) {
            uint32_t tf = rdU32(descV, 0x110);
            float lumMin = rdF32(descV, 0x114), lumMax = rdF32(descV, 0x118);
            DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            if      (tf == 1) cs = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            else if (tf == 2) cs = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            HRESULT hr = g_scCtx->sc4->SetColorSpace1(cs);
            L("[CS] backbufferTransferFunction=%u lum=[%.4f,%.1f] -> SetColorSpace1(%d) hr=0x%08lx",
              tf, lumMin, lumMax, (int)cs, (unsigned long)hr);
            g_csSet = true;
        }
        // (b) Optional HUDLessColor + UIColorAndAlpha tags (fsr2dlss.ini HudlessTags=true).
        // This game BAKES UI into presentColor (no separate hudless), so this is best-effort:
        // it tags presentColor as HUDLessColor + an empty UI layer. May not reduce UI ghosting
        // for baked-UI titles; left off by default. Works on both the 2.7.x (slSetTag) and
        // 2.7.30+ (slSetTagForFrame) paths.
        if (g_cfg.hudlessTags && g_curToken) {
            void*    pcRes = rdPtr(descV, 0x18);
            uint32_t pcFmt = rdU32(descV, 0x24), pcW = rdU32(descV, 0x28), pcH = rdU32(descV, 0x2C), pcState = rdU32(descV, 0x40);
            void*    cmdList = rdPtr(descV, 0x10);
            if (pcRes) {
                sl::Resource hud = makeSlTex(pcRes, pcState, pcW, pcH, pcFmt);
                sl::Extent hudExt{0, 0, pcW, pcH};
                sl::ResourceTag tags[2];
                int nt = 0;
                tags[nt++] = sl::ResourceTag(&hud, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &hudExt);
                sl::Resource ui;
                sl::Extent uiExt{0, 0, g_scCtx ? g_scCtx->dispW : pcW, g_scCtx ? g_scCtx->dispH : pcH};
                if (g_scCtx && g_scCtx->uiTex) {
                    ui = sl::Resource(sl::ResourceType::eTex2d, g_scCtx->uiTex, (uint32_t)D3D12_RESOURCE_STATE_COMMON);
                    ui.width = g_scCtx->dispW; ui.height = g_scCtx->dispH; ui.mipLevels = 1; ui.arrayLayers = 1;
                    tags[nt++] = sl::ResourceTag(&ui, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &uiExt);
                }
                sl::ViewportHandle vp{0};
                sl::Result tr = p_slSetTagForFrame ? p_slSetTagForFrame(*g_curToken, vp, tags, nt, (void*)cmdList)
                                                   : p_slSetTag(vp, tags, nt, (sl::CommandBuffer*)cmdList);
                g_lastHudTagged = (tr == sl::Result::eOk);
            }
        }
    }
    return 0;
}

extern "C" void slbridge_destroy(void* handle)
{
    BridgeCtx* c = (BridgeCtx*)handle;
    if (!c || c->magic != CTX_MAGIC) return;
    L("[BR] destroy ctx=%p kind=%d", (void*)c, (int)c->kind);
    unregisterCtx(c);
    if (c->dummyList)  c->dummyList->Release();
    if (c->dummyAlloc) c->dummyAlloc->Release();
    if (c->dummyTex)   c->dummyTex->Release();
    if (c->uiTex)      c->uiTex->Release();
    if (c->sc4)        c->sc4->Release();
    c->magic = 0;
    delete c;
}

static const char* resStr(sl::Result r)
{
    switch (r) {
        case sl::Result::eOk: return "eOk";
        case sl::Result::eErrorDriverOutOfDate: return "eErrorDriverOutOfDate";
        case sl::Result::eErrorOSOutOfDate: return "eErrorOSOutOfDate";
        case sl::Result::eErrorOSDisabledHWS: return "eErrorOSDisabledHWS";
        case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
        case sl::Result::eErrorAdapterNotSupported: return "eErrorAdapterNotSupported";
        case sl::Result::eErrorMissingProxy: return "eErrorMissingProxy";
        case sl::Result::eErrorNotInitialized: return "eErrorNotInitialized";
        case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported";
        case sl::Result::eErrorFeatureMissing: return "eErrorFeatureMissing";
        case sl::Result::eErrorInvalidParameter: return "eErrorInvalidParameter";
        case sl::Result::eErrorUnsupportedInterface: return "eErrorUnsupportedInterface";
        case sl::Result::eErrorMissingInputParameter: return "eErrorMissingInputParameter";
        default: return "eError(other)";
    }
}
