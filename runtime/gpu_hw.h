// gpu_hw.h - what the hardware renderers share (Direct3D 11: gpu_d3d11.cpp, SDL GPU: sdl/gpu_sdlgpu.cpp): the
// vertex and constant layouts of gpu.hlsl, batching of PS1 primitives, the CPU copy of VRAM, dirty tiles and the
// HD letter atlas's layout. A backend supplies the render targets and the drawing.
#pragma once
#include "psx.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

const int HW_SHARP = 0x100; // renderer-only primitive flag: never texture-filtered

struct HwVertex {
    float x, y;
    u32 color;
    float u, v;
    u32 a0, a1;
    u32 box; // texels the primitive uses: u0 | v0 << 8 | u1 << 16 | v1 << 24 (filters stay inside)
};

struct DrawCB {
    s32 pass_mode;
    float src_factor;
    s32 scale;
    s32 true_color;
    float rect[4];
    float fill[4];
    s32 filter;
    s32 samples;
    s32 copy_off[2];
};

struct HwRect { int x0, y0, x1, y1; }; // render target pixels, x1 / y1 exclusive

void write_png(const std::string& path, const u8* rgba, int w, int h, int pitch); // platform_common.cpp

static inline u32 rgb555_to_rgba(u16 p) {
    u32 r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
    auto e = [](u32 c) { return (c * 255 + 15) / 31; };
    return e(r) | (e(g) << 8) | (e(b) << 16) | ((p & 0x8000) ? 0xFF000000u : 0);
}
static inline u16 rgba_to_555(u32 c) {
    auto q = [](u32 v) { return (v * 31 + 127) / 255; };
    return (u16)(q(c & 0xFF) | (q((c >> 8) & 0xFF) << 5) | (q((c >> 16) & 0xFF) << 10) | ((c >> 24) > 127 ? 0x8000 : 0));
}

struct HwRenderer : Renderer {
    u16* shadow;
    int S = 1;
    int MS = 1;                    // samples per pixel of the render target (MSAA / SSAA)
    int want_ms = 1;               // what the setting asked for when it was made (the GPU may give fewer)
    // HD letters (hd_text.cpp): reconstructions packed in rows of equal height, uploaded when first drawn
    static const int GLYPH_ATLAS = 4096;
    static_assert(HD_TEXT_K == 12 && HD_TEXT_PAD == 1, "gpu.hlsl's HD_K and HD_PAD");
    static const int GLYPH_SRC_W = 64, GLYPH_SRC_H = 32;
    static_assert(HD_TEXT_MAX_W + 2 * HD_TEXT_PAD + 4 <= GLYPH_SRC_W && HD_TEXT_MAX_H + 2 * HD_TEXT_PAD + 4 <= GLYPH_SRC_H,
                  "hd_text_source's grid fits glyph_src");
    bool glyph_ok = false;                       // the backend made the atlas
    std::vector<u32> glyph_cell;                 // per glyph id: x | y << 16, ~0u = not in the atlas
    struct Shelf { int y, h, x; };
    std::vector<Shelf> glyph_shelves;
    int glyph_top = 0;
    bool glyph_mips = false;                     // top level changed since the mip chain was made
    int copy_off[2] = {0, 0};      // for the next constants (ps_copy / ps_copy_ms)
    std::vector<HwVertex> batch;
    int batch_kind = -1;          // 0 opaque, 1 blended, 2 two-pass
    int batch_mode = 0;
    HwRect batch_scissor = {};
    bool dirty[16][32] = {};       // 32x32 tiles needing a copy to the sampling texture
    bool any_dirty = false;
    std::vector<u32> frame24;
    // frame interpolation: in-between frames are drawn into a target covering only the displayed buffer; the
    // viewport is shifted so primitives keep their VRAM coordinates
    bool redirect = false;          // drawing an in-between frame
    int tox = 0, toy = 0;           // that target's origin in render-target pixels

    explicit HwRenderer(u16* v) : shadow(v) {}

