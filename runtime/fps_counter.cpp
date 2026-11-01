// fps_counter.cpp - the FPS counter (show_fps, Shift+F10): the frames presented per second (in-between frames
// included) and the game's own frames per second (its buffer flips; 30 in battle, lower when it slows down). Both
// are measured over half-second windows and drawn into a small chip with a 5x7 pixel font, which the presenters
// put over the window's top-left corner, where the battle HUD leaves room (screenshots include it).
#include "platform_common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------- measuring
// events per second between the first and the last event of a window, so a steady rate reads exactly
struct Rate {
    long long first = 0, last = 0;
    int n = -1; // intervals since `first` (-1: no event yet)
    void hit(long long t) {
        if (n < 0) { first = t; n = 0; }
        else { n++; last = t; }
    }
    // the rate, then a new window from the last event; -1 = too few events yet (keep the value shown)
    int take(long long now, long long hz) {
        if (n >= 1 && last > first) {
            int r = (int)std::lround((double)n * (double)hz / (double)(last - first));
            first = last;
            n = 0;
            return r;
        }
        if (n < 0 || now - first > hz) { n = -1; return 0; } // nothing for a second: stopped
        return -1;
    }
};

static Rate s_presents, s_flips;
static int s_fps = -1, s_game = -1; // -1: not measured yet

void fps_game_frame() { s_flips.hit(plat_ticks()); }

void fps_frame_presented() {
    static const long long hz = plat_tick_hz();
    const long long now = plat_ticks();
    s_presents.hit(now);
    if (s_presents.n < 1 || now - s_presents.first < hz / 2) return;
    s_fps = s_presents.take(now, hz);
    int g = s_flips.take(now, hz);
    if (g >= 0) s_game = g;
}

// ---------------------------------------------------------------- drawing
// 5x7 glyphs, one byte per row from the top, bit 4 = left column
static const char k_chars[] = "0123456789FPSGAME-";
static const u8 k_font[][7] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 0 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}, // 2 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 4 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 6 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 8 9
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // F P
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, // S G
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, // A M
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, // E -
};
// layout in font pixels: a label column ("FPS", "GAME") and a right-aligned value column, two lines
const int ADV = 6, GLYPH_H = 7, PAD_X = 4, PAD_Y = 3, LINE_GAP = 3, COL_GAP = 5, RADIUS = 3, MARGIN = 4;
static int text_w(const char* s) { return std::max(0, (int)strlen(s) * ADV - 1); }

static std::vector<u32> s_pixels;
static FpsOverlay s_overlay;
static int s_drawn_fps = -2, s_drawn_game = -2, s_drawn_unit;

static u32 premul(int r, int g, int b, int a) {
    return (u32)(r * a / 255) | (u32)(g * a / 255) << 8 | (u32)(b * a / 255) << 16 | (u32)a << 24;
}

static void draw_text(const char* s, int x, int y, int unit, u32 color) {
    for (; *s; s++, x += ADV * unit) {
        const char* c = strchr(k_chars, *s);
        if (!c) continue;
        const u8* rows = k_font[c - k_chars];
        for (int gy = 0; gy < GLYPH_H; gy++)
            for (int gx = 0; gx < 5; gx++) {
                if (!(rows[gy] & (0x10 >> gx))) continue;
                for (int py = 0; py < unit; py++) {
                    u32* row = &s_pixels[(size_t)(y + gy * unit + py) * s_overlay.w + x + gx * unit];
                    std::fill(row, row + unit, color);
                }
            }
    }
}

const FpsOverlay* fps_overlay(int window_h) {
    if (!g_settings.show_fps || window_h <= 0) return nullptr;
    const int unit = std::max(1, (window_h + 240) / 480); // 2 font pixels per screen pixel at 720 to 1199 lines
    if (unit == s_drawn_unit && s_fps == s_drawn_fps && s_game == s_drawn_game) return &s_overlay;
    s_drawn_unit = unit;
    s_drawn_fps = s_fps;
    s_drawn_game = s_game;
    char fps[8] = "--", game[8] = "--";
    if (s_fps >= 0) snprintf(fps, sizeof fps, "%d", std::min(s_fps, 9999));
    if (s_game >= 0) snprintf(game, sizeof game, "%d", std::min(s_game, 9999));
    const int label_w = text_w("GAME"), value_w = std::max({text_w("000"), text_w(fps), text_w(game)});
    s_overlay.w = (PAD_X + label_w + COL_GAP + value_w + PAD_X) * unit;
    s_overlay.h = (PAD_Y + GLYPH_H + LINE_GAP + GLYPH_H + PAD_Y) * unit;
    s_overlay.x = s_overlay.y = MARGIN * unit;
    s_pixels.assign((size_t)s_overlay.w * s_overlay.h, 0);
    // the chip: dark, 80% opaque (light text stays readable over the brightest grass), rounded corners
    const float r = (float)(RADIUS * unit);
    for (int y = 0; y < s_overlay.h; y++)
        for (int x = 0; x < s_overlay.w; x++) {
            float cx = x + 0.5f, cy = y + 0.5f;
            float dx = std::max({r - cx, cx - (s_overlay.w - r), 0.0f}), dy = std::max({r - cy, cy - (s_overlay.h - r), 0.0f});
            float cover = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            s_pixels[(size_t)y * s_overlay.w + x] = premul(16, 16, 16, (int)std::lround(204 * cover));
        }
    const u32 label = premul(180, 180, 180, 255), value = premul(255, 255, 255, 255);
    const int x0 = PAD_X * unit, x1 = (PAD_X + label_w + COL_GAP + value_w) * unit;
    const int y0 = PAD_Y * unit, y1 = (PAD_Y + GLYPH_H + LINE_GAP) * unit;
    draw_text("FPS", x0, y0, unit, label);
    draw_text(fps, x1 - text_w(fps) * unit, y0, unit, value);
    draw_text("GAME", x0, y1, unit, label);
    draw_text(game, x1 - text_w(game) * unit, y1, unit, value);
    s_overlay.rgba = s_pixels.data();
    s_overlay.version++;
    return &s_overlay;
}
