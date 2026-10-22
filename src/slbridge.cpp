// ============================================================================
// Streamline bridge. This is where the FfxApi proxy hands off to NVIDIA
// Streamline: it substitutes the FSR3 frame-generation swapchain with a
// DLSS-G swapchain, runs DLSS super-resolution on the upscale dispatch, feeds
// DLSS-G depth/motion-vectors/camera each frame, and drives the Reflex/PCL
// marker loop that paces the generated frames.
//
// Built against the Streamline 2.14 SDK headers (third_party/streamline). The
// runtime plugins and the DLSS models ship with the release package.
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
#include <cmath>

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"
#include "sl_pcl.h"
#include "sl_helpers.h"
#include "slbridge.h"
#include "config.h"
#include "overlay.h"

extern "C" void bridge_logs(const char* s);
extern "C" bool nvhook_install();             // nvhook.cpp: hooks nvapi_QueryInterface to control flip metering
extern "C" void nvhook_set_flip_block(bool);  // hide SetFlipConfig (software pacer) or not
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
static const char* resStr(sl::Result r) { return sl::getResultAsStr(r); }

BridgeStats g_stats;

// Not reentrant; use the returned pointer immediately.
static const wchar_t* modPath(const wchar_t* name)
{
    static wchar_t buf[MAX_PATH];
    config_mod_path(name, buf, MAX_PATH);
    return buf;
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
static PFun_slSetTag*                 p_slSetTag;            // deprecated global tagging (Compat LegacyTagging=true)
static PFun_slSetTagForFrame*         p_slSetTagForFrame;    // frame-based tagging (default)
static PFun_slSetConstants*           p_slSetConstants;
static PFun_slGetNewFrameToken*       p_slGetNewFrameToken;
static PFun_slSetFeatureLoaded*       p_slSetFeatureLoaded;
static PFun_slEvaluateFeature*        p_slEvaluateFeature;
// feature funcs
static PFun_slDLSSGSetOptions*        p_slDLSSGSetOptions;
static PFun_slDLSSGGetState*          p_slDLSSGGetState;
static PFun_slReflexSetOptions*       p_slReflexSetOptions;
static PFun_slReflexSleep*            p_slReflexSleep;
static PFun_slPCLSetMarker*           p_slPCLSetMarker;      // per-frame latency markers live in sl.pcl
static PFun_slDLSSSetOptions*         p_slDLSSSetOptions;

static bool     g_frameTagging = false;      // slSetTagForFrame + eUseFrameBasedResourceTagging in use
static uint32_t g_srFrameIdx  = 0x40000000;  // SR frame-token space when it cannot share DLSS-G's index
static uint32_t g_srLastOutW  = 0, g_srLastOutH = 0; static int g_srLastMode = -1; static uint32_t g_srLastGen = 0xFFFFFFFF;
static bool     g_srDisabled  = false;

static sl::FrameToken* g_curToken = nullptr;   // current DLSS-G frame token, shared with the Present hook
static volatile LONG  g_presentMarks = 0;      // # present-marker pairs emitted by the hook
static unsigned long  g_presentTid   = 0;      // thread that calls Present (should == feedTid)
static bool           g_hookLive     = false;  // has the game ever called our hooked Present?

static bool          g_slReady   = false;
static ID3D12Device* g_device    = nullptr;

// ---- per-frame feeding state -----------------------------------------------
struct FfxResourceLite { void* res; uint32_t format, width, height, state; };
static FfxResourceLite g_hudless = {};   // from FRAMEGEN configure 0x00020002
struct BridgeCtx;                        // fwd
static BridgeCtx* g_scCtx = nullptr;     // the active SL swapchain context
static ID3D12CommandQueue* g_queue = nullptr; // the game's present queue (from NEW_DX12)
static bool     g_fgEnabled = false;     // the game's FG toggle (FRAMEGEN configure)
static uint64_t g_frameID   = 0;
static unsigned g_frameCount = 0;
static bool     g_diagShowInterpOnly = false; // DIAGNOSTIC: present only generated frames
static uint32_t g_maxPresented   = 0;         // max numFramesActuallyPresented ever seen
static uint32_t g_doubledFrames  = 0;         // # frames where presented >= 2
static int      g_lastAppliedMode = -1;       // last DLSSGMode applied (-1 = never)
static uint32_t g_lastAppliedGen  = 0;        // last numFramesToGenerate applied
static uint32_t g_frameIdx       = 0;         // monotonic SL frame index (one token per frame)
static uint32_t g_curFrameID     = 0xFFFFFFFF; // game's frameID for the current SL token
static unsigned g_presentCount   = 0;         // presents seen (warmup gate for the Reflex sleep)
static const unsigned WARMUP_PRESENTS = 120;  // skip the throttling sleep during fragile SL/swapchain init
static bool     g_lastAppliedInterp = false;  // last interp-only flag applied
static uint32_t g_dlssgMaxGen = 1;            // numFramesToGenerateMax reported by the card (1 = 2x only)
static bool     g_dlssgDynamicOk = false;     // bIsDynamicMFGSupported
static uint32_t g_dlssgStatus = 0;
static uint64_t g_dlssgVram = 0;
static uint32_t g_lastPresented = 0;          // numFramesActuallyPresented from the last state query
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

// Tag resources for a frame. Frame-based tagging (slSetTagForFrame) is the current Streamline API;
// the global slSetTag path is kept behind Compat LegacyTagging for troubleshooting. The two are
// mutually exclusive per slInit (eUseFrameBasedResourceTagging), so the choice is fixed at init.
static sl::Result setTags(sl::FrameToken* tok, const sl::ViewportHandle& vp, const sl::ResourceTag* tags, uint32_t n, void* cmdList)
{
    if (g_frameTagging) {
        if (!tok) return sl::Result::eErrorMissingInputParameter;
        return p_slSetTagForFrame(*tok, vp, tags, n, (sl::CommandBuffer*)cmdList);
    }
    if (!p_slSetTag) return sl::Result::eErrorNotInitialized;
    return p_slSetTag(vp, tags, n, (sl::CommandBuffer*)cmdList);
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
enum CtxKind { CTX_SWAPCHAIN = 1, CTX_FRAMEGEN = 2 };
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
    config_load();
    L("[SL] init begin dir=%ls device=%p (SDK headers %d.%d.%d)", slDir, deviceV, SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
    L("[CFG] fgMode=%s multiplier=%dx dynFps=%.0f flipMetering=%d dlss=%d quality=%s preset=%s steamFix=%d overlay=%d key=%s showFps=%d hudlessTags=%d legacyTagging=%d",
      config_fg_mode_name(g_cfg.fgMode), g_cfg.fgMultiplier, g_cfg.fgDynamicFps, g_cfg.flipMode, (int)g_cfg.dlssSR,
      config_sr_quality_name(g_cfg.srQuality), config_sr_preset_name(g_cfg.srPreset), (int)g_cfg.steamFix,
      (int)g_cfg.overlayOn, config_key_name(g_cfg.overlayKey), (int)g_cfg.showFps, (int)g_cfg.hudlessTags, (int)g_cfg.legacyTagging);

    // Flip metering. With FlipMetering=hardware (the shipped default) we leave SetFlipConfig alone,
    // and Blackwell's hardware flip queue paces the generated frames for true 3x/4x MFG. With
    // software/auto we hide SetFlipConfig before slInit so DLSS-G uses its software pacer, which
    // tops out at a clean 2x. Decide the block, then install the nvapi hook.
    nvhook_set_flip_block(g_cfg.flipMode != 1);   // hardware -> leave it; software/auto -> hide it
    nvhook_install();
    if (g_cfg.flipMode == 1) L("[SL] FlipMetering=hardware (hardware flip queue paces generated frames)");

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
    p_slSetTagForFrame         = (PFun_slSetTagForFrame*)        GP(h, "slSetTagForFrame");
    p_slSetConstants           = (PFun_slSetConstants*)          GP(h, "slSetConstants");
    p_slGetNewFrameToken       = (PFun_slGetNewFrameToken*)      GP(h, "slGetNewFrameToken");
    p_slSetFeatureLoaded       = (PFun_slSetFeatureLoaded*)      GP(h, "slSetFeatureLoaded");
    p_slEvaluateFeature        = (PFun_slEvaluateFeature*)       GP(h, "slEvaluateFeature");
    if (!p_slInit || !p_slSetD3DDevice || !p_slUpgradeInterface || !p_slGetNewFrameToken || !p_slSetConstants) { L("[SL] missing core exports"); return false; }

    g_frameTagging = !g_cfg.legacyTagging && p_slSetTagForFrame != nullptr;
    if (!g_frameTagging && !p_slSetTag) { L("[SL] neither slSetTagForFrame nor slSetTag exported"); return false; }
    g_stats.frameTagging = g_frameTagging;

    static const sl::Feature feats[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    sl::Preferences pref{};
    pref.logLevel          = g_cfg.debug ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins    = &slDir;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = slDir;
    pref.featuresToLoad    = feats;
    pref.numFeaturesToLoad = 4;
    // Manual hooking only. Do NOT set eDisableCLStateTracking: DLSS super-resolution is evaluated on the
    // game's own command list mid-frame, and the game keeps recording on it afterwards, so SL must
    // restore the command list state after slEvaluateFeature.
    pref.flags             = sl::PreferenceFlags::eUseManualHooking;
    // Frame-based resource tagging is the Streamline 2.7.30+ model: every tag is bound to a frame
    // token, so DLSS-G reads exactly the buffers tagged for the frame it is presenting. We report the
    // SDK version we compile against. LegacyTagging reproduces the pre-2.14 builds: global slSetTag and
    // host SDK 2.7.4 (that pairing used the global tag path and v5 DLSSGOptions).
    uint64_t hostVer = sl::kSDKVersion;
    if (g_frameTagging) pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    else                hostVer = ((uint64_t)2 << 48) | ((uint64_t)7 << 32) | ((uint64_t)4 << 16) | sl::kSDKVersionMagic;
    pref.engine            = sl::EngineType::eCustom;
    pref.engineVersion     = "1.0.0";
    pref.projectId         = "{E57EBA73-277D-4D76-8C29-60AB8155FA36}";
    pref.applicationId     = 141959980;
    pref.renderAPI         = sl::RenderAPI::eD3D12;

    sl::Result r = p_slInit(pref, hostVer);
    L("[SL] slInit(host=%u.%u.%u, frameTagging=%d) -> %d (%s)", (unsigned)(hostVer >> 48), (unsigned)((hostVer >> 32) & 0xffff), (unsigned)((hostVer >> 16) & 0xffff), (int)g_frameTagging, (int)r, resStr(r));
    if (r != sl::Result::eOk) return false;
    g_stats.slVersion[0] = SL_VERSION_MAJOR; g_stats.slVersion[1] = SL_VERSION_MINOR; g_stats.slVersion[2] = SL_VERSION_PATCH;
    r = p_slSetD3DDevice(deviceV);
    L("[SL] slSetD3DDevice -> %d (%s)", (int)r, resStr(r));

    LUID luid = g_device->GetAdapterLuid();
    sl::AdapterInfo ai{}; ai.deviceLUID = (uint8_t*)&luid; ai.deviceLUIDSizeInBytes = sizeof(luid);
    sl::Result rg = p_slIsFeatureSupported(sl::kFeatureDLSS_G, ai);
    L("[SL] DLSS_G supported -> %d (%s)", (int)rg, resStr(rg));
    if (rg != sl::Result::eOk) {
        if (p_slGetFeatureRequirements) {
            sl::FeatureRequirements req{};
            if (p_slGetFeatureRequirements(sl::kFeatureDLSS_G, req) == sl::Result::eOk)
                L("[SL] DLSS_G requirements: flags=0x%x os=%u.%u.%u (need %u.%u.%u) driver=%u.%u.%u (need %u.%u.%u)", (unsigned)req.flags,
                  req.osVersionDetected.major, req.osVersionDetected.minor, req.osVersionDetected.build, req.osVersionRequired.major, req.osVersionRequired.minor, req.osVersionRequired.build,
                  req.driverVersionDetected.major, req.driverVersionDetected.minor, req.driverVersionDetected.build, req.driverVersionRequired.major, req.driverVersionRequired.minor, req.driverVersionRequired.build);
        }
        L("[SL] DLSS_G not supported; bridge disabled"); return false;
    }
    // Activate DLSS super-resolution too, so slGetFeatureFunction can hand back its options fn.
    sl::Result rd = p_slIsFeatureSupported(sl::kFeatureDLSS, ai);
    L("[SL] DLSS(SR) supported -> %d (%s)", (int)rd, resStr(rd));

    if (p_slGetFeatureFunction) {
        p_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)p_slDLSSGSetOptions);
        p_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState",   (void*&)p_slDLSSGGetState);
        p_slGetFeatureFunction(sl::kFeatureDLSS,   "slDLSSSetOptions",  (void*&)p_slDLSSSetOptions);
        p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions",(void*&)p_slReflexSetOptions);
        p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep",     (void*&)p_slReflexSleep);
        p_slGetFeatureFunction(sl::kFeaturePCL,    "slPCLSetMarker",    (void*&)p_slPCLSetMarker);
    }
    L("[SL] feature funcs: dlssgSetOpt=%p dlssgGetState=%p dlssSetOpt=%p pclMarker=%p reflexOpt=%p sleep=%p",
      (void*)p_slDLSSGSetOptions, (void*)p_slDLSSGGetState, (void*)p_slDLSSSetOptions, (void*)p_slPCLSetMarker, (void*)p_slReflexSetOptions, (void*)p_slReflexSleep);
    if (!p_slDLSSGSetOptions) { L("[SL] slDLSSGSetOptions missing; bridge disabled"); return false; }

    g_slReady = true;
    L("[SL] init complete, bridge ready");
    return true;
}

// ---- in-place Present/ResizeBuffers vtable hooks ----------------------------
// We hand the game Streamline's real swapchain rather than a COM wrapper: a wrapper
// pointer can break SL's swapchain identity and its flip-metering binding, and adds a
// COM-lifetime crash risk. Instead we detour Present/Present1 in place on the real
// swapchain so we can bracket each present with PCL markers (and draw the overlay),
// and ResizeBuffers/ResizeBuffers1 so the overlay drops its back-buffer work first.
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers1)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
static PFN_Present        g_origPresent        = nullptr;
static PFN_Present1       g_origPresent1       = nullptr;
static PFN_ResizeBuffers  g_origResizeBuffers  = nullptr;
static PFN_ResizeBuffers1 g_origResizeBuffers1 = nullptr;