    // ------------------------------------------------------------ the backend
    virtual void create_targets(int scale, int samples) = 0; // sets S, MS, want_ms
    virtual void sync_copy() = 0;                    // dirty tiles of the render target into the copy textures read from
    virtual void glyph_clear() = 0;                  // the atlas emptied
    virtual void glyph_draw(const HdGlyph& g, int x, int y, int w, int h) = 0; // letter g reconstructed into a cell
    virtual void upload_pixels(int x, int y, int w, int h, const u32* rgba) = 0; // native pixels into the render target
    virtual void fill_rect(int x, int y, int w, int h, const float* col) = 0;
    virtual void copy_rect(int sx, int sy, int dx, int dy, int w, int h) = 0; // within the render target, no wrapping
    virtual void download(int x, int y, int w, int h) = 0; // the render target, downsampled, into shadow
    virtual void show_vram(const DisplayInfo& d) = 0;      // present() of a 15-bit display

    // the resolution or the sample count changed since the targets were made
    bool targets_stale() const { return S != g_settings.scale || want_ms != aa_samples(g_settings.anti_aliasing); }

    DrawCB make_cb(int pass, float factor, const float* rect = nullptr, const float* fillc = nullptr) const {
        DrawCB c = {};
        c.pass_mode = pass;
        c.src_factor = factor;
        c.scale = S;
        c.true_color = g_settings.true_color ? 1 : 0;
        c.filter = std::clamp(g_settings.texture_filter, 0, TEX_COUNT - 1); // 1 bilinear, 2 xBR
        c.samples = MS;
        c.copy_off[0] = copy_off[0];
        c.copy_off[1] = copy_off[1];
        if (rect) memcpy(c.rect, rect, sizeof c.rect);
        if (fillc) memcpy(c.fill, fillc, sizeof c.fill);
        return c;
    }

    // ------------------------------------------------------------ dirty tracking
    void mark_dirty(int x0, int y0, int x1, int y1) {
        x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(1023, x1); y1 = std::min(511, y1);
        if (x0 > x1 || y0 > y1) return;
        for (int ty = y0 >> 5; ty <= (y1 >> 5); ty++)
            for (int tx = x0 >> 5; tx <= (x1 >> 5); tx++) dirty[ty][tx] = true;
        any_dirty = true;
    }
    bool region_dirty(int x0, int y0, int w, int h) {
        if (!any_dirty) return false;
        for (int ty = y0 >> 5; ty <= ((y0 + h - 1) >> 5); ty++)
            for (int tx = x0 >> 5; tx <= ((x0 + w - 1) >> 5); tx++)
                if (dirty[ty & 15][tx & 31]) return true;
        return false;
    }

    // ------------------------------------------------------------ HD letter atlas
    void glyph_reset() {
        glyph_clear();
        glyph_cell.assign(glyph_cell.size(), ~0u);
        glyph_shelves.clear();
        glyph_top = 0;
        glyph_mips = true;
    }

    bool glyph_alloc(int w, int h, int& x, int& y) {
        const int gap = 8; // between cells, so a letter's coarse mip levels stay clear of the next
        for (Shelf& s : glyph_shelves)
            if (s.h == h && s.x + w <= GLYPH_ATLAS) { x = s.x; y = s.y; s.x += w + gap; return true; }
        if (glyph_top + h > GLYPH_ATLAS || w > GLYPH_ATLAS) return false;
        glyph_shelves.push_back({glyph_top, h, w + gap});
        x = 0;
        y = glyph_top;
        glyph_top += h + gap;
        return true;
    }

    // a letter's cell in the atlas (its reconstruction is uploaded the first time) and its rim / letter
    // palette entries; false = draw the game's sprite
    bool glyph_place(int id, u32& cell, u32& lohi) {
        const HdGlyph* g = hd_text_glyph(id);
        if (!g || !glyph_ok) return false;
        if ((size_t)id >= glyph_cell.size()) glyph_cell.resize(id + 1, ~0u);
        if (glyph_cell[id] == ~0u) {
            const int w = (g->w + 2 * HD_TEXT_PAD) * HD_TEXT_K, h = (g->h + 2 * HD_TEXT_PAD) * HD_TEXT_K;
            int x, y;
            if (!glyph_alloc(w, h, x, y)) { // full: start over (primitives already batched read the old cells)
                flush();
                glyph_reset();
                if (!glyph_alloc(w, h, x, y)) return false;
            }
            glyph_draw(*g, x, y, w, h);
            glyph_cell[id] = (u32)x | (u32)y << 16;
            glyph_mips = true;
            if (g_settings.log_level >= 2) plog("hd text: letter %d (%dx%d texels) at %d,%d\n", id, g->w, g->h, x, y);
        }
        cell = glyph_cell[id];
        lohi = g->lo | g->hi << 4;
        return true;
    }

