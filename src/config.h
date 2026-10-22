// fsr2dlss runtime configuration. One global `g_cfg`, loaded once from fsr2dlss.ini next to the
// mod DLL, edited live by the in-game overlay, and written back with config_save().
//
// Every consumer (proxy.cpp, slbridge.cpp, overlay.cpp, nvhook.cpp) reads the same struct. Plain
// ints/bools are written from the present thread (overlay) and read from the render thread; the
// values are independent and a torn read of one field is harmless, so no locking is used.
#pragma once
#include <cstdint>

// DLSS super-resolution quality override (Config::srQuality).
enum SrQuality : int {
    SR_AUTO = 0,        // derive from the game's render:output ratio (the in-game FSR preset)
    SR_DLAA,
    SR_QUALITY,
    SR_BALANCED,
    SR_PERFORMANCE,
    SR_ULTRA_PERFORMANCE,
    SR_QUALITY_COUNT
};

// DLSS-G mode (Config::fgMode).
enum FgMode : int {
    FG_OFF = 0,         // DLSS-G stays off; the game's real frames are presented as-is
    FG_FIXED,           // fixed multiplier (Config::fgMultiplier), 2x/3x/4x
    FG_DYNAMIC,         // Dynamic Multi Frame Generation (DLSSGMode::eDynamic), if the card supports it
    FG_MODE_COUNT
};

struct Config {
    // [FrameGen]
    int   fgMode        = FG_FIXED;
    int   fgMultiplier  = 3;        // 2..4, clamped to the card's numFramesToGenerateMax+1 at runtime
    float fgDynamicFps  = 0.0f;     // Dynamic MFG target fps; 0 = auto (display refresh rate)
    int   flipMode      = 1;        // 0 = software pacer (hide SetFlipConfig), 1 = hardware flip metering, 2 = auto
    // [Upscaling]
    bool  dlssSR        = true;     // false = pass the upscale through to the game's FSR
    int   srQuality     = SR_AUTO;  // SrQuality
    int   srPreset      = 0;        // sl::DLSSPreset numeric value: 0 default, 10 J, 11 K, 12 L, 13 M, 5 E, 6 F
    // [Overlay]
    bool  steamFix      = true;     // make the Steam overlay report DLSS instead of FSR
    bool  overlayOn     = true;     // in-game configuration overlay available on the toggle key
    int   overlayKey    = 0x24;     // VK_HOME
    bool  showFps       = true;     // FPS counter HUD (displayed and base framerate)
    int   fpsCorner     = 1;        // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    float overlayScale  = 1.0f;     // UI scale
    float overlayNits   = 200.0f;   // overlay paper-white on HDR displays
    // [HUD]
    bool  hudlessTags   = false;    // experimental HUDLessColor / UI tagging for DLSS-G
    // [Logging]
    bool  debug         = false;    // write ffx_bridge.log
    // [Compat]
    bool  legacyTagging = false;    // use the deprecated global slSetTag path and report host SDK 2.7.4

    // Bumped by whoever changes a value at runtime so consumers can re-apply options cheaply.
    volatile uint32_t generation = 0;
    bool  loaded        = false;
};

extern Config g_cfg;

// Load fsr2dlss.ini once (idempotent, thread-safe enough for first-call races: both load the same
// values). Missing file or keys keep the defaults.
void config_load();
// Write the whole configuration back to fsr2dlss.ini (with comments). Returns false on I/O error.
bool config_save();
// Absolute path of a file next to the mod DLL (e.g. L"fsr2dlss.ini"). Buffer is cch wide chars.
void config_mod_path(const wchar_t* name, wchar_t* out, size_t cch);

// Helpers shared by the overlay and the ini writer.
const char* config_key_name(int vk);            // "Home", "F10", ... or "0x24"
int         config_key_from_name(const char* s); // inverse; 0 if unknown
const char* config_sr_quality_name(int q);
const char* config_sr_preset_name(int p);
const char* config_fg_mode_name(int m);