// Emit one PCL marker on the given token.
static void mark(sl::PCLMarker m, sl::FrameToken* tok)
{
    if (!tok || !p_slPCLSetMarker) return;
    p_slPCLSetMarker(m, *tok);
}

// Current DLSS-G request from the configuration + the game's FG toggle.
// Returns the sl::DLSSGMode and fills numFramesToGenerate (clamped to the card's maximum).
static sl::DLSSGMode wantedFgMode(uint32_t& numGen)
{
    uint32_t want = (uint32_t)(g_cfg.fgMultiplier > 1 ? g_cfg.fgMultiplier - 1 : 1);
    if (g_dlssgMaxGen >= 1 && want > g_dlssgMaxGen) want = g_dlssgMaxGen;
    numGen = want;
    if (!g_fgEnabled || g_cfg.fgMode == FG_OFF) return sl::DLSSGMode::eOff;
    if (g_cfg.fgMode == FG_DYNAMIC && g_dlssgDynamicOk) return sl::DLSSGMode::eDynamic;
    return sl::DLSSGMode::eOn;
}

static sl::Result setDlssgOptions(sl::DLSSGMode mode, uint32_t numGen, bool interpOnly)
{
    if (!p_slDLSSGSetOptions) return sl::Result::eErrorNotInitialized;
    sl::DLSSGOptions o;   // kStructVersion5: numFramesToGenerate (MFG), queueParallelismMode, dynamicTargetFrameRate
    o.mode = mode;
    o.numFramesToGenerate = numGen;
    o.flags = interpOnly ? sl::DLSSGFlags::eShowOnlyInterpolatedFrame : sl::DLSSGFlags{};
    o.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;   // without it the generated frame is dropped
    o.enableUserInterfaceRecomposition = sl::Boolean::eFalse;
    o.dynamicTargetFrameRate = (mode == sl::DLSSGMode::eDynamic) ? g_cfg.fgDynamicFps : 0.0f;
    sl::ViewportHandle vp{0};
    return p_slDLSSGSetOptions(vp, o);
}

