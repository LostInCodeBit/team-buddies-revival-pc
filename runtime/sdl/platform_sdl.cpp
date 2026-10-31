// platform_sdl.cpp - SDL3 platform layer (Linux; it builds on Windows too, for testing): window, keyboard
// (controllers: gamepad.cpp), audio, presentation through the SDL GPU API (sdl_present.cpp), crash and hang
// reports. What it shares with the Win32 layer is in platform_common.cpp.
#include "../platform_common.h"
#include "../files.h"
#include "sdl_platform.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <thread>
#ifdef __linux__
#include <csignal>
#include <execinfo.h>
#include <pthread.h>
#endif

void gamepad_event(const SDL_Event& e); // gamepad.cpp
extern bool g_gamepad_events_external;

// ---------------------------------------------------------------- window
static SDL_Window* s_window;
static bool s_fullscreen;
static bool s_focus = true;
static SDL_Scancode s_scan[PK_COUNT]; // the keys of platform_common's map, where the keyboard layout has them

SDL_Window* sdl_window() { return s_window; }
void* platform_hwnd() { return s_window; }

static void map_keys() {
    static const SDL_Keycode keys[PK_COUNT] = {SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_Z, SDLK_X, SDLK_A, SDLK_S,
                                               SDLK_Q, SDLK_W, SDLK_E, SDLK_R, SDLK_RETURN, SDLK_BACKSPACE, SDLK_RSHIFT, SDLK_TAB};
    for (int i = 0; i < PK_COUNT; i++) s_scan[i] = SDL_GetScancodeFromKey(keys[i], nullptr);
}

// ---------------------------------------------------------------- hooks for platform_common.cpp
long long plat_ticks() { return (long long)SDL_GetPerformanceCounter(); }
long long plat_tick_hz() { return (long long)SDL_GetPerformanceFrequency(); }
void plat_sleep_ms(int ms) { SDL_Delay((Uint32)ms); }
void plat_yield() { SDL_CPUPauseInstruction(); }
void plat_debug_out(const char*) {}
void plat_error_box(const char* s) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Team Buddies", s, s_window); }
void plat_exit(int code) {
    fflush(stdout);
    _Exit(code); // no static destructors: the audio and watchdog threads are still running
}
bool plat_focused() { return s_window && s_focus; }
bool plat_key(PlatKey k) {
    const bool* st = SDL_GetKeyboardState(nullptr);
    return st && s_scan[k] != SDL_SCANCODE_UNKNOWN && st[s_scan[k]];
}

static int s_full_mode = DISPLAY_BORDERLESS; // what F11 switches to: borderless or exclusive (the ini's choice)

static void set_fullscreen(bool on) {
    if (!s_window || on == s_fullscreen) return;
    s_fullscreen = on;
    // exclusive: the desktop's own mode set explicitly; borderless: no mode (Wayland only has that one)
    const SDL_DisplayMode* m = on && s_full_mode == DISPLAY_FULLSCREEN ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(s_window)) : nullptr;
    SDL_SetWindowFullscreenMode(s_window, m);
    SDL_SetWindowFullscreen(s_window, on);
}

void platform_toggle_fullscreen() {
    set_fullscreen(!s_fullscreen);
    g_settings.display = s_fullscreen ? s_full_mode : DISPLAY_WINDOW;
}

void platform_get_window_size(int& w, int& h) {
    w = h = 0;
    if (s_window) SDL_GetWindowSizeInPixels(s_window, &w, &h);
    else { w = g_settings.window_w; h = g_settings.window_h; } // hidden runs draw at the window size from the ini
}

void platform_set_title(const char* text) { if (s_window) SDL_SetWindowTitle(s_window, text); }

// refresh rate of the display the window is on (checked at most once a second)
int platform_refresh_hz() {
    static int hz = 60;
    static Uint64 last = 0;
    Uint64 now = SDL_GetTicks();
    if (last && now - last < 1000) return hz;
    last = now;
    SDL_DisplayID d = s_window ? SDL_GetDisplayForWindow(s_window) : SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode* m = d ? SDL_GetCurrentDisplayMode(d) : nullptr)
        if (m->refresh_rate > 1) hz = (int)std::lround(m->refresh_rate);
    return hz;
}