    // ------------------------------------------------------------ batching
    void prepare(int flags, const GpuDrawState& st, int kind, int x0, int y0, int x1, int y1) {
        if (targets_stale() && !redirect) rescale();
        // textures read from areas we rendered to need a fresh copy
        if (flags & PF_TEXTURED) {
            int depth = (st.texpage >> 7) & 3;
            int tpx = (st.texpage & 0xF) * 64, tpy = ((st.texpage >> 4) & 1) * 256;
            int tw = depth == 0 ? 64 : depth == 1 ? 128 : 256;
            bool need = region_dirty(tpx, tpy, std::min(tw, 1024 - tpx), 256);
            if (!need && depth < 2) {
                int cx = (st.clut & 0x3F) * 16, cy = (st.clut >> 6) & 0x1FF;
                need = region_dirty(cx, cy, std::min(depth == 0 ? 16 : 256, 1024 - cx), 1);
            }
            if (need) { flush(); sync_copy(); }
        }
        int mode = (st.texpage >> 5) & 3;
        HwRect sc = {st.clip_x1 * S - tox, st.clip_y1 * S - toy, (st.clip_x2 + 1) * S - tox, (st.clip_y2 + 1) * S - toy};
        if (kind != batch_kind || (kind != 0 && mode != batch_mode) || memcmp(&sc, &batch_scissor, sizeof sc) != 0 || batch.size() > 600000) {
            flush();
            batch_kind = kind;
            batch_mode = mode;
            batch_scissor = sc;
        }
        if (!redirect) // in-between frames never change VRAM
            mark_dirty(std::max<int>(x0, st.clip_x1), std::max<int>(y0, st.clip_y1), std::min<int>(x1, st.clip_x2), std::min<int>(y1, st.clip_y2));
    }

    static int kind_of(int flags) {
        if (!(flags & PF_SEMI)) return 0;
        return (flags & PF_TEXTURED) ? 2 : 1;
    }

    HwVertex hv(float x, float y, u8 r, u8 g, u8 b, float u, float v, int flags, const GpuDrawState& st, u32 box = 0) {
        HwVertex o;
        o.x = x;
        o.y = y;
        o.color = r | (g << 8) | (b << 16) | 0xFF000000u;
        o.u = u;
        o.v = v;
        o.box = box;
        o.a0 = (u32)st.texpage | ((u32)st.clut << 16);
        u32 win = st.tex_mask_x | (st.tex_mask_y << 5) | (st.tex_off_x << 10) | (st.tex_off_y << 15);
        u32 f = ((flags & PF_TEXTURED) ? 1 : 0) | ((flags & PF_RAW) ? 2 : 0) | (st.dither ? 4 : 0) | (st.set_mask ? 8 : 0) |
                ((flags & HW_SHARP) ? 32 : 0);
        o.a1 = win | (f << 20);
        if (st.icon >= 0 && (flags & PF_TEXTURED) && st.icon_w && st.icon_h) { // button prompt drawn from the glyph atlas (icons.cpp)
            o.a0 = (u32)st.icon | ((u32)(st.icon_w | (st.icon_h << 8)) << 16);
            o.a1 = win | ((f | 16) << 20);
            o.u -= st.icon_u;
            o.v -= st.icon_v;
        }
        return o;
    }

    static VertexF vf(const Vertex& v) { return VertexF{(float)v.x, (float)v.y, v.r, v.g, v.b, v.u, v.v}; }

