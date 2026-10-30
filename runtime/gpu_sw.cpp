// gpu_sw.cpp - software rasterizer at native resolution (reference renderer).
#include "psx.h"
#include <algorithm>
#include <cstdlib>
#include <cmath>

void platform_present_image(const u32* rgba, int w, int h, int stride, bool enabled);

static const int k_dither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

struct SoftRenderer : Renderer {
    u16* vram;
    std::vector<u32> frame;

    explicit SoftRenderer(u16* v) : vram(v) {}

    inline u16 texel(int u, int v, const GpuDrawState& st) const {
        if (st.icon >= 0) // button prompt replaced from the glyph atlas (icons.cpp)
            return icon_sample_555(st.icon, std::clamp(u - st.icon_u, 0, st.icon_w - 1), std::clamp(v - st.icon_v, 0, st.icon_h - 1),
                                   st.icon_w, st.icon_h);
        u = (u & ~(st.tex_mask_x * 8)) | ((st.tex_off_x & st.tex_mask_x) * 8);
        v = (v & ~(st.tex_mask_y * 8)) | ((st.tex_off_y & st.tex_mask_y) * 8);
        u &= 0xFF;
        v &= 0xFF;
        int tpx = (st.texpage & 0xF) * 64, tpy = ((st.texpage >> 4) & 1) * 256;
        int depth = (st.texpage >> 7) & 3;
        int row = ((tpy + v) & 511) * 1024;
        int cx = (st.clut & 0x3F) * 16, cy = (st.clut >> 6) & 0x1FF;
        switch (depth) {
        case 0: {
            u16 w = vram[row + ((tpx + u / 4) & 1023)];
            int idx = (w >> ((u & 3) * 4)) & 0xF;
            return vram[cy * 1024 + ((cx + idx) & 1023)];
        }
        case 1: {
            u16 w = vram[row + ((tpx + u / 2) & 1023)];
            int idx = (w >> ((u & 1) * 8)) & 0xFF;
            return vram[cy * 1024 + ((cx + idx) & 1023)];
        }
        default:
            return vram[row + ((tpx + u) & 1023)];
        }
    }

    inline void shade(int x, int y, int r, int g, int b, int u, int v, int flags, const GpuDrawState& st) {
        u16& dst = vram[y * 1024 + x];
        if (st.check_mask && (dst & 0x8000)) return;
        bool semi = (flags & PF_SEMI) != 0;
        u16 maskbit = 0;
        int R = r, G = g, B = b;
        if (flags & PF_TEXTURED) {
            u16 t = texel(u, v, st);
            if (t == 0) return;
            int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
            if (flags & PF_RAW) { R = tr << 3; G = tg << 3; B = tb << 3; }
            else { R = std::min(255, (tr * r) >> 4); G = std::min(255, (tg * g) >> 4); B = std::min(255, (tb * b) >> 4); }
            semi = semi && (t & 0x8000);
            maskbit = t & 0x8000;
        }
        if (st.dither) {
            int d = k_dither[y & 3][x & 3];
            R = clamp255(R + d); G = clamp255(G + d); B = clamp255(B + d);
        }
        int r5 = R >> 3, g5 = G >> 3, b5 = B >> 3;
        if (semi) {
            int br = dst & 31, bg = (dst >> 5) & 31, bb = (dst >> 10) & 31;
            switch ((st.texpage >> 5) & 3) {
            case 0: r5 = (br + r5) >> 1; g5 = (bg + g5) >> 1; b5 = (bb + b5) >> 1; break;
            case 1: r5 = std::min(31, br + r5); g5 = std::min(31, bg + g5); b5 = std::min(31, bb + b5); break;
            case 2: r5 = std::max(0, br - r5); g5 = std::max(0, bg - g5); b5 = std::max(0, bb - b5); break;
            case 3: r5 = std::min(31, br + (r5 >> 2)); g5 = std::min(31, bg + (g5 >> 2)); b5 = std::min(31, bb + (b5 >> 2)); break;
            }
        }
        dst = (u16)(r5 | (g5 << 5) | (b5 << 10) | maskbit | (st.set_mask ? 0x8000 : 0));
    }

