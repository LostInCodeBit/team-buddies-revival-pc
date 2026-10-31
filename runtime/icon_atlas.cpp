// icon_atlas.cpp - high-resolution glyphs that replace the game's PlayStation button prompts when the
// player uses a keyboard or an Xbox-style controller. Drawn once at startup with GDI+ into one atlas
// (icon_atlas.h): row 0 = Xbox, row 1 = keyboard, one 256x256 cell per IconId, premultiplied BGRA.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "icon_atlas.h"
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

static const int CELL = ICON_CELL, ROWS = ICON_ROWS;

static void rounded_rect(GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static void label(Graphics& g, const wchar_t* text, float cx, float cy, float size, Color fill, Color outline, float outline_w) {
    FontFamily fam(L"Arial Black");
    const FontFamily* use = fam.IsAvailable() ? &fam : FontFamily::GenericSansSerif();
    GraphicsPath p;
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    p.AddString(text, -1, use, fam.IsAvailable() ? FontStyleRegular : FontStyleBold, size, RectF(cx - 400, cy - 200, 800, 400), &sf);
    // optical centring: put the glyph box itself in the middle
    RectF b;
    p.GetBounds(&b);
    Matrix m;
    m.Translate(cx - (b.X + b.Width / 2), cy - (b.Y + b.Height / 2));
    p.Transform(&m);
    if (outline_w > 0) {
        Pen pen(outline, outline_w);
        pen.SetLineJoin(LineJoinRound);
        g.DrawPath(&pen, &p);
    }
    SolidBrush br(fill);
    g.FillPath(&br, &p);
}

// Xbox: dark round button, coloured letter (like the controller), Menu button for Start
static void draw_xbox(Graphics& g, int id, float w, float h) {
    // body fills the sprite like the game's own oval icons; Start keeps a round button
    float bw = id == ICON_START ? std::min(w, h) - 16 : w - 16, bh = h - 16, x = (w - bw) / 2, y = (h - bh) / 2;
    float d = std::min(bw, bh);
    SolidBrush body(Color(255, 52, 52, 56));
    Pen rim(Color(255, 12, 12, 12), 12);
    g.FillEllipse(&body, x, y, bw, bh);
    g.DrawEllipse(&rim, x, y, bw, bh);
    SolidBrush shine(Color(60, 255, 255, 255));
    g.FillEllipse(&shine, x + bw * 0.18f, y + bh * 0.08f, bw * 0.64f, bh * 0.30f);
    Color dark(255, 10, 10, 10);
    switch (id) {
    case ICON_CROSS: label(g, L"A", w / 2, h / 2, 150, Color(255, 110, 200, 50), dark, 14); break;
    case ICON_CIRCLE: label(g, L"B", w / 2, h / 2, 150, Color(255, 235, 60, 50), dark, 14); break;
    case ICON_SQUARE: label(g, L"X", w / 2, h / 2, 150, Color(255, 50, 130, 240), dark, 14); break;
    case ICON_TRIANGLE: label(g, L"Y", w / 2, h / 2, 150, Color(255, 250, 200, 30), dark, 14); break;
    case ICON_START: { // "Menu": three bars
        SolidBrush white(Color(255, 240, 240, 240));
        float bw = d * 0.46f, bh = d * 0.085f;
        for (int k = -1; k <= 1; k++) {
            GraphicsPath p;
            rounded_rect(p, w / 2 - bw / 2, h / 2 + k * d * 0.17f - bh / 2, bw, bh, bh / 2);
            g.FillPath(&white, &p);
        }
        break;
    }
    }
}

// keyboard: light key cap with the key that is mapped to the button
static void draw_key(Graphics& g, int id, float w, float h) {
    const wchar_t* text = nullptr;
    float size = 150;
    switch (id) {
    case ICON_CROSS: text = L"Z"; break;
    case ICON_CIRCLE: text = L"X"; break;
    case ICON_SQUARE: text = L"A"; break;
    case ICON_TRIANGLE: text = L"S"; break;
    case ICON_START: text = L"ENTER"; size = 84; break;
    default: return;
    }
    float m = 14, x = m, y = m, cw = w - 2 * m, ch = h - 2 * m, r = 40;
    GraphicsPath base, top;
    rounded_rect(base, x, y, cw, ch, r);
    rounded_rect(top, x + 14, y + 10, cw - 28, ch - 40, r - 10);
    SolidBrush side(Color(255, 150, 150, 158)), face(Color(255, 238, 238, 242));
    Pen rim(Color(255, 16, 16, 20), 10);
    g.FillPath(&side, &base);
    g.FillPath(&face, &top);
    g.DrawPath(&rim, &base);
    label(g, text, w / 2, y + 10 + (ch - 40) / 2, size, Color(255, 24, 24, 28), Color(0, 0, 0, 0), 0);
}

void icon_atlas_build() {
    ULONG_PTR token;
    GdiplusStartupInput in;
    if (GdiplusStartup(&token, &in, nullptr) != Ok) { plog("icons: GDI+ unavailable, keeping PlayStation prompts\n"); return; }
    bool has[ROWS][ICON_COUNT] = {};
    {
        int W = ICON_COUNT * CELL, H = ROWS * CELL;
        Bitmap bmp(W, H, PixelFormat32bppPARGB);
        Graphics g(&bmp);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        for (int row = 0; row < ROWS; row++)
            for (int id = 0; id < ICON_COUNT; id++) {
                // draw in "screen space" (aspect * CELL wide), squeezed into the square cell like the game's texels
                float a = icon_aspect(id), w = CELL * a, h = CELL;
                GraphicsState saved = g.Save();
                g.SetClip(RectF((float)(id * CELL), (float)(row * CELL), (float)CELL, (float)CELL));
                g.TranslateTransform((float)(id * CELL), (float)(row * CELL));
                g.ScaleTransform(1.0f / a, 1.0f);
                bool known = id <= ICON_START;
                if (known) (row == 0 ? draw_xbox : draw_key)(g, id, w, h);
                has[row][id] = known;
                g.Restore(saved);
            }
        BitmapData bd;
        Rect r(0, 0, W, H);
        if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Ok) {
            std::vector<u32> px((size_t)W * H);
            for (int y = 0; y < H; y++) memcpy(&px[(size_t)y * W], (const u8*)bd.Scan0 + (size_t)y * bd.Stride, (size_t)W * 4);
            bmp.UnlockBits(&bd);
            icon_atlas_set(std::move(px), has);
        }
    }
    GdiplusShutdown(token);
}