// Query DLSS-G state (present thread). Tracks the card's limits and the frames reaching the display.
static void queryDlssgState()
{
    if (!p_slDLSSGGetState) return;
    sl::ViewportHandle vp{0};
    sl::DLSSGState st;   // kStructVersion4: numFramesToGenerateMax, bIsDynamicMFGSupported
    if (p_slDLSSGGetState(vp, st, nullptr) != sl::Result::eOk) return;
    if (st.numFramesToGenerateMax >= 1 && st.numFramesToGenerateMax <= 8) g_dlssgMaxGen = st.numFramesToGenerateMax;
    g_dlssgDynamicOk = (st.bIsDynamicMFGSupported == sl::Boolean::eTrue);
    g_dlssgStatus = (uint32_t)st.status;
    g_dlssgVram = st.estimatedVRAMUsageInBytes;
    g_lastPresented = st.numFramesActuallyPresented;
    if (st.numFramesActuallyPresented > g_maxPresented) g_maxPresented = st.numFramesActuallyPresented;
    if (st.numFramesActuallyPresented >= 2) ++g_doubledFrames;
    g_stats.fgMaxGen = g_dlssgMaxGen; g_stats.fgDynamicOk = g_dlssgDynamicOk; g_stats.fgStatus = g_dlssgStatus; g_stats.fgVramBytes = g_dlssgVram;
}

