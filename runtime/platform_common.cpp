// platform_common.cpp - the part of the platform layer that is the same on Windows and Linux (platform_common.h):
// logging, screenshots, the picture's aspect, frame pacing, input and the hotkeys.
#include "platform_common.h"
#include "files.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <unordered_set>

std::string g_base_dir;
std::string g_ini_path;
int g_sync_interval = 1;
int g_sync_interval_max = 4;
long long g_present_wait;

// ---------------------------------------------------------------- logging
static FILE* s_log;
void plog(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    static size_t written = 0;
    const size_t limit = 32u << 20; // keep the log file bounded
    if (!s_log) s_log = fopen(getenv("TB_LOG") ? getenv("TB_LOG") : (g_base_dir + "teambuddies.log").c_str(), "w"); // TB_LOG: test harness
    if (s_log && written < limit) {
        written += strlen(buf);
        fputs(written < limit ? buf : "log size limit reached, further output dropped\n", s_log);
        fflush(s_log);
    }
    plat_debug_out(buf);
    fputs(buf, stdout);
}
void log_once(const char* key, const char* fmt, ...) {
    static std::unordered_set<std::string> seen;
    if (!seen.insert(key).second) return;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plog("%s", buf);
}
// test harness: TB_HIDDEN runs show nothing on screen, not even an error box (the log has it)
bool hidden_run() {
    static const bool h = getenv("TB_HIDDEN") != nullptr;
    return h;
}

void fatal(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plog("FATAL: %s\n", buf);
    if (!hidden_run()) plat_error_box(buf);
    plat_exit(1);
}

