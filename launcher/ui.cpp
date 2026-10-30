// ui.cpp - Direct2D/DirectWrite drawing, input routing and Material 3 widgets for the launcher.
#include "ui.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace ui {
Input in;
float W = 0, H = 0;

static HWND s_hwnd;
static ID2D1Factory* s_d2d;
static IDWriteFactory* s_dw;
static ID2D1HwndRenderTarget* s_hwnd_rt;
static ID2D1RenderTarget* s_rt;   // the window's target, or a WIC bitmap while taking a snapshot
static bool s_snap;
static std::unordered_map<int, Rc> s_rects;
static ID2D1SolidColorBrush* s_brush;
static IDWriteTextFormat* s_fmt[F_COUNT];
static IDWriteInlineObject* s_ellipsis[F_COUNT];
static float s_dpi = 96;
static bool s_animating, s_again;
static int s_active;            // widget the mouse went down on
static double s_dt = 1.0 / 60, s_last_t;
static std::unordered_map<int, float> s_anim;

struct Popup {
    bool open = false, drawing = false;
    int id = 0;
    Rc anchor;
    std::vector<std::wstring> items;
    int selected = -1;
    int result_id = 0, result = -1; // choice made, delivered to dropdown(id) on the next frame
} static s_pop;

template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static D2D1_COLOR_F c2d(u32 c) {
    return D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, ((c >> 24) & 255) / 255.0f);
}
static D2D1_RECT_F r2d(Rc r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }

static std::wstring pick_family(IDWriteFontCollection* fc, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names) {
        UINT32 idx;
        BOOL found = FALSE;
        if (SUCCEEDED(fc->FindFamilyName(n, &idx, &found)) && found) return n;
    }
    return L"Segoe UI";
}

static bool create_target() {
    RECT rc;
    GetClientRect(s_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties();
    p.dpiX = p.dpiY = s_dpi;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(s_hwnd, D2D1::SizeU(rc.right, rc.bottom));
    if (FAILED(s_d2d->CreateHwndRenderTarget(p, hp, &s_hwnd_rt))) return false;
    s_rt = s_hwnd_rt;
    s_rt->CreateSolidColorBrush(D2D1::ColorF(0), &s_brush);
    s_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

bool init(HWND hwnd) {
    s_hwnd = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &s_d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&s_dw))) return false;
    IDWriteFontCollection* fc = nullptr;
    s_dw->GetSystemFontCollection(&fc);
    std::wstring textf = pick_family(fc, {L"Segoe UI Variable Text", L"Segoe UI"});
    std::wstring dispf = pick_family(fc, {L"Segoe UI Variable Display", L"Segoe UI"});
    std::wstring markf = pick_family(fc, {L"Segoe UI Black", L"Segoe UI"});
    std::wstring iconf = pick_family(fc, {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    fc->Release();
    struct Spec { const std::wstring* fam; float size; DWRITE_FONT_WEIGHT w; };
    const Spec spec[F_COUNT] = {
        {&textf, 15, DWRITE_FONT_WEIGHT_NORMAL},    // F_BODY
        {&textf, 13, DWRITE_FONT_WEIGHT_NORMAL},    // F_BODY_SMALL
        {&textf, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD}, // F_LABEL
        {&textf, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD}, // F_LABEL_SMALL
        {&textf, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD}, // F_TITLE
        {&dispf, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD}, // F_TITLE_LARGE
        {&dispf, 30, DWRITE_FONT_WEIGHT_SEMI_BOLD}, // F_HEADLINE
        {&dispf, 42, DWRITE_FONT_WEIGHT_BOLD},      // F_DISPLAY
        {&markf, 27, DWRITE_FONT_WEIGHT_BLACK},     // F_WORDMARK
        {&iconf, 16, DWRITE_FONT_WEIGHT_NORMAL},    // F_ICON
        {&iconf, 20, DWRITE_FONT_WEIGHT_NORMAL},    // F_ICON_LARGE
    };
    for (int i = 0; i < F_COUNT; i++) {
        if (FAILED(s_dw->CreateTextFormat(spec[i].fam->c_str(), nullptr, spec[i].w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          spec[i].size, L"en-us", &s_fmt[i])))
            return false;
        s_dw->CreateEllipsisTrimmingSign(s_fmt[i], &s_ellipsis[i]);
    }
    UINT dpi = GetDpiForWindow(hwnd);
    s_dpi = dpi ? (float)dpi : 96.0f;
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    s_last_t = (double)t.QuadPart / f.QuadPart;
    return create_target();
}

void shutdown() {
    for (int i = 0; i < F_COUNT; i++) { release(s_ellipsis[i]); release(s_fmt[i]); }
    release(s_brush);
    release(s_hwnd_rt);
    s_rt = nullptr;
    release(s_dw);
    release(s_d2d);
}

void on_resize() {
    if (!s_hwnd_rt) return;
    RECT rc;
    GetClientRect(s_hwnd, &rc);
    s_hwnd_rt->Resize(D2D1::SizeU(rc.right, rc.bottom));
}

void set_dpi(float dpi) {
    s_dpi = dpi;
    if (s_hwnd_rt) s_hwnd_rt->SetDpi(dpi, dpi);
    on_resize();
}

bool begin_frame() {
    if (!s_rt && !create_target()) return false;
    if (s_snap) { // fixed size, no window involved
        s_dt = 1.0;
        s_animating = s_again = false;
        s_rt->BeginDraw();
        s_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        s_rt->Clear(c2d(col::surface));
        return true;
    }
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    double now = (double)t.QuadPart / f.QuadPart;
    s_dt = std::clamp(now - s_last_t, 0.0, 0.1);
    s_last_t = now;
    s_animating = s_again = false;
    RECT rc;
    GetClientRect(s_hwnd, &rc);
    W = rc.right * 96.0f / s_dpi;
    H = rc.bottom * 96.0f / s_dpi;
    s_rt->BeginDraw();
    s_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    s_rt->Clear(c2d(col::surface));
    if (s_pop.open && in.escape) s_pop.open = false;
    return true;
}

void end_frame() {
    HRESULT hr = s_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET && !s_snap) { release(s_brush); release(s_hwnd_rt); s_rt = nullptr; s_again = true; }
    if (in.released) s_active = 0;
    in.pressed = in.released = in.enter = in.escape = false;
    in.wheel = 0;
    if (!s_snap && (s_animating || s_again)) InvalidateRect(s_hwnd, nullptr, FALSE);
}