// Framerate statistics for the overlay: real presents per second (the game presents once per
// rendered frame) and frames reaching the display per second (sum of numFramesActuallyPresented).
static void updateFpsStats()
{
    static ULONGLONG s_last = 0; static unsigned s_lastPresents = 0; static uint64_t s_lastShown = 0;
    static uint64_t s_shown = 0;
    s_shown += g_lastPresented;
    ULONGLONG now = GetTickCount64();
    if (!s_last) { s_last = now; s_lastPresents = g_presentCount; s_lastShown = s_shown; return; }
    ULONGLONG dt = now - s_last;
    if (dt < 500) return;
    float base = (g_presentCount - s_lastPresents) * 1000.0f / (float)dt;
    uint64_t shown = s_shown - s_lastShown;
    float disp = shown ? shown * 1000.0f / (float)dt : base;
    if (disp < base) disp = base;
    g_stats.baseFps = base; g_stats.displayFps = disp;
    s_last = now; s_lastPresents = g_presentCount; s_lastShown = s_shown;
}

// Reflex present loop, part 1: just before the real Present(), assert options and ePresentStart.
static void presentPre()
{
    g_presentTid = GetCurrentThreadId();
    if (!g_hookLive) { g_hookLive = true; L("[PRESENT] FIRST hooked present -> live (tid=%lu)", g_presentTid); }
    if (!g_curToken) return;
    // Re-assert DLSS-G options every frame on the present thread, right before Present. Streamline
    // requires the options be set on the presenting thread so it can bind them to the right Present.
    {
        uint32_t numGen = 1;
        sl::DLSSGMode mode = wantedFgMode(numGen);
        sl::Result orr = setDlssgOptions(mode, numGen, g_diagShowInterpOnly);
        if ((int)mode != g_lastAppliedMode || numGen != g_lastAppliedGen || g_diagShowInterpOnly != g_lastAppliedInterp) {
            L("[FGC] slDLSSGSetOptions(mode=%s numFramesToGenerate=%u interpOnly=%d) -> %d (%s)", sl::getDLSSGModeAsStr(mode), numGen, (int)g_diagShowInterpOnly, (int)orr, resStr(orr));
            g_lastAppliedMode = (int)mode; g_lastAppliedGen = numGen; g_lastAppliedInterp = g_diagShowInterpOnly;
        }
        g_stats.fgApplied = (mode == sl::DLSSGMode::eOff) ? 0 : (mode == sl::DLSSGMode::eDynamic ? 1 : numGen + 1);
    }
    // Reflex options before Present. useMarkersToOptimize stays false: this game is single-threaded
    // and emits no genuine sim/submit markers, so letting Reflex optimize off them corrupts the pacer
    // and drops the generated frame.
    if (p_slReflexSetOptions) {
        sl::ReflexOptions ro{}; ro.mode = sl::ReflexMode::eLowLatency; ro.frameLimitUs = g_frameLimitUs;
        ro.useMarkersToOptimize = false;
        p_slReflexSetOptions(ro);
    }
    // Close the render-submit interval, then open present (full marker cadence for the pacer).
    mark(sl::PCLMarker::eRenderSubmitEnd, g_curToken);
    mark(sl::PCLMarker::ePresentStart,    g_curToken);
    InterlockedIncrement(&g_presentMarks);
}
// Reflex present loop, part 2: just after the real Present() returns, which is the true frame
// boundary on this single-threaded render/present thread. The sleep has to happen here rather
// than mid-dispatch. We close the present interval, then sleep, which throttles the start of the
// next real frame and gives flip metering a steady cadence to meter against.
static void presentPost()
{
    ++g_presentCount;
    queryDlssgState();
    updateFpsStats();
    if (!g_curToken) return;
    mark(sl::PCLMarker::ePresentEnd, g_curToken);   // the token advances in feedDLSSG, keyed by the game's frameID
    if (g_presentCount >= WARMUP_PRESENTS && p_slReflexSleep) p_slReflexSleep(*g_curToken);
}
static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* This, UINT s, UINT f)
{
    overlay_render((IDXGISwapChain3*)This);   // overlay is render-submit work: before the markers
    presentPre();
    HRESULT hr = g_origPresent(This, s, f);
    presentPost();
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* This, UINT s, UINT f, const DXGI_PRESENT_PARAMETERS* p)
{
    overlay_render((IDXGISwapChain3*)This);
    presentPre();
    HRESULT hr = g_origPresent1(This, s, f, p);
    presentPost();
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* This, UINT c, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl)
{
    L("[SC] ResizeBuffers(%u, %ux%u, fmt=%u, flags=0x%x)", c, w, h, (unsigned)fmt, fl);
    overlay_before_resize();
    return g_origResizeBuffers(This, c, w, h, fmt, fl);
}
static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers1(IDXGISwapChain3* This, UINT c, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl, const UINT* nodes, IUnknown* const* q)
{
    L("[SC] ResizeBuffers1(%u, %ux%u, fmt=%u, flags=0x%x)", c, w, h, (unsigned)fmt, fl);
    overlay_before_resize();
    return g_origResizeBuffers1(This, c, w, h, fmt, fl, nodes, q);
}
// IDXGISwapChain vtable: Present=8, ResizeBuffers=13, IDXGISwapChain1::Present1=22,
// IDXGISwapChain3::ResizeBuffers1=39. Idempotent.
static void hookVtbl(void** vtbl, int idx, void* hook, void** orig)
{
    if (vtbl[idx] == hook) return;
    DWORD oldp;
    if (VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldp)) {
        *orig = vtbl[idx];
        vtbl[idx] = hook;
        VirtualProtect(&vtbl[idx], sizeof(void*), oldp, &oldp);
    }
}
static void hookSwapchain(IDXGISwapChain4* sc)
{
    void** vtbl = *(void***)sc;
    hookVtbl(vtbl, 8,  (void*)&Hook_Present,        (void**)&g_origPresent);
    hookVtbl(vtbl, 22, (void*)&Hook_Present1,       (void**)&g_origPresent1);
    hookVtbl(vtbl, 13, (void*)&Hook_ResizeBuffers,  (void**)&g_origResizeBuffers);
    hookVtbl(vtbl, 39, (void*)&Hook_ResizeBuffers1, (void**)&g_origResizeBuffers1);
    // Which module owns the original Present we detoured? If it's dxgi.dll (native) rather than
    // sl.interposer.dll, we're bypassing SL's DLSS-G present entirely -> no interpolation.
    HMODULE mod = nullptr; char modName[MAX_PATH] = {0};
    if (g_origPresent && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            (LPCSTR)g_origPresent, &mod) && mod) {
        GetModuleFileNameA(mod, modName, MAX_PATH);
    }
    L("[SC] hooked Present(orig=%p) Present1(orig=%p) ResizeBuffers(orig=%p) on sc4=%p  origPresentModule=%s",
      (void*)g_origPresent, (void*)g_origPresent1, (void*)g_origResizeBuffers, (void*)sc, modName[0] ? modName : "<unknown>");
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

    // Pass the game's present queue to CreateSwapChain as-is; Streamline's proxy swapchain owns
    // presentation, so there is no need to upgrade the queue.
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

    // Tell SL to load DLSS-G for this swapchain before creating it, so the proxy factory builds an
    // interpolating swapchain rather than a passthrough one.
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
        c->texDesc.format = (uint32_t)d1.Format; // store DXGI format; the game does not use it semantically
        if (FAILED(thr)) L("[SC] dummy interp texture create hr=0x%08lx", (unsigned long)thr);
    }
    // Transparent UIColorAndAlpha surface (R8G8B8A8 - 8-bit alpha). The game bakes UI into the
    // backbuffer so we feed an empty one when HudlessTags is on.
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
    hookSwapchain(sc4);
    sc4->AddRef();
    *outSwapchain = static_cast<IDXGISwapChain4*>(sc4);
    registerCtx(c);
    g_scCtx = c;
    g_queue = queue;
    L("[SC] substituted SL swapchain sc4=%p (handed to game, Present hooked in place) ctx=%p", (void*)sc4, (void*)c);

    // Leave DLSS-G OFF initially (passthrough); the present hook turns it on once the game enables FG.
    { sl::Result orr = setDlssgOptions(sl::DLSSGMode::eOff, 1, false); L("[SC] slDLSSGSetOptions(eOff) -> %d (%s)", (int)orr, resStr(orr)); }
    queryDlssgState();
    L("[SC] DLSS-G caps: numFramesToGenerateMax=%u (up to %ux) dynamicMFG=%d status=0x%x", g_dlssgMaxGen, g_dlssgMaxGen + 1, (int)g_dlssgDynamicOk, g_dlssgStatus);

    // In-game overlay on the game's present queue and this swapchain.
    if (g_cfg.overlayOn || g_cfg.showFps) overlay_init(g_device, queue, sc4, (void*)od->OutputWindow);
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

