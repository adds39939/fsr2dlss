// fsr2dlss.ini reader/writer. See config.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include "config.h"

Config g_cfg;

extern "C" void bridge_logs(const char* s);   // proxy.cpp logger (no-op unless Debug=true)
static void CL(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n] = '\n'; buf[n + 1] = 0;
    bridge_logs(buf);
}

// ---- mod directory ------------------------------------------------------------------------------
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
void config_mod_path(const wchar_t* name, wchar_t* out, size_t cch)
{
    computeModDir();
    _snwprintf(out, cch, L"%s%s", g_modDir, name);
    out[cch - 1] = 0;
}

// ---- name tables --------------------------------------------------------------------------------
struct KeyName { int vk; const char* name; };
static const KeyName kKeys[] = {
    { VK_HOME, "Home" }, { VK_END, "End" }, { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" },
    { VK_PAUSE, "Pause" }, { VK_SCROLL, "ScrollLock" }, { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" },
    { VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" }, { VK_F5, "F5" }, { VK_F6, "F6" },
    { VK_F7, "F7" }, { VK_F8, "F8" }, { VK_F9, "F9" }, { VK_F10, "F10" }, { VK_F11, "F11" }, { VK_F12, "F12" },
    { VK_OEM_3, "Tilde" }, { VK_BACK, "Backspace" }, { VK_TAB, "Tab" },
};
const char* config_key_name(int vk)
{
    for (const KeyName& k : kKeys) if (k.vk == vk) return k.name;
    static char buf[16];
    snprintf(buf, sizeof(buf), "0x%02X", vk & 0xff);
    return buf;
}
int config_key_from_name(const char* s)
{
    if (!s || !*s) return 0;
    for (const KeyName& k : kKeys) if (!_stricmp(s, k.name)) return k.vk;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { int v = (int)strtol(s + 2, nullptr, 16); return (v > 0 && v < 256) ? v : 0; }
    if (isdigit((unsigned char)s[0])) { int v = atoi(s); return (v > 0 && v < 256) ? v : 0; }
    return 0;
}

static const char* kSrQualityNames[SR_QUALITY_COUNT] = { "auto", "dlaa", "quality", "balanced", "performance", "ultraperformance" };
const char* config_sr_quality_name(int q) { return (q >= 0 && q < SR_QUALITY_COUNT) ? kSrQualityNames[q] : "auto"; }
static int srQualityFromName(const char* s)
{
    for (int i = 0; i < SR_QUALITY_COUNT; ++i) if (!_stricmp(s, kSrQualityNames[i])) return i;
    if (!_stricmp(s, "ultra_performance") || !_stricmp(s, "ultra performance")) return SR_ULTRA_PERFORMANCE;
    return SR_AUTO;
}

// sl::DLSSPreset values that still select a model in 310.x. Everything else maps back to default.
struct PresetName { int value; const char* name; };
static const PresetName kPresets[] = {
    { 0, "default" }, { 10, "J" }, { 11, "K" }, { 12, "L" }, { 13, "M" }, { 5, "E" }, { 6, "F" },
};
const char* config_sr_preset_name(int p)
{
    for (const PresetName& k : kPresets) if (k.value == p) return k.name;
    return "default";
}
static int srPresetFromName(const char* s)
{
    for (const PresetName& k : kPresets) if (!_stricmp(s, k.name)) return k.value;
    if (isdigit((unsigned char)s[0])) { int v = atoi(s); for (const PresetName& k : kPresets) if (k.value == v) return v; }
    return 0;
}

static const char* kFgModeNames[FG_MODE_COUNT] = { "off", "fixed", "dynamic" };
const char* config_fg_mode_name(int m) { return (m >= 0 && m < FG_MODE_COUNT) ? kFgModeNames[m] : "fixed"; }
static int fgModeFromName(const char* s)
{
    for (int i = 0; i < FG_MODE_COUNT; ++i) if (!_stricmp(s, kFgModeNames[i])) return i;
    return FG_FIXED;
}

// ---- parsing ------------------------------------------------------------------------------------
static char* trimStr(char* s)
{
    while (*s==' '||*s=='\t'||*s=='\r'||*s=='\n') ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e = 0;
    return s;
}
static bool cfgBool(const char* v) { return !_stricmp(v,"true")||!strcmp(v,"1")||!_stricmp(v,"on")||!_stricmp(v,"yes"); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void config_load()
{
    if (g_cfg.loaded) return;
    g_cfg.loaded = true;
    wchar_t path[MAX_PATH]; config_mod_path(L"fsr2dlss.ini", path, MAX_PATH);
    FILE* f = _wfopen(path, L"rb");
    if (!f) { CL("[CFG] no fsr2dlss.ini next to the mod; using defaults"); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* c = strpbrk(line, ";#"); if (c) *c = 0;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char* key = trimStr(line);
        char* val = trimStr(eq + 1);
        for (char* p = key; *p; ++p) *p = (char)tolower((unsigned char)*p);
        if      (!strcmp(key,"multiplier"))       g_cfg.fgMultiplier = clampi(atoi(val), 2, 4);
        else if (!strcmp(key,"mode"))             g_cfg.fgMode = fgModeFromName(val);
        else if (!strcmp(key,"dynamictargetfps")) { float v = (float)atof(val); g_cfg.fgDynamicFps = (v >= 0 && v < 1000) ? v : 0; }
        else if (!strcmp(key,"flipmetering"))     g_cfg.flipMode = !_stricmp(val,"hardware")?1 : (!_stricmp(val,"auto")?2:0);
        else if (!strcmp(key,"dlss"))             g_cfg.dlssSR = cfgBool(val);
        else if (!strcmp(key,"quality"))          g_cfg.srQuality = srQualityFromName(val);
        else if (!strcmp(key,"preset"))           g_cfg.srPreset = srPresetFromName(val);
        else if (!strcmp(key,"steamoverlayfix"))  g_cfg.steamFix = cfgBool(val);
        else if (!strcmp(key,"enabled"))          g_cfg.overlayOn = cfgBool(val);
        else if (!strcmp(key,"togglekey"))        { int k = config_key_from_name(val); if (k) g_cfg.overlayKey = k; }
        else if (!strcmp(key,"showfps"))          g_cfg.showFps = cfgBool(val);
        else if (!strcmp(key,"fpscorner"))        g_cfg.fpsCorner = clampi(atoi(val), 0, 3);
        else if (!strcmp(key,"scale"))            { float v = (float)atof(val); if (v >= 0.5f && v <= 3.0f) g_cfg.overlayScale = v; }
        else if (!strcmp(key,"brightnessnits"))   { float v = (float)atof(val); if (v >= 50.0f && v <= 1000.0f) g_cfg.overlayNits = v; }
        else if (!strcmp(key,"hudlesstags"))      g_cfg.hudlessTags = cfgBool(val);
        else if (!strcmp(key,"debug"))            g_cfg.debug = cfgBool(val);
        else if (!strcmp(key,"legacytagging"))    g_cfg.legacyTagging = cfgBool(val);
    }
    fclose(f);
    CL("[CFG] fsr2dlss.ini: fgMode=%s multiplier=%dx dynFps=%.0f flipMetering=%d dlss=%d quality=%s preset=%s steamFix=%d overlay=%d key=%s showFps=%d hudlessTags=%d legacyTagging=%d",
       config_fg_mode_name(g_cfg.fgMode), g_cfg.fgMultiplier, g_cfg.fgDynamicFps, g_cfg.flipMode, (int)g_cfg.dlssSR,
       config_sr_quality_name(g_cfg.srQuality), config_sr_preset_name(g_cfg.srPreset), (int)g_cfg.steamFix,
       (int)g_cfg.overlayOn, config_key_name(g_cfg.overlayKey), (int)g_cfg.showFps, (int)g_cfg.hudlessTags, (int)g_cfg.legacyTagging);
}

// ---- writing ------------------------------------------------------------------------------------
static const char* b2s(bool b) { return b ? "true" : "false"; }

bool config_save()
{
    wchar_t path[MAX_PATH]; config_mod_path(L"fsr2dlss.ini", path, MAX_PATH);
    wchar_t tmp[MAX_PATH];  config_mod_path(L"fsr2dlss.ini.tmp", tmp, MAX_PATH);
    FILE* f = _wfopen(tmp, L"wb");
    if (!f) { CL("[CFG] save failed: cannot open %ls", tmp); return false; }
    const Config& c = g_cfg;
    fprintf(f,
        "; fsr2dlss configuration. Lives next to amd_fidelityfx_dx12.dll (the game's Win64 folder).\n"
        "; Lines starting with ; or # are comments. Read once at startup; the in-game overlay (press the\n"
        "; ToggleKey, Home by default) edits these values live and rewrites this file when you click Save.\n"
        "\n"
        "[FrameGen]\n"
        "; Frame-generation mode:\n"
        ";   fixed   = DLSS-G with the Multiplier below (2x, 3x, 4x). Recommended.\n"
        ";   dynamic = Dynamic Multi Frame Generation: DLSS-G picks the multiplier every frame to hit\n"
        ";             DynamicTargetFps (0 = your display's refresh rate). Needs driver/hardware support;\n"
        ";             falls back to fixed when the card does not report it.\n"
        ";   off     = no frame generation; the game's real frames are presented as-is.\n"
        "Mode = %s\n"
        "\n"
        "; Frame-generation multiplier: 2 = 2x, 3 = 3x, 4 = 4x. 3x and 4x are Multi-Frame Generation and\n"
        "; need an RTX 50-series card. The value is clamped down to whatever the card reports as its maximum.\n"
        "Multiplier = %d\n"
        "\n"
        "; Target framerate for Mode = dynamic. 0 = auto (display refresh rate).\n"
        "DynamicTargetFps = %.0f\n"
        "\n"
        "; Flip metering (takes effect at the next game start):\n"
        ";   hardware = the Blackwell display engine paces the generated frames (true 3x/4x MFG). Recommended.\n"
        ";   software = block NvAPI_D3D12_SetFlipConfig so DLSS-G uses its software pacer (clean 2x only).\n"
        "FlipMetering = %s\n"
        "\n"
        "[Upscaling]\n"
        "; DLSS super-resolution in place of the game's FSR upscaler.\n"
        ";   true  = DLSS.   false = leave the upscale as FSR.\n"
        "DLSS = %s\n"
        "\n"
        "; DLSS quality mode. auto picks the mode matching the in-game FSR preset / render resolution\n"
        "; (native = DLAA). Otherwise one of: dlaa, quality, balanced, performance, ultraperformance.\n"
        "; The render resolution is still whatever the game renders at; this only selects the DLSS model.\n"
        "Quality = %s\n"
        "\n"
        "; DLSS model preset: default (NVIDIA's choice for the mode), or a fixed model letter:\n"
        ";   K = transformer, best quality (default for DLAA/Quality/Balanced)\n"
        ";   J = transformer, slightly less ghosting, a little more flicker\n"
        ";   L = sharper/more stable than J/K, heavier (default for Ultra Performance)\n"
        ";   M = like L but closer to J/K in cost (default for Performance)\n"
        ";   E, F = legacy CNN models (deprecated)\n"
        "Preset = %s\n"
        "\n"
        "[Overlay]\n"
        "; Make the Steam overlay report \"DLSS\" with the correct native/generated frame counts. Only\n"
        "; affects what Steam displays. Set false and Steam will call it \"FSR\".\n"
        "SteamOverlayFix = %s\n"
        "\n"
        "; In-game configuration overlay.\n"
        "Enabled = %s\n"
        "; Key that opens/closes it: Home, End, Insert, Delete, PageUp, PageDown, Pause, ScrollLock,\n"
        "; F1..F12, Tilde, or a hex virtual-key code such as 0x24.\n"
        "ToggleKey = %s\n"
        "; FPS counter showing the displayed framerate and the base (pre-frame-generation) framerate.\n"
        "ShowFps = %s\n"
        "; FPS counter position: 0 = top-left, 1 = top-right, 2 = bottom-left, 3 = bottom-right.\n"
        "FpsCorner = %d\n"
        "; Overlay UI scale (0.5 .. 3.0) and paper-white brightness in nits on HDR displays (50 .. 1000).\n"
        "Scale = %.2f\n"
        "BrightnessNits = %.0f\n"
        "\n"
        "[HUD]\n"
        "; Experimental. Tag a HUD-less color and UI layer for DLSS-G. Lies of P bakes the UI into the\n"
        "; frame, so this does not help there. Leave off.\n"
        "HudlessTags = %s\n"
        "\n"
        "[Logging]\n"
        "; Write ffx_bridge.log next to the mod. Off by default so a normal install adds no extra files.\n"
        "Debug = %s\n"
        "\n"
        "[Compat]\n"
        "; Troubleshooting only. true = tag resources through the deprecated global slSetTag path and\n"
        "; report host SDK 2.7.4 to Streamline (the behaviour of fsr2dlss builds before Streamline 2.14).\n"
        "LegacyTagging = %s\n",
        config_fg_mode_name(c.fgMode), c.fgMultiplier, c.fgDynamicFps,
        c.flipMode == 1 ? "hardware" : (c.flipMode == 2 ? "auto" : "software"),
        b2s(c.dlssSR), config_sr_quality_name(c.srQuality), config_sr_preset_name(c.srPreset),
        b2s(c.steamFix), b2s(c.overlayOn), config_key_name(c.overlayKey), b2s(c.showFps), c.fpsCorner,
        c.overlayScale, c.overlayNits, b2s(c.hudlessTags), b2s(c.debug), b2s(c.legacyTagging));
    bool ok = (fclose(f) == 0);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) { CL("[CFG] save failed: err=%lu", GetLastError()); DeleteFileW(tmp); }
    else CL("[CFG] saved %ls", path);
    return ok;
}
