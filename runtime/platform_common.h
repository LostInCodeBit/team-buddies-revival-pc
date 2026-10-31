// platform_common.h - what the two platform layers (Win32: platform.cpp, SDL3: sdl/platform_sdl.cpp) share.
// The shared part is platform_common.cpp: logging, screenshots, the picture's aspect, frame pacing, input
// (keyboard map, controllers, scripted test input) and the hotkeys.
#pragma once
#include "psx.h"

// ---------------------------------------------------------------- provided by each platform layer
long long plat_ticks();               // monotonic clock
long long plat_tick_hz();
void plat_sleep_ms(int ms);           // about ms (with a 1 ms timer resolution)
void plat_yield();                    // a spin-wait hint
void plat_debug_out(const char* s);   // to an attached debugger, if any
void plat_error_box(const char* s);   // a message box (never in hidden test runs)
[[noreturn]] void plat_exit(int code);
bool plat_focused();                  // the game window has the keyboard
enum PlatKey { PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT, PK_Z, PK_X, PK_A, PK_S, PK_Q, PK_W, PK_E, PK_R,
               PK_RETURN, PK_BACK, PK_RSHIFT, PK_TAB, PK_COUNT };
bool plat_key(PlatKey k);             // held right now

// ---------------------------------------------------------------- shared (platform_common.cpp)
bool hidden_run();                    // TB_HIDDEN: no window, sound or message boxes (test harness)
void hotkey(int f);                   // F1..F12 pressed (f = 1..12)
void poll_input();                    // keyboard, controllers and scripted input -> g_pads, at every pump
void platform_request_quit();
bool platform_quit_requested();
void platform_quit_if_requested();    // after a pump: saves and exits once the window was closed (or TB_FRAMES)
bool platform_fast();                 // fast-forward (Tab) or a turbo test run: no pacing
bool shot_due();                      // a screenshot of the frame being presented is wanted (F12, TB_SHOT_*)
void shot_save(const u8* rgba, int w, int h, int pitch); // RGBA rows, to screenshots/ (or TB_SHOT_DIR)
void picture_size(int ww, int wh, int native_h, double& vw, double& vh); // size inside the window (aspect)
void update_widescreen_factor(int ww, int wh);
void write_png(const std::string& path, const u8* rgba, int w, int h, int pitch);
extern int g_sync_interval;           // refreshes per present with vsync, chosen by the pacing
extern int g_sync_interval_max;       // what the swapchain can do (DXGI 4, SDL GPU 1)
extern long long g_present_wait;      // ticks the last present call took (it waits while the frame before is queued)