Rc last_rect(int id) { auto it = s_rects.find(id); return it == s_rects.end() ? Rc{} : it->second; }

bool snapshot(const std::wstring& png, void (*frame)(), int frames) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    const float dpi = 144; // 1.5x, crisp enough to read and still small
    UINT pw = (UINT)std::lround(W * dpi / 96), ph = (UINT)std::lround(H * dpi / 96);
    IWICBitmap* bmp = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    bool ok = SUCCEEDED(wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp));
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    ok = ok && SUCCEEDED(s_d2d->CreateWicBitmapRenderTarget(bmp, p, &rt));
    ok = ok && SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0), &brush));
    if (ok) {
        ID2D1RenderTarget* saved_rt = s_rt;
        ID2D1SolidColorBrush* saved_brush = s_brush;
        s_rt = rt;
        s_brush = brush;
        s_snap = true;
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        for (int i = 0; i < frames; i++) frame();
        s_snap = false;
        s_rt = saved_rt;
        s_brush = saved_brush;
        IWICStream* st = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* fr = nullptr;
        ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(png.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
             SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->WriteSource(bmp, nullptr)) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        release(fr);
        release(enc);
        release(st);
    }
    release(brush);
    release(rt);
    release(bmp);
    release(wic);
    return ok;
}

void request_frame() { s_again = true; }

// ---------------------------------------------------------------- drawing
void fill(Rc r, u32 c, float radius) {
    s_brush->SetColor(c2d(c));
    if (radius > 0) s_rt->FillRoundedRectangle(D2D1::RoundedRect(r2d(r), radius, radius), s_brush);
    else s_rt->FillRectangle(r2d(r), s_brush);
}

void stroke(Rc r, u32 c, float radius, float width) {
    s_brush->SetColor(c2d(c));
    Rc q = r.inset(width / 2, width / 2);
    if (radius > 0) s_rt->DrawRoundedRectangle(D2D1::RoundedRect(r2d(q), radius - width / 2, radius - width / 2), s_brush, width);
    else s_rt->DrawRectangle(r2d(q), s_brush, width);
}

void circle(float cx, float cy, float rad, u32 c) {
    s_brush->SetColor(c2d(c));
    s_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), s_brush);
}

void line(float x0, float y0, float x1, float y1, u32 c, float width) {
    s_brush->SetColor(c2d(c));
    s_rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), s_brush, width);
}