// ---------------------------------------------------------------- events
static void handle_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: platform_request_quit(); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: s_focus = true; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: s_focus = false; break;
    case SDL_EVENT_KEYMAP_CHANGED: map_keys(); break;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) break;
        if (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) { platform_toggle_fullscreen(); break; }
        if (e.key.key >= SDLK_F1 && e.key.key <= SDLK_F12) hotkey((int)(e.key.key - SDLK_F1) + 1);
        break;
    default: gamepad_event(e); break;
    }
}

bool platform_pump_messages() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(e);
    return !platform_quit_requested();
}

bool platform_pump() {
    platform_pump_messages();
    poll_input();
    platform_quit_if_requested();
    return true;
}

// ---------------------------------------------------------------- presentation
// software renderer frames and the 24-bit movies; with the GPU presenter (sdl_present.cpp) they go to the
// window like the hardware renderer's, otherwise (no GPU) only screenshots are taken
void platform_present_image(const u32* rgba, int w, int h, int stride, bool enabled) {
    if (!sdl_present_image(rgba, w, h, stride, enabled)) {
        if (shot_due()) shot_save((const u8*)rgba, w, h, stride * 4);
        g_present_wait = 0;
    }
    platform_frame_pace();
    platform_pump();
}

// ---------------------------------------------------------------- audio
static SDL_AudioStream* s_audio;
static const int AUDIO_FRAMES = 512, AUDIO_MAX_QUEUED = 8 * AUDIO_FRAMES; // as the XAudio2 path: ~93 ms at most
static s16 s_abuf[AUDIO_FRAMES * 2];
static int s_afill;

static bool audio_init() {
    if (hidden_run()) return false; // silent
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    s_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s_audio) return false;
    SDL_ResumeAudioStreamDevice(s_audio);
    return true;
}

// with no device, retry every ~2 s so plugging one in works (SDL follows default-device changes itself)
static void audio_recover() {
    static Uint64 next_try = 0;
    Uint64 now = SDL_GetTicks();
    if (s_audio || now < next_try || hidden_run()) return;
    next_try = now + 2000;
    if (audio_init()) plog("audio: output opened\n");
}

int platform_audio_queued() { return s_audio ? SDL_GetAudioStreamQueued(s_audio) / 4 : 0; }

int g_dbg_audio_peak;
void platform_audio_push(const s16* stereo, int frames) {
    for (int i = 0; i < frames * 2; i++) { int a = stereo[i] < 0 ? -stereo[i] : stereo[i]; if (a > g_dbg_audio_peak) g_dbg_audio_peak = a; }
    if (!s_audio) audio_recover();
    if (!s_audio) return;
    for (int i = 0; i < frames; i++) {
        s_abuf[s_afill * 2] = stereo[i * 2];
        s_abuf[s_afill * 2 + 1] = stereo[i * 2 + 1];
        if (++s_afill == AUDIO_FRAMES) {
            s_afill = 0;
            if (platform_audio_queued() < AUDIO_MAX_QUEUED && !platform_fast()) SDL_PutAudioStreamData(s_audio, s_abuf, sizeof s_abuf);
        }
    }
}

// ---------------------------------------------------------------- crash and hang reports (debugging aids)
u32 g_watch_lo, g_watch_len;
extern u64 g_dbg_frames, g_dbg_exceptions;

int platform_private_mb() {
#ifdef __linux__
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    long kb = 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "RssAnon: %ld kB", &kb) == 1) break;
    fclose(f);
    return (int)(kb >> 10);
#else
    return 0;
#endif
}

#ifdef __linux__
static int log_fd() {
    std::string p = getenv("TB_LOG") ? getenv("TB_LOG") : g_base_dir + "teambuddies.log";
    static int fd = -1;
    if (fd < 0) fd = open(p.c_str(), O_WRONLY | O_APPEND);
    return fd;
}
// the native frames name the recompiled functions (linked with -rdynamic)
static void write_stack(int fd) {
    void* f[48];
    int n = backtrace(f, 48);
    backtrace_symbols_fd(f, n, fd);
    char buf[200];
    int len = snprintf(buf, sizeof buf, "  cpu: ra=%08X sp=%08X v0=%08X a0=%08X a1=%08X sr=%08X frame=%llu\n", g_cpu.r[31], g_cpu.r[29],
                       g_cpu.r[2], g_cpu.r[4], g_cpu.r[5], g_cpu.cop0[12], (unsigned long long)g_dbg_frames);
    if (len > 0 && write(fd, buf, (size_t)len) < 0) {}
}
static pthread_t s_main_thread;
static void on_stack_request(int) { write_stack(log_fd()); }
static void on_crash(int sig) {
    char buf[64];
    int len = snprintf(buf, sizeof buf, "CRASH: signal %d\n", sig);
    int fd = log_fd();
    if (len > 0 && write(fd, buf, (size_t)len) < 0) {}
    write_stack(fd);
    _Exit(1);
}
#endif