// ---- desc field readers (descs are valid game memory) -----------------------
static uint32_t rdU32(const void* b, size_t o){ return *(const uint32_t*)((const char*)b+o); }
static uint64_t rdU64(const void* b, size_t o){ return *(const uint64_t*)((const char*)b+o); }
static float    rdF32(const void* b, size_t o){ return *(const float*)((const char*)b+o); }
static void*    rdPtr(const void* b, size_t o){ return *(void* const*)((const char*)b+o); }

static const uint64_t T_FG_CONFIGURE = 0x00020002;
static const uint64_t T_FG_PREPARE   = 0x00020004;
static const uint64_t T_FG_DISPATCH  = 0x00020003;

// Frame start: called from the proxy on the first FfxApi call of the frame (UPSCALE dispatch).
extern "C" void slbridge_frame_start()
{
    // No-op: the frame token + all PCL markers are driven from feedDLSSG, keyed by the
    // game's frameID (the UPSCALE dispatch has no frameID to key on).
}

// Feed DLSS-G from the FRAMEGEN PREPARE dispatch (depth + MV + camera scalars).
static void feedDLSSG(const void* d)
{
    if (!g_slReady || !p_slDLSSGSetOptions || !p_slSetConstants || !p_slGetNewFrameToken) return;

    void*    cmdList = rdPtr(d, 0x20);
    uint32_t renderW = rdU32(d, 0x28), renderH = rdU32(d, 0x2C);
    float    jitX = rdF32(d, 0x30), jitY = rdF32(d, 0x34);
    float    camNear = rdF32(d, 0x48), camFar = rdF32(d, 0x4C), camFov = rdF32(d, 0x50);
    void*    depthRes = rdPtr(d, 0x58); uint32_t depthFmt=rdU32(d,0x64), depthW=rdU32(d,0x68), depthH=rdU32(d,0x6C), depthSt=rdU32(d,0x80);
    void*    mvRes    = rdPtr(d, 0x88); uint32_t mvFmt=rdU32(d,0x94), mvW=rdU32(d,0x98), mvH=rdU32(d,0x9C), mvSt=rdU32(d,0xB0);

    // DLSS-G requires the app to poll the current back-buffer index every frame.
    if (g_scCtx && g_scCtx->sc4) g_scCtx->sc4->GetCurrentBackBufferIndex();

    sl::ViewportHandle vp{0};
    ++g_frameCount;
    // Key the frame token by the game's frameID. The game runs PREPARE more than once per
    // presented frame, and SL forbids setting common constants twice for the same frame, so we
    // advance the token and feed only when the game's frameID changes.
    uint32_t gameFrameID = (uint32_t)rdU64(d, 0x10);
    if (gameFrameID == g_curFrameID) return;   // already fed this game frame
    {
        uint32_t idx = gameFrameID ? gameFrameID : ++g_frameIdx;
        sl::FrameToken* t = nullptr;
        if (p_slGetNewFrameToken(t, &idx) != sl::Result::eOk || !t) return;
        g_curToken = t; g_curFrameID = gameFrameID;
    }
    sl::FrameToken* token = g_curToken;
    g_feedTid = GetCurrentThreadId();
    // The DLSS-G pacer needs the full per-frame sim/render marker cadence.
    // eRenderSubmitEnd is emitted in presentPre just before the real Present.
    mark(sl::PCLMarker::eSimulationStart,   token);
    mark(sl::PCLMarker::eSimulationEnd,     token);
    mark(sl::PCLMarker::eRenderSubmitStart, token);

    sl::Resource depthR = makeSlTex(depthRes, depthSt, depthW, depthH, depthFmt);
    sl::Resource mvR    = makeSlTex(mvRes,    mvSt,    mvW,    mvH,    mvFmt);
    sl::Resource hudR;
    // Set the tag extents explicitly; leaving them {0,0,0,0} makes SL log an "invalid backbuffer
    // resource extent (0 x 0)" and reset every frame.
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
    {
        sl::Result tr = setTags(token, vp, tags, (uint32_t)nt, cmdList);
        static int tagErr = 0;
        if (tr != sl::Result::eOk && tagErr < 5) { ++tagErr; L("[FGC] tag (%s) -> %d (%s)", g_frameTagging ? "slSetTagForFrame" : "slSetTag", (int)tr, resStr(tr)); }
    }

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
    }

    // slDLSSGSetOptions is asserted in presentPre() every frame on the present thread, right before
    // the real Present, because SL only guarantees the updated options bind to the next Present when
    // they are set on the presenting thread. The state query lives in presentPost().
    g_stats.fgGameEnabled = g_fgEnabled;
    if (g_frameCount <= 3 || (g_frameCount % 240) == 5) {
        L("[FGC] f=%u baseFps=%.1f displayFps=%.1f feedTid=%lu fgEn=%d cfgMode=%s mult=%dx maxGen=%u dyn=%d status=0x%x presented=%u maxPres=%u dblFrames=%u vram=%lluMB render=%ux%u zNear=%.1f hudTagged=%d interpOnly=%d | hookLive=%d presentTid=%lu presentMarks=%ld",
          g_frameCount, g_stats.baseFps, g_stats.displayFps,
          g_feedTid, (int)g_fgEnabled, config_fg_mode_name(g_cfg.fgMode), g_cfg.fgMultiplier, g_dlssgMaxGen, (int)g_dlssgDynamicOk, g_dlssgStatus, g_lastPresented,
          g_maxPresented, g_doubledFrames, (unsigned long long)(g_dlssgVram >> 20), renderW, renderH,
          zNear, (int)(g_lastHudTagged), (int)g_diagShowInterpOnly,
          (int)g_hookLive, g_presentTid, (long)g_presentMarks);
    }
}

