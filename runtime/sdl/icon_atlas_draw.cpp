// icon_atlas_draw.cpp - the button glyph atlas (icon_atlas.h) in the SDL build, drawn by hand: the shapes of
// icon_atlas.cpp (which uses GDI+ and Arial Black on Windows) and a small bold stroke alphabet holding the letters
// the prompts need, anti-aliased by their distance to each pixel.
#include "../icon_atlas.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

struct Color { float r, g, b, a; }; // straight alpha
Color rgb(int r, int g, int b, int a = 255) { return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f}; }

struct Seg { float x0, y0, x1, y1; };

float seg_dist(const Seg& s, float x, float y) {
    const float dx = s.x1 - s.x0, dy = s.y1 - s.y0, len2 = dx * dx + dy * dy;
    const float t = len2 > 0 ? std::clamp(((x - s.x0) * dx + (y - s.y0) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(x - (s.x0 + t * dx), y - (s.y0 + t * dy));
}

// ------------------------------------------------------------ the alphabet
// letters as centre lines on a cap height of 1 (y down), drawn with round ends STROKE either side; each designed
// `w` wide and drawn `out` wide (Arial Black's proportions: its capitals are broad)
const float STROKE = 0.17f, GAP = 0.1f;
struct Letter { char c; float w, out; std::vector<Seg> segs; };

void poly(std::vector<Seg>& s, std::initializer_list<std::pair<float, float>> pts) {
    const std::pair<float, float>* prev = nullptr;
    for (const auto& p : pts) {
        if (prev) s.push_back({prev->first, prev->second, p.first, p.second});
        prev = &p;
    }
}
void arc(std::vector<Seg>& s, float cx, float cy, float r, float deg0, float deg1) {
    const int n = 16;
    for (int i = 0; i < n; i++) {
        const float a = (deg0 + (deg1 - deg0) * i / n) * 3.14159265f / 180, b = (deg0 + (deg1 - deg0) * (i + 1) / n) * 3.14159265f / 180;
        s.push_back({cx + r * std::cos(a), cy + r * std::sin(a), cx + r * std::cos(b), cy + r * std::sin(b)});
    }
}

const std::vector<Letter>& alphabet() {
    static std::vector<Letter> v;
    if (!v.empty()) return v;
    auto add = [&](char c, float w, float out) { v.push_back({c, w, out, {}}); return &v.back().segs; };
    std::vector<Seg>* s;
    s = add('A', 0.78f, 1.11f); poly(*s, {{0, 1}, {0.39f, 0}, {0.78f, 1}}); poly(*s, {{0.17f, 0.64f}, {0.61f, 0.64f}});
    s = add('B', 0.6f, 0.8f); poly(*s, {{0.3f, 0}, {0, 0}, {0, 1}, {0.33f, 1}}); arc(*s, 0.3f, 0.235f, 0.235f, -90, 90);
    poly(*s, {{0, 0.47f}, {0.33f, 0.47f}}); arc(*s, 0.33f, 0.735f, 0.265f, -90, 90);
    s = add('E', 0.55f, 0.68f); poly(*s, {{0.55f, 0}, {0, 0}, {0, 1}, {0.55f, 1}}); poly(*s, {{0, 0.5f}, {0.46f, 0.5f}});
    s = add('N', 0.66f, 0.82f); poly(*s, {{0, 1}, {0, 0}, {0.66f, 1}, {0.66f, 0}});
    s = add('R', 0.62f, 0.8f); poly(*s, {{0, 1}, {0, 0}, {0.3f, 0}}); arc(*s, 0.3f, 0.25f, 0.25f, -90, 90);
    poly(*s, {{0.3f, 0.5f}, {0, 0.5f}}); poly(*s, {{0.24f, 0.5f}, {0.62f, 1}});
    s = add('S', 0.58f, 0.87f); arc(*s, 0.29f, 0.25f, 0.25f, -20, -270); arc(*s, 0.29f, 0.75f, 0.25f, -90, 160);
    s = add('T', 0.6f, 0.78f); poly(*s, {{0, 0}, {0.6f, 0}}); poly(*s, {{0.3f, 0}, {0.3f, 1}});
    s = add('X', 0.66f, 1.11f); poly(*s, {{0, 0}, {0.66f, 1}}); poly(*s, {{0.66f, 0}, {0, 1}});
    s = add('Y', 0.68f, 1.05f); poly(*s, {{0, 0}, {0.34f, 0.52f}, {0.68f, 0}}); poly(*s, {{0.34f, 0.52f}, {0.34f, 1}});
    s = add('Z', 0.6f, 0.93f); poly(*s, {{0, 0}, {0.6f, 0}, {0, 1}, {0.6f, 1}});
    return v;
}

// ------------------------------------------------------------ a cell
// shapes are given in "screen space" (aspect * ICON_CELL wide), squeezed into the square cell like the game's texels
struct Cell {
    std::vector<float> px = std::vector<float>((size_t)ICON_CELL * ICON_CELL * 4); // premultiplied
    float aspect = 1;

    // over-composite colour c where dist(x, y) (signed, screen pixels, < 0 inside) puts it
    template <class F> void paint(const Color& c, F dist) {
        for (int y = 0; y < ICON_CELL; y++)
            for (int x = 0; x < ICON_CELL; x++) {
                const float a = std::clamp(0.5f - dist((x + 0.5f) * aspect, y + 0.5f), 0.0f, 1.0f) * c.a;
                if (a <= 0) continue;
                float* p = &px[((size_t)y * ICON_CELL + x) * 4];
                p[0] = c.r * a + p[0] * (1 - a);
                p[1] = c.g * a + p[1] * (1 - a);
                p[2] = c.b * a + p[2] * (1 - a);
                p[3] = a + p[3] * (1 - a);
            }
    }
};

// signed distance to an ellipse (x, y = its box's corner), to first order
float ellipse(float px, float py, float x, float y, float w, float h) {
    const float rx = w / 2, ry = h / 2, dx = px - (x + rx), dy = py - (y + ry);
    const float k = std::sqrt(dx * dx / (rx * rx) + dy * dy / (ry * ry));
    if (k < 1e-6f) return -std::min(rx, ry);
    const float g = std::sqrt(dx * dx / (rx * rx * rx * rx) + dy * dy / (ry * ry * ry * ry)) / k;
    return (k - 1) / g;
}

float rounded_rect(float px, float py, float x, float y, float w, float h, float r) {
    const float qx = std::fabs(px - (x + w / 2)) - (w / 2 - r), qy = std::fabs(py - (y + h / 2)) - (h / 2 - r);
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

// text centred on cx, cy; size = the em size GDI+ draws Arial Black with (its capitals are 0.716 of it)
void label(Cell& cell, const char* text, float cx, float cy, float size, Color fill, Color outline, float outline_w) {
    const float s = 0.716f * size / (1 + 2 * STROKE), t = STROKE * s;
    std::vector<Seg> segs;
    float pen = 0;
    for (const char* p = text; *p; p++)
        for (const Letter& l : alphabet())
            if (l.c == *p) {
                const float k = l.out / l.w * s;
                for (const Seg& g : l.segs) segs.push_back({pen + g.x0 * k, g.y0 * s, pen + g.x1 * k, g.y1 * s});
                pen += (l.out + 2 * STROKE + GAP) * s;
                break;
            }
    if (segs.empty()) return;
    const float width = pen - GAP * s - 2 * t; // centre line extent; the strokes add t on every side
    for (Seg& g : segs) { g.x0 += cx - width / 2; g.x1 += cx - width / 2; g.y0 += cy - s / 2; g.y1 += cy - s / 2; }
    auto dist = [&](float x, float y) {
        float d = 1e9f;
        for (const Seg& g : segs) d = std::min(d, seg_dist(g, x, y));
        return d;
    };
    if (outline_w > 0) cell.paint(outline, [&](float x, float y) { return dist(x, y) - t - outline_w / 2; });
    cell.paint(fill, [&](float x, float y) { return dist(x, y) - t; });
}

// Xbox: dark round button, coloured letter (like the controller), Menu button for Start
bool draw_xbox(Cell& c, int id, float w, float h) {
    const float bw = id == ICON_START ? std::min(w, h) - 16 : w - 16, bh = h - 16, x = (w - bw) / 2, y = (h - bh) / 2;
    const float d = std::min(bw, bh);
    const Color dark = rgb(10, 10, 10);
    switch (id) {
    case ICON_CROSS: case ICON_CIRCLE: case ICON_SQUARE: case ICON_TRIANGLE: case ICON_START: break;
    default: return false;
    }
    c.paint(rgb(52, 52, 56), [&](float px, float py) { return ellipse(px, py, x, y, bw, bh); });
    c.paint(rgb(12, 12, 12), [&](float px, float py) { return std::fabs(ellipse(px, py, x, y, bw, bh)) - 6; });
    c.paint(rgb(255, 255, 255, 60), [&](float px, float py) { return ellipse(px, py, x + bw * 0.18f, y + bh * 0.08f, bw * 0.64f, bh * 0.30f); });
    switch (id) {
    case ICON_CROSS: label(c, "A", w / 2, h / 2, 150, rgb(110, 200, 50), dark, 14); break;
    case ICON_CIRCLE: label(c, "B", w / 2, h / 2, 150, rgb(235, 60, 50), dark, 14); break;
    case ICON_SQUARE: label(c, "X", w / 2, h / 2, 150, rgb(50, 130, 240), dark, 14); break;
    case ICON_TRIANGLE: label(c, "Y", w / 2, h / 2, 150, rgb(250, 200, 30), dark, 14); break;
    case ICON_START: { // "Menu": three bars
        const float lw = d * 0.46f, lh = d * 0.085f;
        for (int k = -1; k <= 1; k++)
            c.paint(rgb(240, 240, 240), [&](float px, float py) { return rounded_rect(px, py, w / 2 - lw / 2, h / 2 + k * d * 0.17f - lh / 2, lw, lh, lh / 2); });
        break;
    }
    }
    return true;
}

// keyboard: light key cap with the key that is mapped to the button
bool draw_key(Cell& c, int id, float w, float h) {
    const char* text = nullptr;
    float size = 150;
    switch (id) {
    case ICON_CROSS: text = "Z"; break;
    case ICON_CIRCLE: text = "X"; break;
    case ICON_SQUARE: text = "A"; break;
    case ICON_TRIANGLE: text = "S"; break;
    case ICON_START: text = "ENTER"; size = 84; break;
    default: return false;
    }
    const float m = 14, x = m, y = m, cw = w - 2 * m, ch = h - 2 * m, r = 40;
    c.paint(rgb(150, 150, 158), [&](float px, float py) { return rounded_rect(px, py, x, y, cw, ch, r); });
    c.paint(rgb(238, 238, 242), [&](float px, float py) { return rounded_rect(px, py, x + 14, y + 10, cw - 28, ch - 40, r - 10); });
    c.paint(rgb(16, 16, 20), [&](float px, float py) { return std::fabs(rounded_rect(px, py, x, y, cw, ch, r)) - 5; });
    label(c, text, w / 2, y + 10 + (ch - 40) / 2, size, rgb(24, 24, 28), Color{0, 0, 0, 0}, 0);
    return true;
}

} // namespace

void icon_atlas_build() {
    const int W = ICON_COUNT * ICON_CELL;
    std::vector<u32> atlas((size_t)W * ICON_ROWS * ICON_CELL);
    bool has[ICON_ROWS][ICON_COUNT] = {};
    for (int row = 0; row < ICON_ROWS; row++)
        for (int id = 0; id < ICON_COUNT; id++) {
            Cell c;
            c.aspect = icon_aspect(id);
            if (!(has[row][id] = (row == 0 ? draw_xbox : draw_key)(c, id, ICON_CELL * c.aspect, (float)ICON_CELL))) continue;
            for (int y = 0; y < ICON_CELL; y++)
                for (int x = 0; x < ICON_CELL; x++) {
                    const float* p = &c.px[((size_t)y * ICON_CELL + x) * 4];
                    auto q = [](float v) { return (u32)std::lround(std::clamp(v, 0.0f, 1.0f) * 255); };
                    atlas[(size_t)(row * ICON_CELL + y) * W + id * ICON_CELL + x] = q(p[2]) | q(p[1]) << 8 | q(p[0]) << 16 | q(p[3]) << 24;
                }
        }
    icon_atlas_set(std::move(atlas), has);
}