    // positions are floats: in-between frames place primitives between pixels
    void tri_f(const VertexF* v, int flags, const GpuDrawState& st) {
        float fx0 = std::min(v[0].x, std::min(v[1].x, v[2].x)), fx1 = std::max(v[0].x, std::max(v[1].x, v[2].x));
        float fy0 = std::min(v[0].y, std::min(v[1].y, v[2].y)), fy1 = std::max(v[0].y, std::max(v[1].y, v[2].y));
        prepare(flags, st, kind_of(flags), (int)std::floor(fx0), (int)std::floor(fy0), (int)std::ceil(fx1), (int)std::ceil(fy1));
        const u32 box = (u32)std::min(v[0].u, std::min(v[1].u, v[2].u)) | (u32)std::min(v[0].v, std::min(v[1].v, v[2].v)) << 8 |
                        (u32)std::max(v[0].u, std::max(v[1].u, v[2].u)) << 16 | (u32)std::max(v[0].v, std::max(v[1].v, v[2].v)) << 24;
        for (int k = 0; k < 3; k++) batch.push_back(hv(v[k].x, v[k].y, v[k].r, v[k].g, v[k].b, v[k].u, v[k].v, flags, st, box));
    }

    void triangle(const Vertex* v, int flags, const GpuDrawState& st) override {
        VertexF f[3] = {vf(v[0]), vf(v[1]), vf(v[2])};
        tri_f(f, flags, st);
    }

    void rect_f(float x, float y, s32 w, s32 h, u8 r, u8 g, u8 b, u8 u0, u8 v0, int flags, const GpuDrawState& st, bool fx, bool fy) {
        prepare(flags, st, kind_of(flags), (int)std::floor(x), (int)std::floor(y), (int)std::ceil(x) + w - 1, (int)std::ceil(y) + h - 1);
        // texture filtering smears small sprites (the game's letters are 8x8 texels): those stay sharp, while
        // larger 2D art, like the main menu's 192x80 pictures, is smoothed with the 3D textures
        if (w < 32 || h < 32) flags |= HW_SHARP;
        float ul = fx ? u0 + 1.0f : (float)u0, ur = fx ? u0 + 1.0f - w : (float)(u0 + w);
        float vt = fy ? v0 + 1.0f : (float)v0, vb2 = fy ? v0 + 1.0f - h : (float)(v0 + h);
        // the texels it covers, flipped or not (u wraps within the page like the GPU's)
        const u32 bu0 = (u32)(int)std::min(ul, ur) & 255, bv0 = (u32)(int)std::min(vt, vb2) & 255;
        const u32 bu1 = std::min<u32>(255, bu0 + (u32)std::max(w, 1) - 1), bv1 = std::min<u32>(255, bv0 + (u32)std::max(h, 1) - 1);
        const u32 box = bu0 | bv0 << 8 | bu1 << 16 | bv1 << 24;
        u32 cell = 0, lohi = 0;
        const bool hd = st.glyph >= 0 && st.icon < 0 && (flags & PF_TEXTURED) && !fx && !fy && glyph_place(st.glyph, cell, lohi);
        auto vert = [&](float px, float py, float u, float v) {
            HwVertex o = hv(px, py, r, g, b, u, v, flags, st, box);
            if (hd) { // HD letter: its atlas cell, rim / letter palette entries, uv from the sprite's corner
                o.box = cell;
                o.a1 = lohi | (o.a1 & 0xFFF00000u) | (64u << 20);
                o.u -= u0;
                o.v -= v0;
            }
            return o;
        };
        HwVertex a = vert(x, y, ul, vt);
        HwVertex bq = vert(x + w, y, ur, vt);
        HwVertex c = vert(x, y + h, ul, vb2);
        HwVertex d = vert(x + w, y + h, ur, vb2);
        batch.push_back(a); batch.push_back(bq); batch.push_back(c);
        batch.push_back(bq); batch.push_back(d); batch.push_back(c);
    }

    void rect(s32 x, s32 y, s32 w, s32 h, u8 r, u8 g, u8 b, u8 u0, u8 v0, int flags, const GpuDrawState& st, bool fx, bool fy) override {
        rect_f((float)x, (float)y, w, h, r, g, b, u0, v0, flags, st, fx, fy);
    }

    void line(const Vertex& a, const Vertex& b, int flags, const GpuDrawState& st) override { line_f(vf(a), vf(b), flags, st); }