// ---- DLSS super-resolution: replace the FSR upscale dispatch (0x00010001) -----
// Returns true if DLSS-SR did the upscale (proxy then SKIPS forwarding to AMD's FSR); false to
// fall back to FSR. Reads the FfxApi upscale desc (offsets from CONTRACT.md), runs SL DLSS on the
// game's command list writing the upscaled result into the game's own output buffer, which the
// downstream DLSS-G/present path then consumes. Independent viewport (vp1) from FG.
static sl::DLSSMode srModeFromConfig(float ratio)
{
    switch (g_cfg.srQuality) {
        case SR_DLAA:              return sl::DLSSMode::eDLAA;
        case SR_QUALITY:           return sl::DLSSMode::eMaxQuality;
        case SR_BALANCED:          return sl::DLSSMode::eBalanced;
        case SR_PERFORMANCE:       return sl::DLSSMode::eMaxPerformance;
        case SR_ULTRA_PERFORMANCE: return sl::DLSSMode::eUltraPerformance;
        default: break;
    }
    // Auto: derive from the render:output ratio so any in-game FSR preset maps to the matching mode.
    if      (ratio > 0.92f) return sl::DLSSMode::eDLAA;
    else if (ratio > 0.62f) return sl::DLSSMode::eMaxQuality;
    else if (ratio > 0.54f) return sl::DLSSMode::eBalanced;
    else if (ratio > 0.45f) return sl::DLSSMode::eMaxPerformance;
    return sl::DLSSMode::eUltraPerformance;
}

