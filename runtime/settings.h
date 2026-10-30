// settings.h - teambuddies.ini, shared by the game and the launcher (no other runtime dependencies).
#pragma once
#include <string>

enum DisplayMode { DISPLAY_WINDOW = 0, DISPLAY_BORDERLESS, DISPLAY_FULLSCREEN };
enum AspectMode { ASPECT_4_3 = 0, ASPECT_16_9, ASPECT_STRETCH, ASPECT_WIDE, ASPECT_PIXELS, ASPECT_COUNT };
const int FRAME_RATE_MONITOR = -1;
// anti-aliasing (hardware renderer): FXAA filters the picture; MSAA smooths polygon edges; SSAA also shades
// every sample, so textures are smoothed too
enum AntiAlias { AA_OFF = 0, AA_FXAA, AA_MSAA2, AA_MSAA4, AA_MSAA8, AA_SSAA2, AA_SSAA4, AA_SSAA8, AA_COUNT };
extern const char* const k_aa_names[AA_COUNT]; // ini values
inline int aa_samples(int m) {
    return m == AA_MSAA2 || m == AA_SSAA2 ? 2 : m == AA_MSAA4 || m == AA_SSAA4 ? 4 : m == AA_MSAA8 || m == AA_SSAA8 ? 8 : 1;
}
inline bool aa_per_sample(int m) { return m >= AA_SSAA2 && m <= AA_SSAA8; }
// texture filtering (hardware renderer): the original's nearest texels, bilinear blending, or xBR, which
// redraws the pixel art's edges smooth at any size (letters, sprites, textures)
enum TexFilter { TEX_NEAREST = 0, TEX_BILINEAR, TEX_XBR, TEX_COUNT };
extern const char* const k_tex_filter_names[TEX_COUNT]; // ini values

struct Settings {
    std::string disc;          // path to the .cue (or .bin)
    int renderer = 1;          // 0 = software, 1 = hardware (Direct3D 11)
    int scale = 3;             // internal resolution multiplier for the hardware renderer
    int window_w = 1280, window_h = 960;
    int display = DISPLAY_WINDOW;
    bool vsync = true;
    int frame_rate = 0;        // 0 = the game's own (30 in gameplay); 60..500 = in-between frames drawn up to that rate;
                               // FRAME_RATE_MONITOR = the monitor's refresh rate (hardware renderer)
    int anti_aliasing = AA_OFF;
    bool widescreen = false;   // GTE widescreen hack
    int aspect = ASPECT_4_3;   // 16:9 stretches; wide fills the window's shape; pixels = whole-number scaling
    bool bilinear = false;     // smooth output when upscaling the final image
    int texture_filter = TEX_NEAREST;
    bool hd_text = true;       // the game's letters drawn from smooth high-resolution reconstructions (hardware renderer)
    bool true_color = true;    // skip 15-bit dithering in the hardware renderer
    double cpu_cpi = 2.0;      // emulated CPU cycles per instruction (lower = faster CPU)
    int volume = 100;
    bool multitap = false;     // emulate a multitap in port 1 (4 players)
    bool vibration = true;     // controller rumble
    bool skip_intro = true;    // press Start through the logo and intro movies at boot
    int log_level = 1;
    int button_icons = 0;      // 0 = follow the input device, 1 = PlayStation, 2 = Xbox, 3 = keyboard
    std::string online_address;  // last host joined (F9 dialog)
    int online_port = 7845;
};
extern Settings g_settings;
// unknown keys are ignored, missing keys keep their defaults, out-of-range values are clamped
void settings_load(const std::string& path);
bool settings_save(const std::string& path); // atomic: temp file + rename