// ---------------------------------------------------------------- screenshots (uncompressed PNG)
static u32 crc32_png(const u8* d, size_t n, u32 c = 0xFFFFFFFF) {
    static u32 t[256];
    if (!t[1]) for (u32 i = 0; i < 256; i++) { u32 x = i; for (int k = 0; k < 8; k++) x = (x >> 1) ^ (0xEDB88320 & (0 - (x & 1))); t[i] = x; }
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void write_png(const std::string& path, const u8* rgba, int w, int h, int pitch) {
    std::vector<u8> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        const u8* row = rgba + (size_t)y * pitch;
        for (int x = 0; x < w; x++) { raw.push_back(row[x * 4]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]); }
    }
    // zlib stream made of stored blocks
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw.size(); p += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - p);
        z.push_back(p + n >= raw.size() ? 1 : 0);
        z.push_back((u8)n); z.push_back((u8)(n >> 8)); z.push_back((u8)~n); z.push_back((u8)(~n >> 8));
        z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
    }
    u32 ad = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((u8)(ad >> s));
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    auto be32 = [&](u32 v) { u8 x[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v}; fwrite(x, 1, 4, f); };
    auto chunk = [&](const char* type, const std::vector<u8>& data) {
        be32((u32)data.size());
        std::vector<u8> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        fwrite(td.data(), 1, td.size(), f);
        be32(~crc32_png(td.data(), td.size()));
    };
    const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<u8> ihdr = {(u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h, 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
}

static bool s_shot_request;
static int s_shot_counter;

bool shot_due() {
    // F12, or every N frames when TB_SHOT_EVERY is set (debugging aid)
    static int every = getenv("TB_SHOT_EVERY") ? atoi(getenv("TB_SHOT_EVERY")) : 0;
    static int frame = 0;
    static std::vector<int> at; // TB_SHOT_AT="1650,2000,4250": screenshots at those frames
    static bool at_parsed = false;
    if (!at_parsed) {
        at_parsed = true;
        if (const char* s = getenv("TB_SHOT_AT"))
            for (const char* p = s; *p;) {
                char* end;
                long v = strtol(p, &end, 10);
                if (end == p) { p++; continue; } // a separator (or anything else): skip it
                at.push_back((int)v);
                p = end;
            }
    }
    ++frame;
    bool shot_at = std::find(at.begin(), at.end(), frame) != at.end();
    // TB_SHOT_VBLANK="7000:16": the first 16 presents from vblank 7000 on (in-between frames included)
    static long long sv_from = -1, sv_left = 0;
    if (sv_from < 0) { const char* e = getenv("TB_SHOT_VBLANK"); sv_from = 0; if (e) sscanf(e, "%lld:%lld", &sv_from, &sv_left); }
    extern u64 g_dbg_frames;
    if (sv_left > 0 && (long long)g_dbg_frames >= sv_from) { shot_at = true; sv_left--; }
    if (!(s_shot_request || shot_at || (every > 0 && frame % every == 0))) return false;
    s_shot_request = false;
    return true;
}

void shot_save(const u8* rgba, int w, int h, int pitch) {
    // TB_SHOT_DIR redirects screenshots (test harness)
    std::string dir = getenv("TB_SHOT_DIR") ? std::string(getenv("TB_SHOT_DIR")) : g_base_dir + "screenshots";
    make_dirs(dir);
    char name[64];
    snprintf(name, sizeof name, PATH_SEP "shot_%04d.png", s_shot_counter++);
    write_png(dir + name, rgba, w, h, pitch);
    plog("saved %s%s\n", dir.c_str(), name);
}

// ---------------------------------------------------------------- the picture inside the window
// size of the picture inside a ww x wh window for the aspect setting; native_h = the game's own line count
void picture_size(int ww, int wh, int native_h, double& vw, double& vh) {
    vw = ww;
    vh = wh;
    const double base = g_settings.widescreen ? 16.0 / 9.0 : 4.0 / 3.0;
    double a = base;
    switch (g_settings.aspect) {
    case ASPECT_STRETCH: return;
    case ASPECT_WIDE: if (!netplay_active()) return; a = 16.0 / 9.0; break; // online the 3D view is fixed at 16:9
    case ASPECT_16_9: a = 16.0 / 9.0; break;
    case ASPECT_PIXELS: {
        // whole-number multiples of the game's own lines: every source pixel the same size
        double bh = native_h > 0 ? native_h : 240, bw = bh * base;
        int k = (int)std::min(ww / bw, wh / bh);
        if (k >= 1) { vw = bw * k; vh = bh * k; return; }
        break;
    }
    }
    if (vw / vh > a) vw = vh * a;
    else vh = vw / a;
}

// widescreen hack strength: 3/4 for 16:9; "wide" follows the window's shape (offline only, it changes the 3D maths)
void update_widescreen_factor(int ww, int wh) {
    int q = 49152;
    if (g_settings.aspect == ASPECT_WIDE && !netplay_active() && wh > 0)
        q = std::clamp((int)(65536.0 * (4.0 / 3.0) / ((double)ww / wh)), 16384, 65536);
    g_gpu_widescreen_q16 = q;
}

// ---------------------------------------------------------------- pacing
static long long s_freq, s_next;
static bool s_fast;

bool platform_fast() { return s_fast; }

void platform_frame_pace() {
    if (!s_freq) { s_freq = plat_tick_hz(); s_next = plat_ticks(); }
    if (s_fast) { s_next = plat_ticks(); return; }
    double hz = (double)CPU_HZ / (double)CYCLES_PER_FRAME;
    s_next += (long long)(s_freq / hz);
    long long now = plat_ticks();
    if (now > s_next + s_freq / 10) { s_next = now; return; } // fell behind
    while (now < s_next) {
        long long left = (s_next - now) * 1000 / s_freq;
        if (left > 2) plat_sleep_ms((int)(left - 1));
        else plat_yield();
        now = plat_ticks();
    }
}

// Unlocked frame rate: between this vblank and its deadline, show as many frames as the cap (fps, or the
// monitor's rate when fps < 0) allows. draw(frac) draws and presents the picture for the moment `frac` (0..1]
// of the way from the previous vblank to this one; it returns false if it could not. With vsync the monitor
// paces the presents (every Nth refresh for a cap that divides its rate); any other cap is kept by spacing the
// presents in time, across vblanks, so 144 or 165 average out right although they don't divide 60.
void platform_present_unlocked(int fps, bool (*draw)(double frac)) {
    if (!s_freq) { s_freq = plat_tick_hz(); s_next = plat_ticks(); }
    if (s_fast) {
        // fast-forward and the test harness: no waiting; TB_UNLOCKED_STEPS=n shows n evenly spaced moments
        static int steps = getenv("TB_UNLOCKED_STEPS") ? std::clamp(atoi(getenv("TB_UNLOCKED_STEPS")), 1, 16) : 1;
        for (int i = 1; i <= steps; i++) if (!draw((double)i / steps)) break;
        s_next = plat_ticks();
        platform_pump();
        return;
    }
    const double vhz = (double)CPU_HZ / (double)CYCLES_PER_FRAME;
    const long long period = (long long)(s_freq / vhz);
    long long start = s_next; // the previous deadline: frac 0
    s_next += period;
    long long now = plat_ticks();
    if (now > s_next + s_freq / 10) { s_next = now; start = now - period; } // fell behind
    const int hz = platform_refresh_hz();
    const bool vs = g_settings.vsync;
    const int cap = fps > 0 ? fps : hz;
    const double per = (double)hz / cap; // refreshes per frame
    const int whole = std::clamp((int)std::lround(per), 1, g_sync_interval_max);
    g_sync_interval = vs && std::fabs(per - whole) < 0.05 ? whole : 1;
    const bool timed = !vs || g_sync_interval * cap < hz * 0.95; // vsync alone would show more than the cap
    const long long refresh = s_freq * g_sync_interval / hz; // between shown frames, with vsync
    const long long gap = s_freq / cap;
    static long long next_free = 0;  // earliest time for the next present (timed); carries over between vblanks
    static long long prev_shown = 0; // with vsync: the refresh that puts up the previous frame
    for (int shown_n = 0;;) {
        now = plat_ticks();
        const long long at = timed ? std::max(next_free, now) : now;
        // with vsync a present waits until the frame before it is up. One that would still be waiting (or not yet
        // due) at the next vblank belongs to that vblank, but every vblank shows at least one.
        const bool late = (vs ? std::max(at, prev_shown) : at) >= s_next;
        if (late && shown_n) break;
        if (!late && at > now) {
            long long ms = (at - now) * 1000 / s_freq;
            if (ms > 1) plat_sleep_ms((int)(ms - 1));
            else plat_yield();
            continue;
        }
        // the moment it reaches the screen: right away without vsync; with it, the first refresh after both now
        // and the previous frame's (counting refreshes from that one)
        long long shown = now;
        if (vs && prev_shown > 0) shown = prev_shown + std::max<long long>(1, (now - prev_shown + refresh - 1) / refresh) * refresh;
        // with vsync the picture runs a refresh behind: one put up as late as a refresh after the next vblank
        // still has its frame (frac can then go below 0, into the previous vblank)
        double frac = std::clamp((double)(shown - (vs ? refresh : 0) - start) / (double)period, -1.0, 1.0);
        const long long t_draw = now;
        if (!draw(frac)) break;
        shown_n++;
        now = plat_ticks();
        // a present that had to wait returned as the previous frame went up: anchor on that refresh
        const bool waited = g_present_wait > s_freq / 1000;
        if (vs) prev_shown = waited ? now + refresh : shown;
        static int pace_log = getenv("TB_PACE_LOG") ? atoi(getenv("TB_PACE_LOG")) : 0; // debug: the first n presents
        if (pace_log > 0) {
            pace_log--;
            auto ms = [&](long long t) { return (double)t * 1000.0 / (double)s_freq; };
            plog("pace: at %.2f ms, +%.2f ms in vblank, draw %.2f + present %.2f ms, %.2f ms left, shown at %.2f -> %.2f, frac %.3f, interval %d\n",
                 ms(now), ms(t_draw - start), ms(now - t_draw - g_present_wait), ms(g_present_wait),
                 ms(s_next - now), ms(shown), ms(vs ? prev_shown : shown), frac, g_sync_interval);
        }
        next_free = std::max(next_free + gap, now);
    }
    while (now < s_next) {
        long long ms = (s_next - now) * 1000 / s_freq;
        if (ms > 2) plat_sleep_ms((int)(ms - 1));
        else plat_yield();
        now = plat_ticks();
    }
    platform_pump();
}

// ---------------------------------------------------------------- input
static bool s_quit;
void platform_request_quit() { s_quit = true; }
bool platform_quit_requested() { return s_quit; }

void platform_quit_if_requested() {
    if (!s_quit) return;
    netplay_shutdown();
    settings_save(g_ini_path);
    gamepad_shutdown(); // stops rumble
    plog("quit\n");
    plat_exit(0);
}

void poll_input() {
    for (int p = 0; p < 4; p++) { g_pads[p].buttons = 0; g_pads[p].connected = false; g_pads[p].lx = g_pads[p].ly = g_pads[p].rx = g_pads[p].ry = 0x80; }
    g_pads[0].connected = true; // keyboard is always player 1
    const bool focus = plat_focused();
    if (focus) {
        static const struct { PlatKey k; u16 b; } km[] = {
            {PK_UP, PAD_UP}, {PK_DOWN, PAD_DOWN}, {PK_LEFT, PAD_LEFT}, {PK_RIGHT, PAD_RIGHT},
            {PK_Z, PAD_CROSS}, {PK_X, PAD_CIRCLE}, {PK_A, PAD_SQUARE}, {PK_S, PAD_TRIANGLE},
            {PK_Q, PAD_L1}, {PK_W, PAD_R1}, {PK_E, PAD_L2}, {PK_R, PAD_R2},
            {PK_RETURN, PAD_START}, {PK_BACK, PAD_SELECT}, {PK_RSHIFT, PAD_SELECT},
        };
        for (auto& k : km) if (plat_key(k.k)) g_pads[0].buttons |= k.b;
    }
    // button prompts follow whichever device was used last (or the ini choice)
    static u16 prev_kb = 0;
    const u16 kb = g_pads[0].buttons; // keyboard only, controllers are added below
    bool kb_used = (kb & ~prev_kb) != 0;
    prev_kb = kb;
    // controllers (SDL3); the first one shares player 1 with the keyboard
    int pad_family = gamepad_poll(0);
    switch (g_settings.button_icons) {
    case 1: g_input_device = DEV_PLAYSTATION; break;
    case 2: g_input_device = DEV_XBOX; break;
    case 3: g_input_device = DEV_KEYBOARD; break;
    default: {
        static bool first = true; // before any input: the controller if one is plugged in, else the keyboard
        if (first) { first = false; int f = gamepad_first_family(); g_input_device = f >= 0 ? (InputDevice)f : DEV_KEYBOARD; }
        if (kb_used) g_input_device = DEV_KEYBOARD;
        else if (pad_family >= 0) g_input_device = (InputDevice)pad_family;
    }
    }
    // test harness: run unthrottled; TB_TURBO=n (n > 1) only until vblank n, then at real speed
    static long long turbo_until = getenv("TB_TURBO") ? std::max(1LL, atoll(getenv("TB_TURBO"))) : 0;
    extern u64 g_dbg_frames;
    const bool turbo = turbo_until == 1 || (long long)g_dbg_frames < turbo_until;
    s_fast = !netplay_active() && (turbo || (focus && plat_key(PK_TAB))); // online: both run at real speed

    // scripted input for automated testing: TB_INPUT="600=start;640=;900=cross" (pad 1), TB_INPUT2 (pad 2)
    static std::vector<std::pair<int, u16>> script[2];
    static bool parsed = false;
    static int frame = 0;
    if (!parsed) {
        parsed = true;
        static const struct { const char* n; u16 b; } bn[] = {
            {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT}, {"cross", PAD_CROSS},
            {"circle", PAD_CIRCLE}, {"square", PAD_SQUARE}, {"triangle", PAD_TRIANGLE}, {"start", PAD_START},
            {"select", PAD_SELECT}, {"l1", PAD_L1}, {"r1", PAD_R1}, {"l2", PAD_L2}, {"r2", PAD_R2}};
        const char* vars[2] = {"TB_INPUT", "TB_INPUT2"};
        for (int pad = 0; pad < 2; pad++) {
            const char* s = getenv(vars[pad]);
            if (!s) continue;
            std::string all = s;
            size_t p = 0;
            while (p < all.size()) {
                size_t e = all.find(';', p);
                std::string item = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
                p = e == std::string::npos ? all.size() : e + 1;
                size_t eq = item.find('=');
                if (eq == std::string::npos) continue;
                u16 b = 0;
                std::string names = item.substr(eq + 1) + "+";
                for (size_t q = 0, r; (r = names.find('+', q)) != std::string::npos; q = r + 1)
                    for (auto& x : bn) if (names.substr(q, r - q) == x.n) b |= x.b;
                script[pad].push_back({atoi(item.c_str()), b});
            }
        }
    }
    frame++;
    static int quit_at = getenv("TB_FRAMES") ? atoi(getenv("TB_FRAMES")) : 0;
    if (quit_at > 0 && frame >= quit_at) s_quit = true;
    // TB_KEYS="6400=F1;6500=F3": function keys pressed at those frames (test harness)
    static std::vector<std::pair<int, int>> keys;
    static bool keys_parsed = false;
    if (!keys_parsed) {
        keys_parsed = true;
        if (const char* e = getenv("TB_KEYS"))
            for (const char* p = e; *p;) {
                int f = 0, n = 0;
                if (sscanf(p, "%d=F%d", &f, &n) == 2 && n >= 1 && n <= 12) keys.push_back({f, n});
                p = strchr(p, ';');
                if (!p) break;
                p++;
            }
    }
    for (auto& k : keys) if (k.first == frame) hotkey(k.second);
    for (int pad = 0; pad < 2; pad++) {
        if (script[pad].empty()) continue;
        u16 sb = 0;
        for (auto& e : script[pad]) if (frame >= e.first) sb = e.second;
        // the script replaces the real devices: a plugged-in controller (stick drift) must not change a test run
        g_pads[pad].connected = true;
        g_pads[pad].buttons = sb;
        g_pads[pad].lx = g_pads[pad].ly = g_pads[pad].rx = g_pads[pad].ry = 0x80;
    }
    // skip_intro: tap Start while the boot logos and intro movies (24-bit MDEC frames) play. Ends once a
    // non-movie screen has stayed up for 7 s after a movie (the title), or 60 s after boot. Online the taps
    // are this player's own input, so both games see them.
    static int intro_phase = g_settings.skip_intro ? 0 : 2, intro_quiet = 0, intro_tick = 0; // 0 before, 1 in movies, 2 done
    if (intro_phase < 2) {
        DisplayInfo d = gpu_display();
        if (d.depth24 && d.enabled) {
            intro_phase = 1;
            intro_quiet = 0;
            if (intro_tick++ % 16 < 6) g_pads[0].buttons |= PAD_START;
        } else if ((intro_phase == 1 && ++intro_quiet > 420) || frame > 3600) {
            intro_phase = 2;
        }
    }
    netplay_sync(); // online: replaces the pads with both players' input for this frame
}

void hotkey(int f) {
    switch (f) {
    case 11: platform_toggle_fullscreen(); break;
    case 12: s_shot_request = true; break;
    case 9: netplay_dialog(); break;
    case 2: if (g_settings.scale > 1) g_settings.scale--; plog("internal resolution x%d\n", g_settings.scale); break;
    case 3: if (g_settings.scale < 8) g_settings.scale++; plog("internal resolution x%d\n", g_settings.scale); break;
    case 4: if (netplay_active()) break; // changes the 3D maths: both players must match
        g_settings.widescreen = !g_settings.widescreen; g_gpu_widescreen_hack = g_settings.widescreen; break;
    case 5: g_settings.bilinear = !g_settings.bilinear; break;
    case 6: {
        static const char* names[ASPECT_COUNT] = {"4:3", "16:9", "stretch", "wide", "pixels"};
        g_settings.aspect = (g_settings.aspect + 1) % ASPECT_COUNT;
        plog("aspect %s\n", names[g_settings.aspect]);
        break;
    }
    case 1:
        g_settings.anti_aliasing = (g_settings.anti_aliasing + 1) % AA_COUNT;
        plog("anti-aliasing %s\n", k_aa_names[g_settings.anti_aliasing]);
        break;
    case 7:
        g_settings.texture_filter = (g_settings.texture_filter + 1) % TEX_COUNT;
        plog("texture filtering %s\n", k_tex_filter_names[g_settings.texture_filter]);
        break;
    case 8: g_settings.true_color = !g_settings.true_color; break;
    case 10: { // original <-> the rate chosen in the ini (60 if none)
        static int last_on = 60;
        if (g_settings.frame_rate != 0) { last_on = g_settings.frame_rate; g_settings.frame_rate = 0; }
        else g_settings.frame_rate = last_on;
        plog("frame rate: %d (0 original, -1 monitor)\n", g_settings.frame_rate);
        break;
    }
    }
}