extern "C" bool slbridge_upscale(const void* d)
{
    if (!g_slReady || !p_slGetNewFrameToken || !p_slSetConstants || !d) return false;
    static unsigned scan = 0;
    if ((scan++ % 120) == 0) g_srDisabled = (GetFileAttributesW(modPath(L"dlss_sr_off.flag")) != INVALID_FILE_ATTRIBUTES);
    if (g_srDisabled || !g_cfg.dlssSR) { g_stats.srActive = false; g_stats.srMode = -1; return false; }

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

    // Choose the DLSS mode and model preset; only re-set options on change (resolution, mode, or
    // the overlay editing the configuration).
    float ratio = (float)renderW / (float)outW;
    sl::DLSSMode mode = srModeFromConfig(ratio);
    uint32_t gen = g_cfg.generation;
    if (outW != g_srLastOutW || outH != g_srLastOutH || (int)mode != g_srLastMode || gen != g_srLastGen) {
        sl::DLSSOptions o;   // kStructVersion3
        o.mode = mode; o.outputWidth = outW; o.outputHeight = outH;
        o.colorBuffersHDR = sl::Boolean::eTrue;   // game renders HDR10 PQ
        o.useAutoExposure = sl::Boolean::eTrue;   // no separate exposure tag needed
        o.alphaUpscalingEnabled = sl::Boolean::eFalse;
        sl::DLSSPreset preset = (sl::DLSSPreset)g_cfg.srPreset;
        o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset = o.ultraQualityPreset = preset;
        sl::Result orr = p_slDLSSSetOptions(vp, o);
        L("[SR] slDLSSSetOptions mode=%s preset=%s render=%ux%u out=%ux%u ratio=%.2f -> %d (%s)", sl::getDLSSModeAsStr(mode), config_sr_preset_name(g_cfg.srPreset), renderW, renderH, outW, outH, ratio, (int)orr, resStr(orr));
        g_srLastOutW = outW; g_srLastOutH = outH; g_srLastMode = (int)mode; g_srLastGen = gen;
        g_stats.srMode = (int)mode; g_stats.srPreset = g_cfg.srPreset;
        g_stats.renderW = renderW; g_stats.renderH = renderH; g_stats.outW = outW; g_stats.outH = outH;
    }

    // Frame token for the upscale. With frame-based tagging the tags live under the token's frame
    // index, so SR uses the index DLSS-G will use for this frame (the game's next frameID): the SR
    // tags (viewport 1) and the FG tags (viewport 0) then share one frame entry and SR never evicts
    // a frame DLSS-G may still read. Before FG has run there is no frameID yet, so SR counts its own.
    // In legacy mode the Steam overlay fix keeps SR on its own slGetNewFrameToken stream, which is
    // what activates Steam's DLSS-G detector so it reports "DLSS" with the correct native count.
    sl::FrameToken* t = g_curToken;
    if (g_frameTagging || g_cfg.steamFix || !t) {
        // Share DLSS-G's index only while DLSS-G is actually being fed (frameID advanced since the last
        // upscale); otherwise (game FG off) count our own so constants are never set twice per index.
        static uint32_t s_lastSeenFgFrame = 0xFFFFFFFF;
        uint32_t idx;
        if (g_frameTagging && g_curFrameID != 0xFFFFFFFF && g_curFrameID != s_lastSeenFgFrame) { idx = g_curFrameID + 1; s_lastSeenFgFrame = g_curFrameID; }
        else idx = ++g_srFrameIdx;
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
    {
        sl::Result cr = p_slSetConstants(c, *t, vp);
        static int cErr = 0;
        if (cr != sl::Result::eOk && cErr < 5) { ++cErr; L("[SR] slSetConstants -> %d (%s)", (int)cr, resStr(cr)); }
    }

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
    {
        sl::Result tr = setTags(t, vp, tags, 4, cmdList);
        static int tErr = 0;
        if (tr != sl::Result::eOk && tErr < 5) { ++tErr; L("[SR] tag -> %d (%s)", (int)tr, resStr(tr)); }
    }

    const sl::BaseStructure* inputs[] = { (const sl::BaseStructure*)&vp };
    sl::Result er = p_slEvaluateFeature(sl::kFeatureDLSS, *t, inputs, 1, (sl::CommandBuffer*)cmdList);
    static int okLog = 0, errLog = 0;
    if (er == sl::Result::eOk) { if (okLog < 1) { ++okLog; L("[SR] DLSS upscale ACTIVE (render %ux%u -> %ux%u)", renderW, renderH, outW, outH); } }
    else if (errLog < 8)       { ++errLog; L("[SR] slEvaluateFeature(DLSS) -> %d (%s); falling back to FSR", (int)er, resStr(er)); }
    g_stats.srActive = (er == sl::Result::eOk);
    if (!g_stats.srActive) g_stats.srMode = -1;
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
        g_stats.fgGameEnabled = g_fgEnabled;
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
        // (a) Set the swapchain color space once from the backbuffer transfer function, and tell the
        //     overlay how to encode its pixels.
        if (!g_csSet && g_scCtx && g_scCtx->sc4) {
            uint32_t tf = rdU32(descV, 0x110);
            float lumMin = rdF32(descV, 0x114), lumMax = rdF32(descV, 0x118);
            DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            int ovlMode = 0;
            if      (tf == 1) { cs = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020; ovlMode = 1; }
            else if (tf == 2) { cs = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;    ovlMode = 2; }
            HRESULT hr = g_scCtx->sc4->SetColorSpace1(cs);
            L("[CS] backbufferTransferFunction=%u lum=[%.4f,%.1f] -> SetColorSpace1(%d) hr=0x%08lx",
              tf, lumMin, lumMax, (int)cs, (unsigned long)hr);
            overlay_set_colorspace(ovlMode);
            g_csSet = true;
        }
        // (b) Optional HUDLessColor + UIColorAndAlpha tags (fsr2dlss.ini HudlessTags=true).
        // This game bakes the UI into presentColor (no separate hudless buffer), so this is
        // best-effort: it tags presentColor as HUDLessColor plus an empty UI layer. It may not
        // reduce UI ghosting for baked-UI titles, so it is off by default.
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
                sl::Result tr = setTags(g_curToken, vp, tags, (uint32_t)nt, cmdList);
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
    if (c->kind == CTX_SWAPCHAIN) {
        overlay_shutdown();
        if (g_scCtx == c) { g_scCtx = nullptr; g_csSet = false; g_curToken = nullptr; }
    }
    if (c->dummyList)  c->dummyList->Release();
    if (c->dummyAlloc) c->dummyAlloc->Release();
    if (c->dummyTex)   c->dummyTex->Release();
    if (c->uiTex)      c->uiTex->Release();
    if (c->sc4)        c->sc4->Release();
    c->magic = 0;
    delete c;
}