void text(const std::wstring& s, Rc r, Font f, u32 c, Align a, bool wrap) {
    IDWriteTextFormat* fmt = s_fmt[f];
    fmt->SetTextAlignment(a == A_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : a == A_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim = {wrap ? DWRITE_TRIMMING_GRANULARITY_NONE : DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    fmt->SetTrimming(&trim, wrap ? nullptr : s_ellipsis[f]);
    s_brush->SetColor(c2d(c));
    s_rt->DrawText(s.c_str(), (UINT32)s.size(), fmt, r2d(r), s_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void icon(wchar_t g, Rc r, u32 c, Font f) {
    wchar_t s[2] = {g, 0};
    text(s, r, f, c, A_CENTER);
}

static IDWriteTextLayout* layout(const std::wstring& s, Font f, float width, bool wrap) {
    IDWriteTextFormat* fmt = s_fmt[f];
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    fmt->SetTrimming(&trim, nullptr);
    IDWriteTextLayout* l = nullptr;
    s_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, width, 10000, &l);
    return l;
}

float text_width(const std::wstring& s, Font f) {
    IDWriteTextLayout* l = layout(s, f, 10000, false);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

float text_height(const std::wstring& s, Font f, float width) {
    IDWriteTextLayout* l = layout(s, f, width, true);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    l->Release();
    return m.height;
}

void push_clip(Rc r) { s_rt->PushAxisAlignedClip(r2d(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void pop_clip() { s_rt->PopAxisAlignedClip(); }

// ---------------------------------------------------------------- interaction
static bool blocked() { return s_pop.open && !s_pop.drawing; }

bool hovered(Rc r) { return !blocked() && r.in(in.mx, in.my); }

bool clicked(int id, Rc r) {
    s_rects[id] = r;
    if (blocked()) return false;
    if (in.pressed && r.in(in.mx, in.my)) s_active = id;
    return in.released && s_active == id && r.in(in.mx, in.my);
}

bool held(int id) { return s_active == id && in.down; }

float anim(int id, float target, float rate) {
    auto it = s_anim.find(id);
    if (it == s_anim.end()) { s_anim[id] = target; return target; }
    float& v = it->second;
    v += (target - v) * (float)(1.0 - std::exp(-rate * s_dt));
    if (std::fabs(target - v) < 0.002f) v = target;
    else s_animating = true;
    return v;
}

void state_layer(Rc r, int id, u32 on_color, float radius) {
    float a = held(id) ? 0.10f : hovered(r) ? 0.08f : 0.0f;
    if (a > 0) fill(r, col::alpha(on_color, a), radius);
}

static u32 lerp_color(u32 a, u32 b, float t) {
    auto ch = [&](int s) { return (u32)std::lround(((a >> s) & 255) + (((int)((b >> s) & 255) - (int)((a >> s) & 255)) * t)) << s; };
    return ch(24) | ch(16) | ch(8) | ch(0);
}

// ---------------------------------------------------------------- buttons
static bool button(int id, Rc r, const std::wstring& label, wchar_t glyph, bool enabled, u32 bg, u32 fg, u32 border) {
    float rad = r.h / 2;
    if (!enabled) {
        if (bg) fill(r, col::alpha(col::on_surface, 0.12f), rad);
        if (border) stroke(r, col::alpha(col::on_surface, 0.12f), rad);
        fg = col::alpha(col::on_surface, 0.38f);
    } else {
        if (bg) fill(r, bg, rad);
        if (border) stroke(r, border, rad);
        state_layer(r, id, fg, rad);
    }
    float tw = text_width(label, F_LABEL), gw = glyph ? 18 + 8 : 0;
    float x = r.x + (r.w - tw - gw) / 2;
    if (glyph) icon(glyph, {x, r.y, 18, r.h}, fg);
    text(label, {x + gw, r.y, tw + 2, r.h}, F_LABEL, fg);
    return enabled && clicked(id, r);
}

bool button_filled(int id, Rc r, const std::wstring& l, wchar_t g, bool e) { return button(id, r, l, g, e, col::primary, col::on_primary, 0); }
bool button_tonal(int id, Rc r, const std::wstring& l, wchar_t g, bool e) { return button(id, r, l, g, e, col::secondary_container, col::on_secondary_container, 0); }
bool button_outlined(int id, Rc r, const std::wstring& l, wchar_t g, bool e) { return button(id, r, l, g, e, 0, col::primary, col::outline); }
bool button_text(int id, Rc r, const std::wstring& l, wchar_t g, bool e) { return button(id, r, l, g, e, 0, col::primary, 0); }

bool icon_button(int id, Rc r, wchar_t glyph, u32 hover_bg) {
    bool h = hovered(r);
    if (hover_bg && h) fill(r, hover_bg, 0);
    else state_layer(r, id, col::on_surface, hover_bg ? 0 : std::min(r.w, r.h) / 2);
    icon(glyph, r, hover_bg && h ? 0xFFFFFFFF : col::on_surface_variant, F_ICON);
    return clicked(id, r);
}

// ---------------------------------------------------------------- switch
bool toggle(int id, Rc r, bool& v, bool enabled) {
    Rc t = {r.r() - 52, r.y + (r.h - 32) / 2, 52, 32};
    float k = anim(id, v ? 1.0f : 0.0f);
    float alpha = enabled ? 1.0f : 0.38f;
    u32 track = lerp_color(col::surface_highest, col::primary, k);
    fill(t, col::alpha(track, alpha), 16);
    if (k < 0.999f) stroke(t, col::alpha(col::outline, alpha * (1 - k)), 16, 2);
    bool press = enabled && held(id);
    float rad = press ? 14 : 8 + 4 * k;
    float cx = t.x + 16 + 20 * k, cy = t.y + 16;
    if (enabled && hovered(t)) circle(cx, cy, 20, col::alpha(v ? col::primary : col::on_surface, 0.08f));
    circle(cx, cy, rad, col::alpha(lerp_color(col::outline, col::on_primary, k), alpha));
    if (k > 0.5f) icon(ic::check, {cx - 8, cy - 8, 16, 16}, col::alpha(col::on_primary_container, alpha * (k - 0.5f) * 2), F_ICON);
    if (enabled && clicked(id, t.inset(-4, -4))) { v = !v; return true; }
    return false;
}

// ---------------------------------------------------------------- segmented buttons
static float seg_w(const std::wstring& s) { return text_width(s, F_LABEL) + 18 + 8 + 28; }

float segmented_width(const std::vector<std::wstring>& items) {
    float w = 0;
    for (auto& s : items) w += seg_w(s);
    return w;
}

bool segmented(int id, Rc r, const std::vector<std::wstring>& items, int& v, bool enabled) {
    float w = segmented_width(items);
    Rc box = {r.r() - w, r.y + (r.h - 40) / 2, w, 40};
    float alpha = enabled ? 1.0f : 0.38f;
    bool changed = false;
    float x = box.x;
    for (int i = 0; i < (int)items.size(); i++) {
        float sw = seg_w(items[i]);
        Rc s = {x, box.y, sw, 40};
        bool sel = i == v;
        // only the outer corners are round: clip a full pill to this segment
        push_clip(s);
        if (sel) fill(box, col::alpha(col::secondary_container, alpha), 20);
        pop_clip();
        if (enabled) {
            push_clip(s);
            float a = held(id * 32 + i) ? 0.10f : hovered(s) ? 0.08f : 0.0f;
            if (a > 0) fill(box, col::alpha(col::on_surface, a), 20);
            pop_clip();
        }
        u32 fg = col::alpha(sel ? col::on_secondary_container : col::on_surface, alpha);
        float tw = text_width(items[i], F_LABEL);
        float cx = s.x + (sw - tw - (sel ? 26 : 0)) / 2;
        if (sel) { icon(ic::check, {cx, s.y, 18, 40}, fg); cx += 26; }
        text(items[i], {cx, s.y, tw + 2, 40}, F_LABEL, fg);
        if (i > 0) line(x, box.y, x, box.b(), col::alpha(col::outline, alpha));
        if (enabled && clicked(id * 32 + i, s) && v != i) { v = i; changed = true; }
        x += sw;
    }
    stroke(box, col::alpha(col::outline, alpha), 20);
    return changed;
}

// ---------------------------------------------------------------- dropdown
bool popup_open() { return s_pop.open; }
void close_popup() { s_pop.open = false; }

bool dropdown(int id, Rc r, const std::vector<std::wstring>& items, int& v, bool enabled) {
    bool changed = false;
    if (s_pop.result_id == id) {
        if (s_pop.result >= 0 && s_pop.result < (int)items.size() && s_pop.result != v) { v = s_pop.result; changed = true; }
        s_pop.result_id = 0;
    }
    float alpha = enabled ? 1.0f : 0.38f;
    bool open = s_pop.open && s_pop.id == id;
    stroke(r, col::alpha(open ? col::primary : col::outline, alpha), 8, open ? 2.0f : 1.0f);
    if (enabled && !open) state_layer(r, id, col::on_surface, 8);
    std::wstring cur = v >= 0 && v < (int)items.size() ? items[v] : L"";
    text(cur, {r.x + 14, r.y, r.w - 48, r.h}, F_BODY, col::alpha(col::on_surface, alpha));
    float k = anim(id, open ? 1.0f : 0.0f, 22);
    // chevron flips while the menu is open
    Rc cv = {r.r() - 36, r.y, 24, r.h};
    if (k > 0.5f) icon(0xE70E, cv, col::alpha(col::on_surface_variant, alpha)); // ChevronUp
    else icon(ic::chevron, cv, col::alpha(col::on_surface_variant, alpha));
    if (enabled && clicked(id, r)) {
        s_pop.open = true;
        s_pop.id = id;
        s_pop.anchor = r;
        s_pop.items = items;
        s_pop.selected = v;
        request_frame();
    }
    return changed;
}

void popups() {
    if (!s_pop.open) return;
    s_pop.drawing = true;
    const float ih = 40;
    float mw = s_pop.anchor.w;
    for (auto& s : s_pop.items) mw = std::max(mw, text_width(s, F_BODY) + 64);
    float mh = ih * s_pop.items.size() + 16;
    Rc m = {s_pop.anchor.r() - mw, s_pop.anchor.b() + 4, mw, mh};
    if (m.b() > H - 8) m.y = s_pop.anchor.y - 4 - mh; // no room below: open upwards
    m.y = std::max(8.0f, m.y);
    if (!s_anim.count(0x7FFF0000 + s_pop.id)) s_anim[0x7FFF0000 + s_pop.id] = 0.0f;
    float k = anim(0x7FFF0000 + s_pop.id, 1.0f, 24); // grows in from the field
    // shadow (elevation 2), then the container
    fill({m.x, m.y + 2, m.w, m.h}, 0x40000000, 12);
    fill({m.x - 1, m.y + 1, m.w + 2, m.h + 3}, 0x26000000, 13);
    Rc grow = {m.x, m.y, m.w, m.h * (0.85f + 0.15f * k)};
    fill(grow, col::surface_container, 12);
    push_clip(grow);
    for (int i = 0; i < (int)s_pop.items.size(); i++) {
        Rc it = {m.x, m.y + 8 + ih * i, m.w, ih};
        bool sel = i == s_pop.selected;
        if (sel) fill(it, col::alpha(col::secondary_container, 1.0f), 0);
        state_layer(it, 0x7FFE0000 + i, col::on_surface, 0);
        if (sel) icon(ic::check, {it.x + 12, it.y, 20, ih}, col::on_secondary_container);
        text(s_pop.items[i], {it.x + 44, it.y, it.w - 56, ih}, F_BODY, sel ? col::on_secondary_container : col::on_surface);
        if (clicked(0x7FFE0000 + i, it)) {
            s_pop.result_id = s_pop.id;
            s_pop.result = i;
            s_pop.open = false;
            request_frame();
        }
    }
    pop_clip();
    s_pop.drawing = false;
    // a press outside the menu closes it without reaching what is underneath
    if (s_pop.open && in.pressed && !m.in(in.mx, in.my)) { s_pop.open = false; s_anim.erase(0x7FFF0000 + s_pop.id); request_frame(); }
    if (!s_pop.open) s_anim.erase(0x7FFF0000 + s_pop.id);
}

// ---------------------------------------------------------------- slider
bool slider(int id, Rc r, int& v, int lo, int hi, bool enabled) {
    float alpha = enabled ? 1.0f : 0.38f;
    float cy = r.y + r.h / 2;
    Rc hit = {r.x - 10, cy - 20, r.w + 20, 40};
    if (enabled) clicked(id, hit); // claims the drag on press
    bool changed = false;
    if (enabled && held(id)) {
        float t = std::clamp((in.mx - r.x) / r.w, 0.0f, 1.0f);
        int nv = lo + (int)std::lround(t * (hi - lo));
        if (nv != v) { v = nv; changed = true; }
    }
    float t = (float)(v - lo) / (hi - lo);
    float x = r.x + r.w * t;
    fill({r.x, cy - 2, r.w, 4}, col::alpha(col::secondary_container, alpha), 2);
    fill({r.x, cy - 2, x - r.x, 4}, col::alpha(col::primary, alpha), 2);
    if (enabled && (hovered(hit) || held(id))) circle(x, cy, 20, col::alpha(col::primary, held(id) ? 0.12f : 0.08f));
    circle(x, cy, 10, col::alpha(col::primary, alpha));
    return changed;
}
} // namespace ui
