// ui.h - small immediate-mode UI on Direct2D/DirectWrite for the launcher (Material 3 dark scheme).
// Every repaint redraws the whole window; widgets draw themselves and read the mouse in the same call.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

typedef unsigned int u32;

struct Rc {
    float x = 0, y = 0, w = 0, h = 0;
    bool in(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rc inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    float r() const { return x + w; }
    float b() const { return y + h; }
};

// Material 3 color roles (dark scheme from a blue seed, yellow tertiary), 0xAARRGGBB
namespace col {
const u32 primary = 0xFFB4C5FF, on_primary = 0xFF00297A, primary_container = 0xFF1F3D8A, on_primary_container = 0xFFDBE1FF;
const u32 secondary_container = 0xFF3F4759, on_secondary_container = 0xFFDCE2F9;
const u32 tertiary = 0xFFEBC248, on_tertiary = 0xFF3E2E00, tertiary_container = 0xFF5A4300, on_tertiary_container = 0xFFFFDF93;
const u32 surface = 0xFF121318, surface_lowest = 0xFF0D0E13, surface_low = 0xFF1A1B21, surface_container = 0xFF1E1F25;
const u32 surface_high = 0xFF292A2F, surface_highest = 0xFF34343A;
const u32 on_surface = 0xFFE3E1E9, on_surface_variant = 0xFFC5C6D0, outline = 0xFF8F909A, outline_variant = 0xFF45464F;
const u32 error = 0xFFFFB4AB, error_container = 0xFF93000A, on_error_container = 0xFFFFDAD6;
const u32 inverse_surface = 0xFFE3E1E9, inverse_on_surface = 0xFF2F3036, inverse_primary = 0xFF4A5C92;
const u32 success = 0xFF9AD49A; // custom role: disc found and recognised
inline u32 alpha(u32 c, float a) { return (c & 0xFFFFFF) | ((u32)(a * ((c >> 24) & 255)) << 24); }
} // namespace col

enum Font { F_BODY, F_BODY_SMALL, F_LABEL, F_LABEL_SMALL, F_TITLE, F_TITLE_LARGE, F_HEADLINE, F_DISPLAY, F_WORDMARK, F_ICON, F_ICON_LARGE, F_COUNT };
enum Align { A_LEFT, A_CENTER, A_RIGHT };

// icon glyphs (Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on Windows 10)
namespace ic {
const wchar_t play = 0xE768, settings = 0xE713, notes = 0xE8A5, disc = 0xE958, game = 0xE7FC, save = 0xE74E;
const wchar_t chevron = 0xE70D, check = 0xE73E, minimize = 0xE921, close = 0xE8BB, reset = 0xE72C, folder = 0xE8B7;
const wchar_t warning = 0xE7BA, keyboard = 0xE765, info = 0xE946;
} // namespace ic

namespace ui {
struct Input {
    float mx = -1, my = -1;   // mouse in DIPs
    bool down = false;        // left button held
    bool pressed = false;     // went down this frame
    bool released = false;    // went up this frame
    float wheel = 0;          // notches this frame
    bool enter = false, escape = false;
};
extern Input in;
extern float W, H;            // window size in DIPs

bool init(HWND hwnd);
void shutdown();
void on_resize();
void set_dpi(float dpi);
bool begin_frame();           // false if the device is gone (recreated on the next frame)
void end_frame();             // presents; schedules another frame while animations run
void request_frame();         // something changed mid-frame: draw again

// drawing
void fill(Rc r, u32 c, float radius = 0);
void stroke(Rc r, u32 c, float radius = 0, float width = 1);
void circle(float cx, float cy, float rad, u32 c);
void line(float x0, float y0, float x1, float y1, u32 c, float width = 1);
void text(const std::wstring& s, Rc r, Font f, u32 c, Align a = A_LEFT, bool wrap = false);
void icon(wchar_t g, Rc r, u32 c, Font f = F_ICON);
float text_width(const std::wstring& s, Font f);
float text_height(const std::wstring& s, Font f, float width); // wrapped to width
void push_clip(Rc r);
void pop_clip();

// interaction (ids are any unique ints; a popup blocks everything else while open)
bool hovered(Rc r);
bool clicked(int id, Rc r);   // pressed and released over r
bool held(int id);            // pressed on id and still down
float anim(int id, float target, float rate = 16.0f); // eased value that follows target
void state_layer(Rc r, int id, u32 on_color, float radius); // M3 hover/press overlay

// widgets
bool button_filled(int id, Rc r, const std::wstring& label, wchar_t glyph = 0, bool enabled = true);
bool button_tonal(int id, Rc r, const std::wstring& label, wchar_t glyph = 0, bool enabled = true);
bool button_outlined(int id, Rc r, const std::wstring& label, wchar_t glyph = 0, bool enabled = true);
bool button_text(int id, Rc r, const std::wstring& label, wchar_t glyph = 0, bool enabled = true);
bool icon_button(int id, Rc r, wchar_t glyph, u32 hover_bg = 0);
bool toggle(int id, Rc r, bool& v, bool enabled = true);                 // M3 switch, 52x32
bool segmented(int id, Rc r, const std::vector<std::wstring>& items, int& v, bool enabled = true);
bool dropdown(int id, Rc r, const std::vector<std::wstring>& items, int& v, bool enabled = true);
bool slider(int id, Rc r, int& v, int lo, int hi, bool enabled = true);
float segmented_width(const std::vector<std::wstring>& items);
void popups();                // call last: draws the open dropdown menu and handles its clicks
bool popup_open();
void close_popup();

// test harness: draw frames into a PNG instead of the window (frame() runs begin/draw/end itself)
bool snapshot(const std::wstring& png, void (*frame)(), int frames = 1);
Rc last_rect(int id);         // where a widget was drawn in the previous frame
} // namespace ui