    void triangle(const Vertex* vin, int flags, const GpuDrawState& st) override {
        Vertex v[3] = {vin[0], vin[1], vin[2]};
        s64 area = (s64)(v[1].x - v[0].x) * (v[2].y - v[0].y) - (s64)(v[2].x - v[0].x) * (v[1].y - v[0].y);
        if (area == 0) return;
        if (area < 0) { std::swap(v[1], v[2]); area = -area; }
        int minx = std::max<int>(st.clip_x1, std::min(v[0].x, std::min(v[1].x, v[2].x)));
        int maxx = std::min<int>(st.clip_x2, std::max(v[0].x, std::max(v[1].x, v[2].x)));
        int miny = std::max<int>(st.clip_y1, std::min(v[0].y, std::min(v[1].y, v[2].y)));
        int maxy = std::min<int>(st.clip_y2, std::max(v[0].y, std::max(v[1].y, v[2].y)));
        if (minx > maxx || miny > maxy) return;
        // edge setup: e_ab(p) = (bx-ax)*(py-ay) - (by-ay)*(px-ax)
        struct Edge { s64 dx, dy, c; int bias; };
        auto mk = [](const Vertex& a, const Vertex& b) {
            Edge e;
            e.dx = b.x - a.x;
            e.dy = b.y - a.y;
            e.c = -(e.dx * a.y - e.dy * a.x);
            bool topleft = (e.dy < 0) || (e.dy == 0 && e.dx > 0);
            e.bias = topleft ? 0 : -1;
            return e;
        };
        Edge e12 = mk(v[1], v[2]), e20 = mk(v[2], v[0]), e01 = mk(v[0], v[1]);
        auto ev = [](const Edge& e, s64 x, s64 y) { return e.dx * y - e.dy * x + e.c; };
        bool gouraud = flags & PF_GOURAUD, tex = flags & PF_TEXTURED;
        double inv = 1.0 / (double)area;
        for (int y = miny; y <= maxy; y++) {
            s64 w0 = ev(e12, minx, y), w1 = ev(e20, minx, y), w2 = ev(e01, minx, y);
            for (int x = minx; x <= maxx; x++, w0 -= e12.dy, w1 -= e20.dy, w2 -= e01.dy) {
                if (w0 + e12.bias < 0 || w1 + e20.bias < 0 || w2 + e01.bias < 0) continue;
                double b0 = w0 * inv, b1 = w1 * inv, b2 = w2 * inv;
                int r = v[0].r, g = v[0].g, b = v[0].b;
                if (gouraud) {
                    r = (int)(b0 * v[0].r + b1 * v[1].r + b2 * v[2].r + 0.5);
                    g = (int)(b0 * v[0].g + b1 * v[1].g + b2 * v[2].g + 0.5);
                    b = (int)(b0 * v[0].b + b1 * v[1].b + b2 * v[2].b + 0.5);
                }
                int u = 0, tv = 0;
                if (tex) {
                    u = (int)(b0 * v[0].u + b1 * v[1].u + b2 * v[2].u + 0.5);
                    tv = (int)(b0 * v[0].v + b1 * v[1].v + b2 * v[2].v + 0.5);
                }
                shade(x, y, r, g, b, u, tv, flags, st);
            }
        }
    }

    void rect(s32 x, s32 y, s32 w, s32 h, u8 r, u8 g, u8 b, u8 u0, u8 v0, int flags, const GpuDrawState& st, bool fx, bool fy) override {
        int x1 = std::max<int>(x, st.clip_x1), x2 = std::min<int>(x + w - 1, st.clip_x2);
        int y1 = std::max<int>(y, st.clip_y1), y2 = std::min<int>(y + h - 1, st.clip_y2);
        for (int py = y1; py <= y2; py++) {
            int dv = py - y;
            int tv = fy ? v0 - dv : v0 + dv;
            for (int px = x1; px <= x2; px++) {
                int du = px - x;
                int tu = fx ? u0 - du : u0 + du;
                shade(px, py, r, g, b, tu, tv, flags, st);
            }
        }
    }

    void line(const Vertex& a, const Vertex& b, int flags, const GpuDrawState& st) override {
        int dx = b.x - a.x, dy = b.y - a.y;
        int n = std::max(std::abs(dx), std::abs(dy));
        bool gouraud = flags & PF_GOURAUD;
        for (int i = 0; i <= n; i++) {
            double t = n ? (double)i / n : 0.0;
            int x = a.x + (int)std::lround(dx * t), y = a.y + (int)std::lround(dy * t);
            if (x < st.clip_x1 || x > st.clip_x2 || y < st.clip_y1 || y > st.clip_y2) continue;
            int r = a.r, g = a.g, bb = a.b;
            if (gouraud) { r = (int)(a.r + (b.r - a.r) * t); g = (int)(a.g + (b.g - a.g) * t); bb = (int)(a.b + (b.b - a.b) * t); }
            shade(x, y, r, g, bb, 0, 0, flags & ~PF_TEXTURED, st);
        }
    }

    void fill(int x, int y, int w, int h, u32 c) override {
        u16 p = (u16)(((c & 0xFF) >> 3) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) vram[((y + yy) & 511) * 1024 + ((x + xx) & 1023)] = p;
    }

    void vram_write(int x, int y, int w, int h, const u16* data, const GpuDrawState& st) override {
        u16 m = st.set_mask ? 0x8000 : 0;
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) {
                u16& d = vram[((y + yy) & 511) * 1024 + ((x + xx) & 1023)];
                if (st.check_mask && (d & 0x8000)) continue;
                d = data[yy * w + xx] | m;
            }
    }

    void vram_copy(int sx, int sy, int dx, int dy, int w, int h, const GpuDrawState& st) override {
        std::vector<u16> tmp((size_t)w * h);
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) tmp[yy * w + xx] = vram[((sy + yy) & 511) * 1024 + ((sx + xx) & 1023)];
        vram_write(dx, dy, w, h, tmp.data(), st);
    }

    void vram_read(int, int, int, int) override {}

    void present(const DisplayInfo& d) override {
        int w = d.w, h = d.h;
        frame.resize((size_t)w * h);
        if (d.depth24) {
            const u8* bytes = (const u8*)vram;
            for (int y = 0; y < h; y++) {
                int row = ((d.y + y) & 511) * 2048;
                for (int x = 0; x < w; x++) {
                    int o = d.x * 2 + x * 3;
                    u8 r = bytes[row + (o & 2047)], g = bytes[row + ((o + 1) & 2047)], b = bytes[row + ((o + 2) & 2047)];
                    frame[y * w + x] = r | (g << 8) | (b << 16) | 0xFF000000u;
                }
            }
        } else {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    u16 p = vram[((d.y + y) & 511) * 1024 + ((d.x + x) & 1023)];
                    u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                    frame[y * w + x] = (r | (r >> 5)) | ((g | (g >> 5)) << 8) | ((b | (b >> 5)) << 16) | 0xFF000000u;
                }
        }
        platform_present_image(frame.data(), w, h, w, d.enabled);
    }
};

Renderer* renderer_create_software(u16* vram) { return new SoftRenderer(vram); }