    void line_f(const VertexF& a, const VertexF& b, int flags, const GpuDrawState& st) {
        prepare(flags, st, kind_of(flags), (int)std::floor(std::min(a.x, b.x)), (int)std::floor(std::min(a.y, b.y)),
                (int)std::ceil(std::max(a.x, b.x)), (int)std::ceil(std::max(a.y, b.y)));
        // expand the line into a quad one native pixel wide
        float dx = b.x - a.x, dy = b.y - a.y;
        float ax = a.x + 0.5f, ay = a.y + 0.5f, bx = b.x + 0.5f, by = b.y + 0.5f;
        float len = std::sqrt(dx * dx + dy * dy);
        float nx = 0, ny = 0;
        if (len > 0) { dx /= len; dy /= len; nx = -dy * 0.5f; ny = dx * 0.5f; }
        else { nx = 0.5f; dx = 0.5f; dy = 0; }
        ax -= dx * 0.5f; ay -= dy * 0.5f; bx += dx * 0.5f; by += dy * 0.5f;
        HwVertex p0 = hv(ax + nx, ay + ny, a.r, a.g, a.b, 0, 0, flags, st);
        HwVertex p1 = hv(ax - nx, ay - ny, a.r, a.g, a.b, 0, 0, flags, st);
        HwVertex p2 = hv(bx + nx, by + ny, b.r, b.g, b.b, 0, 0, flags, st);
        HwVertex p3 = hv(bx - nx, by - ny, b.r, b.g, b.b, 0, 0, flags, st);
        batch.push_back(p0); batch.push_back(p1); batch.push_back(p2);
        batch.push_back(p1); batch.push_back(p3); batch.push_back(p2);
    }

    // ------------------------------------------------------------ vram operations
    // split a possibly wrapping VRAM rectangle into up to four pieces
    template <typename F> static void split(int x, int y, int w, int h, F fn) {
        int w0 = std::min(w, 1024 - x), h0 = std::min(h, 512 - y);
        fn(x, y, w0, h0);
        if (w0 < w) fn(0, y, w - w0, h0);
        if (h0 < h) fn(x, 0, w0, h - h0);
        if (w0 < w && h0 < h) fn(0, 0, w - w0, h - h0);
    }

