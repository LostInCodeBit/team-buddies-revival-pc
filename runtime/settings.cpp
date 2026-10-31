// settings.cpp - teambuddies.ini reader/writer, shared by the game and the launcher.
#include "files.h"
#include "settings.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>

Settings g_settings;

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static const char* const k_aspect_names[ASPECT_COUNT] = {"4:3", "16:9", "stretch", "wide", "pixels"};
const char* const k_aa_names[AA_COUNT] = {"off", "fxaa", "msaa2x", "msaa4x", "msaa8x", "ssaa2x", "ssaa4x", "ssaa8x"};
const char* const k_tex_filter_names[TEX_COUNT] = {"off", "bilinear", "xbr"};

void settings_load(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    std::map<std::string, std::string> kv;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    Settings& s = g_settings;
    auto I = [&](const char* k, int& v) { if (kv.count(k)) v = atoi(kv[k].c_str()); };
    auto B = [&](const char* k, bool& v) { if (kv.count(k)) v = kv[k] == "1" || kv[k] == "true" || kv[k] == "yes"; };
    if (kv.count("disc")) s.disc = kv["disc"];
    if (kv.count("renderer")) s.renderer = kv["renderer"] == "software" ? 0 : 1;
    I("resolution_scale", s.scale);
    I("window_width", s.window_w);
    I("window_height", s.window_h);
    if (kv.count("display")) {
        const std::string& v = kv["display"];
        s.display = v == "borderless" ? DISPLAY_BORDERLESS : v == "fullscreen" ? DISPLAY_FULLSCREEN : DISPLAY_WINDOW;
    } else if (kv.count("fullscreen")) { // ini from before the display setting
        bool fs = false;
        B("fullscreen", fs);
        s.display = fs ? DISPLAY_BORDERLESS : DISPLAY_WINDOW;
    }
    B("vsync", s.vsync);
    if (kv.count("frame_rate")) {
        const std::string& v = kv["frame_rate"];
        int n = atoi(v.c_str());
        s.frame_rate = v == "monitor" ? FRAME_RATE_MONITOR : n >= 60 ? std::min(n, 500) : 0; // "original" -> 0
    }
    if (kv.count("anti_aliasing"))
        for (int i = 0; i < AA_COUNT; i++) if (kv["anti_aliasing"] == k_aa_names[i]) s.anti_aliasing = i;
    B("widescreen_hack", s.widescreen);
    if (kv.count("aspect"))
        for (int i = 0; i < ASPECT_COUNT; i++) if (kv["aspect"] == k_aspect_names[i]) s.aspect = i;
    B("smooth_output", s.bilinear);
    if (kv.count("texture_filtering")) { // also the true/false of older versions
        const std::string& v = kv["texture_filtering"];
        s.texture_filter = v == "xbr" ? TEX_XBR : v == "bilinear" || v == "true" || v == "1" || v == "yes" ? TEX_BILINEAR : TEX_NEAREST;
    }
    B("hd_text", s.hd_text);
    B("true_color", s.true_color);
    if (kv.count("cpu_cycles_per_instruction")) s.cpu_cpi = atof(kv["cpu_cycles_per_instruction"].c_str());
    I("volume", s.volume);
    B("multitap", s.multitap);
    B("vibration", s.vibration);
    B("skip_intro", s.skip_intro);
    if (kv.count("button_icons")) {
        const std::string& v = kv["button_icons"];
        s.button_icons = v == "playstation" ? 1 : v == "xbox" ? 2 : v == "keyboard" ? 3 : 0;
    }
    if (kv.count("online_address")) s.online_address = kv["online_address"];
    I("online_port", s.online_port);
    I("log_level", s.log_level);
    s.scale = std::clamp(s.scale, 1, 8);
    s.window_w = std::clamp(s.window_w, 320, 16384);
    s.window_h = std::clamp(s.window_h, 240, 16384);
    s.volume = std::clamp(s.volume, 0, 100);
    if (!(s.cpu_cpi >= 0.25 && s.cpu_cpi <= 8.0)) s.cpu_cpi = 2.0;
    if (s.online_port < 1 || s.online_port > 65535) s.online_port = 7845;
}

