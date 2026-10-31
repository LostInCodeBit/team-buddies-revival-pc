// icon_atlas_common.cpp - the button glyph atlas (icon_atlas.h) once painted: where its cells are and what the
// software renderer samples from them.
#include "icon_atlas.h"
#include <cstring>

static std::vector<u32> s_atlas; // premultiplied BGRA, (ICON_COUNT * ICON_CELL) x (ICON_ROWS * ICON_CELL)
static bool s_has[ICON_ROWS][ICON_COUNT];

void write_png(const std::string& path, const u8* rgba, int w, int h, int pitch); // platform_common.cpp

void icon_atlas_set(std::vector<u32>&& pixels, const bool (&has)[ICON_ROWS][ICON_COUNT]) {
    s_atlas = std::move(pixels);
    memcpy(s_has, has, sizeof s_has);
    // debugging aid: TB_ICON_DUMP=<file.png> saves the atlas (on black)
    if (const char* f = getenv("TB_ICON_DUMP")) {
        std::vector<u32> rgba(s_atlas.size());
        for (size_t i = 0; i < s_atlas.size(); i++) {
            const u32 p = s_atlas[i];
            rgba[i] = ((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16) | 0xFF000000u;
        }
        write_png(f, (const u8*)rgba.data(), ICON_COUNT * ICON_CELL, ICON_ROWS * ICON_CELL, ICON_COUNT * ICON_CELL * 4);
    }
}

const u32* icon_atlas_pixels(int& w, int& h) {
    w = ICON_COUNT * ICON_CELL;
    h = ICON_ROWS * ICON_CELL;
    return s_atlas.empty() ? nullptr : s_atlas.data();
}

int icon_slot(InputDevice dev, IconId id) {
    if (id == ICON_NONE || s_atlas.empty()) return -1;
    int row = dev == DEV_XBOX ? 0 : dev == DEV_KEYBOARD ? 1 : -1;
    if (row < 0 || !s_has[row][id]) return -1;
    return row * ICON_COUNT + id;
}

// software renderer: box-filtered texel (u, v) of a w x h sprite mapped onto the slot's cell, as 15-bit color
u16 icon_sample_555(int slot, int u, int v, int w, int h) {
    int W = ICON_COUNT * ICON_CELL;
    int cx = (slot % ICON_COUNT) * ICON_CELL, cy = (slot / ICON_COUNT) * ICON_CELL;
    int x0 = cx + u * ICON_CELL / w, x1 = cx + (u + 1) * ICON_CELL / w, y0 = cy + v * ICON_CELL / h, y1 = cy + (v + 1) * ICON_CELL / h;
    u32 sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
    for (int y = y0; y < y1; y += 2)
        for (int x = x0; x < x1; x += 2) {
            u32 p = s_atlas[(size_t)y * W + x];
            sb += p & 0xFF; sg += (p >> 8) & 0xFF; sr += (p >> 16) & 0xFF; sa += p >> 24; n++;
        }
    if (!n || sa < n * 128) return 0; // transparent
    u32 r = sr * 255 / sa, g = sg * 255 / sa, b = sb * 255 / sa;
    u16 c = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    return c ? c : 0x0421; // black would read as "transparent" on the PS1
}