    void upload_rect(int x, int y, int w, int h) {
        std::vector<u32> tmp((size_t)w * h);
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) tmp[yy * w + xx] = rgb555_to_rgba(shadow[(y + yy) * 1024 + x + xx]);
        upload_pixels(x, y, w, h, tmp.data());
    }

    static void fill_color(u32 c, float* col) {
        col[0] = (c & 0xFF) / 255.0f;
        col[1] = ((c >> 8) & 0xFF) / 255.0f;
        col[2] = ((c >> 16) & 0xFF) / 255.0f;
        col[3] = 0;
        if (!g_settings.true_color) for (int k = 0; k < 3; k++) col[k] = std::floor(col[k] * 255.0f / 8.0f) / 31.0f;
    }

    void fill(int x, int y, int w, int h, u32 c) override {
        flush();
        if (w <= 0 || h <= 0) return;
        u16 p = (u16)(((c & 0xFF) >> 3) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) shadow[((y + yy) & 511) * 1024 + ((x + xx) & 1023)] = p;
        float col[4];
        fill_color(c, col);
        split(x, y, w, h, [&](int a, int b, int cw, int ch) { fill_rect(a, b, cw, ch, col); });
    }

    void vram_write(int x, int y, int w, int h, const u16* data, const GpuDrawState& st) override {
        flush();
        u16 m = st.set_mask ? 0x8000 : 0;
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) {
                u16& d = shadow[((y + yy) & 511) * 1024 + ((x + xx) & 1023)];
                if (st.check_mask && (d & 0x8000)) continue;
                d = data[yy * w + xx] | m;
            }
        split(x, y, w, h, [&](int a, int b, int cw, int ch) { upload_rect(a, b, cw, ch); });
    }

    void vram_copy(int sx, int sy, int dx, int dy, int w, int h, const GpuDrawState& st) override {
        flush();
        // keep the CPU copy coherent
        std::vector<u16> tmp((size_t)w * h);
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) tmp[yy * w + xx] = shadow[((sy + yy) & 511) * 1024 + ((sx + xx) & 1023)];
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) {
                u16& d = shadow[((dy + yy) & 511) * 1024 + ((dx + xx) & 1023)];
                if (st.check_mask && (d & 0x8000)) continue;
                d = tmp[yy * w + xx] | (st.set_mask ? 0x8000 : 0);
            }
        sync_copy();
        bool wraps = sx + w > 1024 || sy + h > 512 || dx + w > 1024 || dy + h > 512;
        if (wraps) {
            split(dx, dy, w, h, [&](int a, int b, int cw, int ch) { upload_rect(a, b, cw, ch); });
            return;
        }
        copy_rect(sx, sy, dx, dy, w, h);
    }

    void vram_read(int x, int y, int w, int h) override {
        flush();
        split(x, y, w, h, [&](int a, int b, int cw, int ch) { download(a, b, cw, ch); });
    }

    void rescale() {
        flush();
        download(0, 0, 1024, 512);
        create_targets(g_settings.scale, aa_samples(g_settings.anti_aliasing));
        memset(dirty, 0, sizeof dirty);
        any_dirty = false;
        upload_rect(0, 0, 1024, 512);
    }

    // ------------------------------------------------------------ in-between frames (interp.cpp)
    bool interp_supported() const override { return true; }
    void interp_tri(const VertexF* v, int flags, const GpuDrawState& st) override { tri_f(v, flags, st); }
    void interp_rect(float x, float y, s32 w, s32 h, u8 r, u8 g, u8 b, u8 u0, u8 v0, int flags, const GpuDrawState& st, bool fx, bool fy) override {
        rect_f(x, y, w, h, r, g, b, u0, v0, flags, st, fx, fy);
    }
    void interp_line(const VertexF& a, const VertexF& b, int flags, const GpuDrawState& st) override { line_f(a, b, flags, st); }

    void interp_fill(int x, int y, int w, int h, u32 c) override { // like fill(), without touching VRAM
        flush();
        if (w <= 0 || h <= 0) return;
        float col[4];
        fill_color(c, col);
        split(x, y, w, h, [&](int a, int b, int cw, int ch) { fill_rect(a, b, cw, ch, col); });
        batch_kind = -1;
    }

    // ------------------------------------------------------------ the picture on screen
    void present(const DisplayInfo& d) override {
        flush();
        if (targets_stale()) rescale();
        static int dump_at = getenv("TB_VRAM_DUMP") ? atoi(getenv("TB_VRAM_DUMP")) : -1, presents = 0;
        if (++presents == dump_at) { // debugging aid: CPU copy and downsampled GPU copy of VRAM
            std::string dir = getenv("TB_SHOT_DIR") ? getenv("TB_SHOT_DIR") : g_base_dir;
            std::vector<u32> px(1024 * 512);
            for (int i = 0; i < 1024 * 512; i++) px[i] = rgb555_to_rgba(shadow[i]) | 0xFF000000u;
            write_png(dir + PATH_SEP "vram_cpu.png", (const u8*)px.data(), 1024, 512, 4096);
            download(0, 0, 1024, 512);
            for (int i = 0; i < 1024 * 512; i++) px[i] = rgb555_to_rgba(shadow[i]) | 0xFF000000u;
            write_png(dir + PATH_SEP "vram_gpu.png", (const u8*)px.data(), 1024, 512, 4096);
        }
        if (d.depth24) {
            // 24-bit (FMV) frames come from CPU uploads, so the CPU copy is exact
            frame24.resize((size_t)d.w * d.h);
            const u8* bytes = (const u8*)shadow;
            for (int y = 0; y < d.h; y++) {
                int row = ((d.y + y) & 511) * 2048;
                for (int x = 0; x < d.w; x++) {
                    int o = d.x * 2 + x * 3;
                    frame24[y * d.w + x] = bytes[row + (o & 2047)] | (bytes[row + ((o + 1) & 2047)] << 8) | (bytes[row + ((o + 2) & 2047)] << 16) | 0xFF000000u;
                }
            }
            platform_present_image(frame24.data(), d.w, d.h, d.w, d.enabled);
            return;
        }
        show_vram(d);
    }
};