bool settings_save(const std::string& path) {
    // write a temp file and swap it in, so a crash mid-write never leaves a broken ini
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        const Settings& s = g_settings;
        auto b = [](bool v) { return v ? "true" : "false"; };
        f << "; Team Buddies native port settings (also edited by TeamBuddiesLauncher.exe)\n";
        f << "disc = " << s.disc << "\n\n";
        f << "; renderer: hardware (upscaled: Direct3D 11 on Windows, Vulkan on Linux) or software (original 1x)\n";
        f << "renderer = " << (s.renderer ? "hardware" : "software") << "\n";
        f << "; internal resolution multiplier for the hardware renderer (1-8), F2/F3 in game\n";
        f << "resolution_scale = " << s.scale << "\n";
        f << "; display: window, borderless or fullscreen (exclusive); F11 switches window <-> full screen\n";
        f << "display = " << (s.display == DISPLAY_BORDERLESS ? "borderless" : s.display == DISPLAY_FULLSCREEN ? "fullscreen" : "window") << "\n";
        f << "window_width = " << s.window_w << "\nwindow_height = " << s.window_h << "\n";
        f << "vsync = " << b(s.vsync) << "\n";
        f << "; frame_rate: original (gameplay runs at 30), 60, 120, 144, 165, 240... or monitor: in-between frames are\n";
        f << "; drawn up to that rate, the game keeps its speed (hardware renderer; F10 switches it off and on)\n";
        f << "frame_rate = " << (s.frame_rate == FRAME_RATE_MONITOR ? "monitor" : s.frame_rate >= 60 ? std::to_string(s.frame_rate) : "original") << "\n";
        f << "; anti_aliasing (hardware renderer, F1): off, fxaa, msaa2x/4x/8x (polygon edges), ssaa2x/4x/8x (edges and textures)\n";
        f << "anti_aliasing = " << k_aa_names[std::clamp(s.anti_aliasing, 0, AA_COUNT - 1)] << "\n";
        f << "; aspect: 4:3, 16:9, wide (fills the window's shape), stretch or pixels (whole-number scaling) (F6)\n";
        f << "aspect = " << k_aspect_names[std::clamp(s.aspect, 0, ASPECT_COUNT - 1)] << "\n";
        f << "; widen the 3D view for wide screens (F4)\n";
        f << "widescreen_hack = " << b(s.widescreen) << "\n";
        f << "; bilinear filtering of the final image (F5)\n";
        f << "smooth_output = " << b(s.bilinear) << "\n";
        f << "; texture_filtering (F7): off (the original's sharp texels), bilinear (blended; small sprites such as\n";
        f << "; text stay sharp) or xbr (pixel art redrawn with smooth edges at any size)\n";
        f << "texture_filtering = " << k_tex_filter_names[std::clamp(s.texture_filter, 0, TEX_COUNT - 1)] << "\n";
        f << "; draw the game's letters from smooth high-resolution reconstructions (hardware renderer)\n";
        f << "hd_text = " << b(s.hd_text) << "\n";
        f << "; render without the PS1's 15-bit dithering\n";
        f << "true_color = " << b(s.true_color) << "\n";
        f << "; emulated CPU speed: 2.0 = original, lower = faster CPU (less slowdown)\n";
        f << "cpu_cycles_per_instruction = " << s.cpu_cpi << "\n";
        f << "volume = " << s.volume << "\n";
        f << "; emulate a multitap on port 1 for 4 players\n";
        f << "multitap = " << b(s.multitap) << "\n";
        f << "; controller rumble\n";
        f << "vibration = " << b(s.vibration) << "\n";
        f << "; skip the logos and intro movies at start\n";
        f << "skip_intro = " << b(s.skip_intro) << "\n";
        f << "; button prompts: auto (follow the device in use), playstation, xbox or keyboard\n";
        f << "button_icons = " << (s.button_icons == 1 ? "playstation" : s.button_icons == 2 ? "xbox" : s.button_icons == 3 ? "keyboard" : "auto") << "\n";
        f << "; online play (F9): last address joined and the UDP port\n";
        f << "online_address = " << s.online_address << "\n";
        f << "online_port = " << s.online_port << "\n";
        f << "log_level = " << s.log_level << "\n";
        f.flush();
        if (!f) { f.close(); file_delete(tmp); return false; }
    }
    return file_move(tmp, path, MOVE_DURABLE);
}