void platform_log_stack_here() {
#ifdef __linux__
    write_stack(log_fd());
#endif
}

void watch_hit(u32 a, u32 v, int size) {
    static int hits = 0;
    if (++hits > 20) return;
    plog("watch: write%d %08X = %08X (frame %llu, ra=%08X sp=%08X)\n", size * 8, a, v, (unsigned long long)g_dbg_frames, g_cpu.r[31], g_cpu.r[29]);
    platform_log_stack_here();
}

static void watchdog() {
    u64 last = 0, last_frames = 0;
    int stuck = 0;
    bool dumped = false;
    for (;;) {
        SDL_Delay(1000);
        // progress = frames advance and interrupts keep being taken
        bool moving = g_dbg_frames != last_frames && g_dbg_exceptions != last;
        last_frames = g_dbg_frames;
        last = g_dbg_exceptions;
        if (moving) { stuck = 0; dumped = false; continue; }
        if (++stuck < 3 || dumped) continue;
        dumped = true;
        plog("watchdog: no frame for 3 s, main thread stack:\n");
#ifdef __linux__
        pthread_kill(s_main_thread, SIGUSR1);
#endif
    }
}

void platform_start_watchdog() {
    if (const char* w = getenv("TB_WATCH")) {
        unsigned lo = 0, len = 4;
        sscanf(w, "%x:%x", &lo, &len);
        g_watch_lo = lo & 0x1FFFFF;
        g_watch_len = len;
        plog("watching writes to %06X..%06X\n", g_watch_lo, g_watch_len + g_watch_lo);
    }
#ifdef __linux__
    s_main_thread = pthread_self();
    signal(SIGUSR1, on_stack_request);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) signal(sig, on_crash);
#endif
    std::thread(watchdog).detach();
}

// ---------------------------------------------------------------- disc choice (no disc next to the game)
std::string platform_ask_disc() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return "";
    struct Pick { std::atomic<bool> done{false}; std::string path; } pick;
    static const SDL_DialogFileFilter filters[] = {{"PlayStation disc image (*.cue)", "cue"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(
        [](void* ud, const char* const* list, int) {
            Pick* p = (Pick*)ud;
            if (list && list[0]) p->path = list[0];
            else if (!list) plog("disc: no file chooser (%s)\n", SDL_GetError());
            p->done = true;
        },
        &pick, nullptr, filters, 2, nullptr, false);
    while (!pick.done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        SDL_Delay(10);
    }
    if (pick.path.empty())
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Team Buddies",
                                 "No disc image was chosen.\nStart the game with the path of your Team Buddies (USA, SLUS-00869) "
                                 ".cue file, or put the disc image next to the game.", nullptr);
    return pick.path;
}

// ---------------------------------------------------------------- init
bool platform_init() {
    const bool hidden = hidden_run();
    // test harness: hidden runs never open a window (the offscreen driver needs no display) and have no sound
    if (hidden) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    g_gamepad_events_external = true;
    gamepad_init();
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        plog("sdl: %s\n", SDL_GetError());
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    }
    plog("sdl: %d.%d.%d, video %s, audio %s\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
         SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
         SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none");
    map_keys();
    if (!hidden) {
        s_window = SDL_CreateWindow("Team Buddies", g_settings.window_w, g_settings.window_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!s_window) { plog("sdl: no window (%s)\n", SDL_GetError()); return false; }
    } else {
        plog("hidden test run: no window, no sound\n");
    }
    sdl_present_init(); // the GPU device and the window's swapchain; without one, frames are only screenshots
    if (!hidden && g_settings.display != DISPLAY_WINDOW) {
        s_full_mode = g_settings.display;
        set_fullscreen(true);
    }
    if (!hidden && !audio_init()) plog("audio: unavailable (%s), running without sound\n", SDL_GetError());
    g_gpu_widescreen_hack = g_settings.widescreen;
    return true;
}

void platform_shutdown() {
    if (s_audio) { SDL_DestroyAudioStream(s_audio); s_audio = nullptr; }
    sdl_present_shutdown();
    if (s_window) { SDL_DestroyWindow(s_window); s_window = nullptr; }
}
