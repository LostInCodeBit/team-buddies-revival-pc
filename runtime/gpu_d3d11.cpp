// gpu_d3d11.cpp - hardware renderer: draws PS1 primitives with Direct3D 11 into an upscaled VRAM.
#include "platform_d3d.h"
#include "psx.h"
#include <algorithm>
#include <cmath>

void platform_present_image(const u32* rgba, int w, int h, int stride, bool enabled);

static const char* k_hlsl = R"(
cbuffer Draw : register(b0) {
    int pass_mode;     // 0 = all texels, 1 = opaque texels only, 2 = semi-transparent texels only
    float src_factor;
    int scale;
    int true_color;
    float4 rect;       // upload/fill: x0 y0 x1 y1 in render target pixels
    float4 fill_color;
    int filter;        // bilinear texture filtering
    int samples;       // samples per pixel of the render target (MSAA / SSAA)
    int2 copy_off;     // copies: source position minus destination position, render target pixels
};
Texture2D<float4> vram : register(t0);
Texture2D<float4> icons : register(t2);   // button glyph atlas, premultiplied (icon_atlas.cpp)
Texture2DMS<float4> vram_ms : register(t3); // the render target when multisampled
Texture2D<float4> picture : register(t4); // FXAA input
Texture2D<float2> glyphs : register(t5);  // HD letters (hd_text.cpp): coverage, opacity
static const float HD_K = 12.0, HD_PAD = 1.0, HD_ATLAS = 4096.0; // HD_TEXT_K, HD_TEXT_PAD (psx.h), GLYPH_ATLAS
SamplerState icon_smp : register(s0);
SamplerState lin_smp : register(s1);

// box: the texels the primitive uses (u0 v0 u1 v1); filters read no neighbours outside it, so a letter never
// picks up the edge of the next one in the font
struct VSIn { float2 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD; uint2 attr : ATTR; uint4 box : BOX; };
struct VSOut { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD0; nointerpolation uint2 attr : ATTR; nointerpolation uint4 box : BOX; };
// SSAA builds this file with PER_SAMPLE: colours and texture coordinates are taken at every sample. Otherwise
// at the centroid of the covered samples, so a pixel on a polygon's edge (MSAA) never reads texels from outside it.
#ifdef PER_SAMPLE
#define AA_AT sample
#else
#define AA_AT centroid
#endif
struct PSIn { float4 pos : SV_Position; AA_AT float4 col : COLOR; AA_AT float2 uv : TEXCOORD0; nointerpolation uint2 attr : ATTR; nointerpolation uint4 box : BOX; };

VSOut vs_main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos.x / 512.0 - 1.0, 1.0 - i.pos.y / 256.0, 0, 1);
    o.col = i.col;
    o.uv = i.uv;
    o.attr = i.attr;
    o.box = i.box;
    return o;
}

uint enc(float4 c) {
    uint3 v = (uint3)(c.rgb * 31.0 + 0.5);
    return v.r | (v.g << 5) | (v.b << 10) | (c.a > 0.5 ? 0x8000u : 0u);
}
float4 at(int2 n, int2 sub) { return vram.Load(int3((n & int2(1023, 511)) * scale + sub, 0)); }

static const int dither[16] = { -4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2 };

// one texel through the texture window, page and palette; sub = position inside an upscaled texel
float4 texel(int2 uvi, int2 sub, uint texpage, uint clut, uint win) {
    int mx = win & 31, my = (win >> 5) & 31, ox = (win >> 10) & 31, oy = (win >> 15) & 31;
    int u = ((uvi.x & ~(mx * 8)) | ((ox & mx) * 8)) & 255;
    int v = ((uvi.y & ~(my * 8)) | ((oy & my) * 8)) & 255;
    int tpx = (texpage & 15) * 64, tpy = ((texpage >> 4) & 1) * 256;
    int depth = (texpage >> 7) & 3;
    int2 cl = int2((clut & 63) * 16, (clut >> 6) & 511);
    if (depth == 0) {
        uint w = enc(at(int2(tpx + u / 4, tpy + v), int2(0, 0)));
        return at(int2(cl.x + ((w >> ((u & 3) * 4)) & 15), cl.y), int2(0, 0));
    }
    if (depth == 1) {
        uint w = enc(at(int2(tpx + u / 2, tpy + v), int2(0, 0)));
        return at(int2(cl.x + ((w >> ((u & 1) * 8)) & 255), cl.y), int2(0, 0));
    }
    return at(int2(tpx + u, tpy + v), sub);
}

// a texel for the filters: kept inside the primitive's box, colour premultiplied by opacity (transparent = 0)
float4 tx(int2 p, int4 box, uint texpage, uint clut, uint win) {
    float4 s = texel(clamp(p, box.xy, box.zw), int2(0, 0), texpage, clut, win);
    float op = enc(s) == 0 ? 0.0 : 1.0;
    return float4(s.rgb * op, op);
}
// brightness for xBR's edge tests (48 = its weight for luma); transparency counts as a colour of its own
float lum(float4 c) { return c.a > 0.5 ? dot(c.rgb, float3(0.299, 0.587, 0.114)) * 48.0 : 200.0; }
float4 wdist(float4 a, float4 b, float4 c, float4 d, float4 e, float4 f, float4 g, float4 h) {
    return abs(a - b) + abs(a - c) + abs(d - e) + abs(d - f) + 4.0 * abs(g - h);
}
bool4 near_eq(float4 a, float4 b) { return abs(a - b) < 15.0; }

// xBR-lv2 by Hyllian (MIT licence: licenses/xBR-lv2.txt), free scale: the texel E under the sample and its
// neighbours decide, per corner, whether an edge crosses E at 45 (or 30 / 60) degrees, and the part of E beyond
// it takes the neighbour's colour. Pixel art keeps hard, smooth edges at any size instead of blocks (nearest) or
// blur (bilinear). w = texels per pixel.
//      A1 B1 C1
//   A0 A  B  C  C4
//   D0 D  E  F  F4
//   G0 G  H  I  I4
//      G5 H5 I5
float4 xbr(float2 uv, float w, int4 box, uint texpage, uint clut, uint win) {
    int2 e = (int2)floor(uv);
    float2 fp = uv - e;
#define T(dx, dy) tx(e + int2(dx, dy), box, texpage, clut, win)
    float4 E = T(0, 0), B = T(0, -1), D = T(-1, 0), F = T(1, 0), H = T(0, 1);
    float4 b = float4(lum(B), lum(D), lum(H), lum(F));
    // every rule needs E to differ from a side neighbour: inside a flat patch E stays (5 texels read, not 21)
    [branch] if (all(b == lum(E).xxxx)) return E;
    float4 A1 = T(-1, -2), B1 = T(0, -2), C1 = T(1, -2);
    float4 A0 = T(-2, -1), A = T(-1, -1), C = T(1, -1), C4 = T(2, -1);
    float4 D0 = T(-2, 0), F4 = T(2, 0);
    float4 G0 = T(-2, 1), G = T(-1, 1), I = T(1, 1), I4 = T(2, 1);
    float4 G5 = T(-1, 2), H5 = T(0, 2), I5 = T(1, 2);
#undef T
    float4 c = float4(lum(C), lum(A), lum(G), lum(I));
    float4 ee = lum(E).xxxx;
    float4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, ii = c.wxyz;
    float4 i4 = float4(lum(I4), lum(C1), lum(A0), lum(G5));
    float4 i5 = float4(lum(I5), lum(C4), lum(A1), lum(G0));
    float4 h5 = float4(lum(H5), lum(F4), lum(B1), lum(D0));
    float4 f4 = h5.yzwx;
    // edge lines through E, per corner (bottom right, top right, top left, bottom left)
    const float4 Ao = float4(1.0, -1.0, -1.0, 1.0), Bo = float4(1.0, 1.0, -1.0, -1.0), Co = float4(1.5, 0.5, -0.5, 0.5);
    const float4 Bx = float4(0.5, 2.0, -0.5, -2.0), Cx = float4(1.0, 1.0, -0.5, 0.0);
    const float4 By = float4(2.0, 0.5, -2.0, -0.5), Cy = float4(2.0, 0.0, -1.0, 0.5);
    const float4 Ci = float4(0.25, 0.25, 0.25, 0.25);
    float4 fx = Ao * fp.y + Bo * fp.x, fx_left = Ao * fp.y + Bx * fp.x, fx_up = Ao * fp.y + By * fp.x;
    // anti-aliased edge: about one screen pixel wide, whatever the magnification
    float dl = clamp(w, 0.02, 0.5);
    float4 delta = dl.xxxx, deltaL = float4(0.5, 1.0, 0.5, 1.0) * dl, deltaU = deltaL.yxwz;
    bool4 r0 = ee != f && ee != h;
    bool4 r1 = r0 && (!near_eq(f, b) && !near_eq(h, d) || near_eq(ee, ii) && !near_eq(f, i4) && !near_eq(h, i5) || near_eq(ee, g) || near_eq(ee, c));
    bool4 r2l = ee != g && d != g, r2u = ee != c && b != c;
    float4 wd1 = wdist(ee, c, g, ii, h5, f4, h, f), wd2 = wdist(h, d, i5, f, i4, b, ee, ii);
    bool4 edri = wd1 <= wd2 && r0, edr = wd1 < wd2 && r1;
    bool4 edr_l = 2.0 * abs(f - g) <= abs(h - c) && r2l && edr, edr_u = abs(f - g) >= 2.0 * abs(h - c) && r2u && edr;
    float4 fx45i = saturate((fx + delta - Co - Ci) / (2.0 * delta)) * edri;
    float4 fx45 = saturate((fx + delta - Co) / (2.0 * delta)) * edr;
    float4 fx30 = saturate((fx_left + deltaL - Cx) / (2.0 * deltaL)) * edr_l;
    float4 fx60 = saturate((fx_up + deltaU - Cy) / (2.0 * deltaU)) * edr_u;
    bool4 px = abs(ee - f) <= abs(ee - h);
    float4 m = max(max(fx30, fx60), max(fx45, fx45i));
    float4 r1c = lerp(E, px.x ? F : H, m.x);
    r1c = lerp(r1c, px.z ? D : B, m.z);
    float4 r2c = lerp(E, px.y ? B : F, m.y);
    r2c = lerp(r2c, px.w ? H : D, m.w);
    float3 d1 = abs(E.rgb - r1c.rgb) + abs(E.a - r1c.a), d2 = abs(E.rgb - r2c.rgb) + abs(E.a - r2c.a);
    return dot(d1, 1) <= dot(d2, 1) ? r2c : r1c;
}

float4 ps_main(PSIn i) : SV_Target {
    uint texpage = i.attr.x & 0xFFFF, clut = i.attr.x >> 16;
    uint win = i.attr.y & 0xFFFFF, flags = i.attr.y >> 20;
    float3 rgb = i.col.rgb;
    float mask = (flags & 8) ? 1.0 : 0.0;
    float2 duv = fwidth(i.uv); // texels per pixel (before any branch: derivatives need all four pixels)
    if (flags & 16) {
        // replaced button prompt: texpage = atlas slot, clut = sprite size in texels, uv relative to the sprite
        float2 sz = float2(clut & 255, clut >> 8);
        float2 a = (float2(texpage % 10, texpage / 10) + clamp(i.uv / sz, 0.002, 0.998)) / float2(10, 2); // ICON_COUNT x 2 rows
        float4 t = icons.Sample(icon_smp, a);
        if (t.a < 0.5 || pass_mode == 2) discard;
        t.rgb /= t.a;
        if (flags & 2) rgb = t.rgb;
        else rgb = saturate(t.rgb * i.col.rgb * (255.0 / 128.0));
    } else if (flags & 1) {
        float4 t;
        bool stp;
        if (flags & 64) {
            // HD letter (hd_text.cpp): box = its cell in the glyph atlas, uv relative to the sprite, win = the
            // palette entries of its rim (low 4 bits) and of the letter; coverage blends the two, opacity cuts out
            float2 cell = float2(i.box.x | (i.box.y << 8), i.box.z | (i.box.w << 8));
            float2 g = glyphs.Sample(lin_smp, (cell + (i.uv + HD_PAD) * HD_K) / HD_ATLAS);
            if (g.y < 0.5) discard;
            int2 cl = int2((clut & 63) * 16, (clut >> 6) & 511);
            float4 rim = at(int2(cl.x + (win & 15), cl.y), int2(0, 0)), fill = at(int2(cl.x + ((win >> 4) & 15), cl.y), int2(0, 0));
            t = lerp(rim, fill, g.x);
            stp = (g.x < 0.5 ? rim.a : fill.a) > 0.5;
        } else {
            int2 uvi = (int2)floor(i.uv);
            float2 fr = i.uv - uvi;
            t = texel(uvi, (int2)min(fr * scale, scale - 1), texpage, clut, win);
            uint tv = enc(t);
            stp = (tv & 0x8000) != 0;
            int4 box = (int4)i.box;
            // filter 1 bilinear (small sprites such as letters stay sharp: flag 32), 2 xBR (for them too); 15-bit
            // textures, which may be pictures the game rendered at the upscaled resolution, are never redrawn by xBR
            int mode = filter == 1 && (flags & 32) ? 0 : filter == 2 && ((texpage >> 7) & 3) >= 2 ? 1 : filter;
            if (mode == 2) {
                float4 x = xbr(i.uv, max(duv.x, duv.y), box, texpage, clut, win);
                if (x.a < 0.5) discard;
                t.rgb = x.rgb / x.a;
            } else if (mode == 1) {
                // bilinear between the four nearest texels; transparent texels carry no weight
                float2 uc = i.uv - 0.5;
                int2 b = (int2)floor(uc);
                float2 f = uc - b;
                float4 acc = 0;
                for (int k = 0; k < 4; k++) {
                    int2 o = int2(k & 1, k >> 1);
                    float wgt = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
                    acc += tx(b + o, box, texpage, clut, win) * wgt;
                }
                if (acc.a < 0.5) discard;
                t.rgb = acc.rgb / acc.a;
            } else if (tv == 0) discard;
        }
        if (pass_mode == 1 && stp) discard;
        if (pass_mode == 2 && !stp) discard;
        if (flags & 2) rgb = t.rgb;
        else rgb = saturate(t.rgb * i.col.rgb * (255.0 / 128.0));
        if (stp) mask = 1.0;
    }
    if (!true_color) {
        float3 c8 = rgb * 255.0;
        if (flags & 4) {
            int2 p = (int2)(i.pos.xy) / scale;
            c8 = clamp(c8 + dither[(p.y & 3) * 4 + (p.x & 3)], 0, 255);
        }
        rgb = floor(c8 / 8.0) / 31.0;
    }
    return float4(rgb * src_factor, mask);
}
)" // one literal may hold at most 16 KB (MSVC); the pieces are joined
R"(
// full-rect helpers for uploads and fills
float4 vs_rect(uint id : SV_VertexID) : SV_Position {
    float2 p = float2(id & 1, id >> 1);
    float2 xy = lerp(rect.xy, rect.zw, p);
    return float4(xy.x / (512.0 * scale) - 1.0, 1.0 - xy.y / (256.0 * scale), 0, 1);
}
Texture2D<float4> native : register(t1);
float4 ps_upload(float4 pos : SV_Position) : SV_Target { return native.Load(int3((int2)pos.xy / scale, 0)); }
float4 ps_fill(float4 pos : SV_Position) : SV_Target { return fill_color; }
float4 ps_down(float4 pos : SV_Position) : SV_Target { return vram.Load(int3((int2)pos.xy * scale, 0)); }

// multisampled render target: a region copy into it (from the single-sample copy), the average of a pixel's
// samples into the single-sample copy, and a per-sample copy out of it
float4 ps_copy(float4 pos : SV_Position) : SV_Target { return native.Load(int3((int2)pos.xy + copy_off, 0)); }
float4 ps_resolve(float4 pos : SV_Position) : SV_Target {
    float4 s = 0;
    for (int i = 0; i < samples; i++) s += vram_ms.Load((int2)pos.xy, i);
    return s / samples;
}
float4 ps_copy_ms(float4 pos : SV_Position, uint si : SV_SampleIndex) : SV_Target { return vram_ms.Load((int2)pos.xy + copy_off, si); }

// a triangle over the whole target
float4 vs_full(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0, 1);
}

// HD letter reconstruction (hd_text.cpp), drawn into the letter's atlas cell (rect.xy = its origin) when it is
// first used. glyph_src, per texel on the letter's padded grid with two zero texels beyond each side: opacity and
// coverage (brightness between the rim and the letter colour), interpolated (Catmull-Rom) to the pixel's centre,
// texel j's centre being at (j + 0.5) * HD_K pixels. The letter ends at half coverage. The dark rim, never past
// the original's opaque texels, runs out to where coverage fades (anti-aliased letters: an even rim) or, for
// letters with a drawn outline rect.z texels wide, to that distance from the letter's edge: the most coverage
// on rings around the pixel (16 directions, three radii up to rect.z) passes half, so the outline is the
// letter's smooth shape grown by a disc. Their opacity is interpolated with the smoother B-spline, so where the
// original outline is thinner, or around a see-through counter (0, 6, 8, 9), the cut is round too.
Texture2D<float2> glyph_src : register(t6);
float4 cr_weights(float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5 * float4(-t + 2 * t2 - t3, 2 - 5 * t2 + 3 * t3, t + 4 * t2 - 3 * t3, t3 - t2);
}
float4 bspline_weights(float t) {
    float u = 1 - t, t2 = t * t, t3 = t2 * t;
    return float4(u * u * u, 3 * t3 - 6 * t2 + 4, -3 * t3 + 3 * t2 + 3 * t + 1, t3) / 6.0;
}
// both fields at s (texel units on the padded grid); reads beyond the texture return 0
float2 glyph_fields(float2 s, bool smooth = false) {
    int2 i = (int2)floor(s);
    float4 wx = smooth ? bspline_weights(s.x - i.x) : cr_weights(s.x - i.x), wy = smooth ? bspline_weights(s.y - i.y) : cr_weights(s.y - i.y);
    float2 f = 0;
    [unroll] for (int y = 0; y < 4; y++) {
        float2 r = 0;
        [unroll] for (int x = 0; x < 4; x++) r += wx[x] * glyph_src.Load(int3(i + int2(x, y) + 1, 0));
        f += wy[y] * r;
    }
    return f;
}
float4 ps_glyph(float4 pos : SV_Position) : SV_Target {
    float2 s = (pos.xy - rect.xy) / HD_K - 0.5;
    float2 f = glyph_fields(s);
    float rim, opaque = f.x;
    if (rect.z > 0.5) {
        float grown = f.y;
        [loop] for (int k = 0; k < 16; k++) {
            float2 d;
            sincos(k * 0.3926991, d.y, d.x);
            [unroll] for (int ring = 1; ring <= 3; ring++) grown = max(grown, glyph_fields(s + d * (rect.z * ring / 3.0)).y);
        }
        rim = saturate((grown - 0.5) * HD_K + 0.5);
        opaque = glyph_fields(s, true).x;
    } else {
        rim = saturate((f.y - 0.08) * HD_K * 2.5 + 0.5);
    }
    float a = min(rim, saturate((opaque - 0.15) * HD_K + 0.5));
    return float4(a > 0 ? saturate((f.y - 0.5) * HD_K * 0.7 + 0.5) : 0, a, 0, 0);
}

// FXAA (after Timothy Lottes): find the edge direction from the luma of the four diagonal neighbours, blend
// along it, and fall back to the shorter blend where the longer one leaves the local contrast range.
// rect.xy = the picture's top left in the input, rect.zw = 1 / input size; fill_color = the picture's texel
// centres (u0 v0 u1 v1), so nothing next to it in VRAM bleeds in.
float3 pic(float2 uv) { return picture.SampleLevel(lin_smp, clamp(uv, fill_color.xy, fill_color.zw), 0).rgb; }
float4 ps_fxaa(float4 pos : SV_Position) : SV_Target {
    float2 rcp = rect.zw;
    float2 uv = (rect.xy + pos.xy) * rcp;
    const float3 L = float3(0.299, 0.587, 0.114);
    float3 m = pic(uv);
    float lnw = dot(pic(uv + float2(-1, -1) * rcp), L);
    float lne = dot(pic(uv + float2(1, -1) * rcp), L);
    float lsw = dot(pic(uv + float2(-1, 1) * rcp), L);
    float lse = dot(pic(uv + float2(1, 1) * rcp), L);
    float lm = dot(m, L);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    float2 dir = float2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
    float rmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rmin, -8.0, 8.0) * rcp;
    float3 a = 0.5 * (pic(uv + dir * (1.0 / 3.0 - 0.5)) + pic(uv + dir * (2.0 / 3.0 - 0.5)));
    float3 b = a * 0.5 + 0.25 * (pic(uv - dir * 0.5) + pic(uv + dir * 0.5));
    float lb = dot(b, L);
    return float4(lb < lmin || lb > lmax ? a : b, 1);
}
)";

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

void write_png(const std::string& path, const u8* rgba, int w, int h, int pitch); // platform.cpp

static inline u32 rgb555_to_rgba(u16 p) {
    u32 r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
    auto e = [](u32 c) { return (c * 255 + 15) / 31; };
    return e(r) | (e(g) << 8) | (e(b) << 16) | ((p & 0x8000) ? 0xFF000000u : 0);
}
static inline u16 rgba_to_555(u32 c) {
    auto q = [](u32 v) { return (v * 31 + 127) / 255; };
    return (u16)(q(c & 0xFF) | (q((c >> 8) & 0xFF) << 5) | (q((c >> 16) & 0xFF) << 10) | ((c >> 24) > 127 ? 0x8000 : 0));
}

struct D3DRenderer : Renderer {
    u16* shadow;
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    int S = 1;
    int MS = 1;                    // samples per pixel of rt (MSAA / SSAA)
    int want_ms = 1;               // what the setting asked for when rt was made (the GPU may give fewer)
    // rt: the upscaled VRAM the game draws into (multisampled with MSAA / SSAA); copy: its single-sample copy,
    // kept current per tile, that textures, readbacks and (multisampled) the screen read from
    ID3D11Texture2D* rt = nullptr, * copy = nullptr;
    ID3D11RenderTargetView* rtv = nullptr, * copy_rtv = nullptr;
    ID3D11ShaderResourceView* rt_srv = nullptr, * copy_srv = nullptr;
    ID3D11Texture2D* up = nullptr;
    ID3D11ShaderResourceView* up_srv = nullptr;
    ID3D11ShaderResourceView* icon_srv = nullptr;
    ID3D11SamplerState* icon_smp = nullptr;
    // HD letters (hd_text.cpp): reconstructions packed in rows of equal height, uploaded when first drawn
    static const int GLYPH_ATLAS = 4096;
    static_assert(HD_TEXT_K == 12 && HD_TEXT_PAD == 1, "k_hlsl's HD_K and HD_PAD");
    ID3D11Texture2D* glyph_tex = nullptr;
    ID3D11ShaderResourceView* glyph_srv = nullptr;
    ID3D11RenderTargetView* glyph_rtv = nullptr; // its top mip level, drawn into by ps_glyph
    ID3D11Texture2D* glyph_src = nullptr;        // ps_glyph's input: one letter's texels (hd_text_source)
    ID3D11ShaderResourceView* glyph_src_srv = nullptr;
    ID3D11PixelShader* ps_glyph = nullptr;
    static const int GLYPH_SRC_W = 64, GLYPH_SRC_H = 32;
    static_assert(HD_TEXT_MAX_W + 2 * HD_TEXT_PAD + 4 <= GLYPH_SRC_W && HD_TEXT_MAX_H + 2 * HD_TEXT_PAD + 4 <= GLYPH_SRC_H,
                  "hd_text_source's grid fits glyph_src");
    std::vector<u32> glyph_cell;                 // per glyph id: x | y << 16, ~0u = not in the atlas
    struct Shelf { int y, h, x; };
    std::vector<Shelf> glyph_shelves;
    int glyph_top = 0;
    bool glyph_mips = false;                     // top level changed since the mip chain was made
    ID3D11Texture2D* down_rt = nullptr, * down_stage = nullptr;
    ID3D11RenderTargetView* down_rtv = nullptr;
    ID3D11VertexShader* vs = nullptr, * vs_rect = nullptr, * vs_full = nullptr;
    ID3D11PixelShader* ps = nullptr, * ps_upload = nullptr, * ps_fill = nullptr, * ps_down = nullptr;
    ID3D11PixelShader* ps_sample = nullptr; // ps_main shaded per sample (SSAA)
    ID3D11PixelShader* ps_copy = nullptr, * ps_resolve = nullptr, * ps_copy_ms = nullptr, * ps_fxaa = nullptr;
    ID3D11SamplerState* lin_smp = nullptr;
    // FXAA output: the displayed area at the internal resolution
    ID3D11Texture2D* post = nullptr;
    ID3D11RenderTargetView* post_rtv = nullptr;
    ID3D11ShaderResourceView* post_srv = nullptr;
    int post_w = 0, post_h = 0;
    int copy_off[2] = {0, 0};      // for the next set_cb (ps_copy / ps_copy_ms)
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr, * cb = nullptr;
    ID3D11BlendState* bs_opaque = nullptr, * bs_mode[4] = {};
    ID3D11RasterizerState* rs = nullptr;
    static const int VB_SIZE = 1 << 17;
    std::vector<HwVertex> batch;
    int batch_kind = -1;          // 0 opaque, 1 blended, 2 two-pass
    int batch_mode = 0;
    D3D11_RECT batch_scissor = {};
    bool dirty[16][32] = {};       // 32x32 tiles needing a copy to the sampling texture
    bool any_dirty = false;
    std::vector<u32> frame24;
    // frame interpolation: in-between frames are drawn into irt, which covers only the displayed buffer;
    // the viewport is shifted so primitives keep their VRAM coordinates
    // (multisampled like rt; irt_res is then its resolved picture)
    ID3D11Texture2D* irt = nullptr, * irt_res = nullptr;
    ID3D11RenderTargetView* irtv = nullptr;
    ID3D11ShaderResourceView* irt_srv = nullptr, * irt_res_srv = nullptr;
    int irt_w = 0, irt_h = 0, irt_ms = 0;
    bool redirect = false;          // drawing an in-between frame
    int tox = 0, toy = 0;           // irt's origin in render-target pixels

    explicit D3DRenderer(u16* v) : shadow(v) {
        dev = d3d_device();
        ctx = d3d_context();
        ID3DBlob* b;
        b = d3d_compile(k_hlsl, "vs_main", "vs_5_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
        D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"ATTR", 0, DXGI_FORMAT_R32G32_UINT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BOX", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        dev->CreateInputLayout(il, 5, b->GetBufferPointer(), b->GetBufferSize(), &layout);
        b->Release();
        b = d3d_compile(k_hlsl, "vs_rect", "vs_5_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_rect);
        b->Release();
        b = d3d_compile(k_hlsl, "vs_full", "vs_5_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_full);
        b->Release();
        auto mkps = [&](const char* src, const char* e, ID3D11PixelShader** out) {
            ID3DBlob* x = d3d_compile(src, e, "ps_5_0");
            dev->CreatePixelShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, out);
            x->Release();
        };
        mkps(k_hlsl, "ps_main", &ps);
        mkps(k_hlsl, "ps_upload", &ps_upload);
        mkps(k_hlsl, "ps_fill", &ps_fill);
        mkps(k_hlsl, "ps_down", &ps_down);
        mkps(k_hlsl, "ps_copy", &ps_copy);
        mkps(k_hlsl, "ps_resolve", &ps_resolve);
        mkps(k_hlsl, "ps_copy_ms", &ps_copy_ms);
        mkps(k_hlsl, "ps_fxaa", &ps_fxaa);
        mkps(k_hlsl, "ps_glyph", &ps_glyph);
        mkps((std::string("#define PER_SAMPLE 1\n") + k_hlsl).c_str(), "ps_main", &ps_sample);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &lin_smp);

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = VB_SIZE * sizeof(HwVertex);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &vb);
        bd.ByteWidth = sizeof(DrawCB);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = 0;
        dev->CreateBuffer(&bd, nullptr, &cb);

        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bl, &bs_opaque);
        for (int m = 0; m < 4; m++) {
            D3D11_RENDER_TARGET_BLEND_DESC& r = bl.RenderTarget[0];
            r.BlendEnable = TRUE;
            r.SrcBlend = D3D11_BLEND_ONE;
            r.DestBlend = D3D11_BLEND_BLEND_FACTOR;
            r.BlendOp = m == 2 ? D3D11_BLEND_OP_REV_SUBTRACT : D3D11_BLEND_OP_ADD;
            r.SrcBlendAlpha = D3D11_BLEND_ONE;
            r.DestBlendAlpha = D3D11_BLEND_ZERO;
            r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            dev->CreateBlendState(&bl, &bs_mode[m]);
        }
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        rd.MultisampleEnable = TRUE; // per-sample coverage on multisampled targets (no effect on the others)
        dev->CreateRasterizerState(&rd, &rs);

        // native-resolution helpers
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 1024;
        td.Height = 512;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &up);
        dev->CreateShaderResourceView(up, nullptr, &up_srv);
        create_icon_atlas();
        create_glyph_atlas();
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        dev->CreateTexture2D(&td, nullptr, &down_rt);
        dev->CreateRenderTargetView(down_rt, nullptr, &down_rtv);
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&td, nullptr, &down_stage);

        create_targets(g_settings.scale, aa_samples(g_settings.anti_aliasing));
        upload_rect(0, 0, 1024, 512);
    }

    template <typename T> static void release(T*& p) { if (p) p->Release(); p = nullptr; }

    void create_targets(int scale, int samples) {
        release(rtv); release(rt_srv); release(rt);
        release(copy_rtv); release(copy_srv); release(copy);
        release(irtv); release(irt_srv); release(irt); release(irt_res_srv); release(irt_res);
        irt_w = irt_h = irt_ms = 0;
        S = scale;
        want_ms = samples;
        // samples: as many as asked that the GPU supports, keeping the render target within 512 MB (8x
        // resolution: 4 samples at most)
        const double per_sample = 1024.0 * S * 512.0 * S * 4;
        MS = samples;
        while (MS > 1) {
            UINT q = 0;
            if (per_sample * MS <= 512.0 * 1024 * 1024 &&
                SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, MS, &q)) && q > 0) break;
            MS /= 2;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 1024 * S;
        td.Height = 512 * S;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = MS;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        while (FAILED(dev->CreateTexture2D(&td, nullptr, &rt))) { // out of video memory: fewer samples
            if (td.SampleDesc.Count == 1) fatal("cannot create %dx VRAM render target", S);
            td.SampleDesc.Count /= 2;
        }
        MS = td.SampleDesc.Count;
        dev->CreateRenderTargetView(rt, nullptr, &rtv);
        dev->CreateShaderResourceView(rt, nullptr, &rt_srv);
        td.SampleDesc.Count = 1;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &copy))) fatal("cannot create %dx VRAM copy", S);
        dev->CreateRenderTargetView(copy, nullptr, &copy_rtv);
        dev->CreateShaderResourceView(copy, nullptr, &copy_srv);
        float black[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(rtv, black);
        if (MS > 1) plog("hardware renderer: %dx internal resolution (%dx%d VRAM), %d samples%s\n", S, 1024 * S, 512 * S, MS,
                         MS < samples ? " (fewer than asked: GPU or memory)" : "");
        else plog("hardware renderer: %dx internal resolution (%dx%d VRAM)\n", S, 1024 * S, 512 * S);
    }

    // the resolution or the sample count changed since rt was made
    bool targets_stale() const { return S != g_settings.scale || want_ms != aa_samples(g_settings.anti_aliasing); }

    void set_cb(int pass, float factor, const float* rect = nullptr, const float* fillc = nullptr) {
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
        ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);
    }

    void bind_target() {
        ctx->OMSetRenderTargets(1, redirect ? &irtv : &rtv, nullptr);
        D3D11_VIEWPORT vp = {(float)-tox, (float)-toy, 1024.0f * S, 512.0f * S, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetConstantBuffers(0, 1, &cb);
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
    void sync_copy() {
        if (!any_dirty) return;
        if (MS > 1) { // multisampled: average each pixel's samples into the copy, a draw per run of tiles
            ctx->OMSetRenderTargets(1, &copy_rtv, nullptr);
            D3D11_VIEWPORT vp = {0, 0, 1024.0f * S, 512.0f * S, 0, 1};
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(rs);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
            ctx->VSSetConstantBuffers(0, 1, &cb);
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx->VSSetShader(vs_rect, nullptr, 0);
            ctx->PSSetShader(ps_resolve, nullptr, 0);
            ctx->PSSetShaderResources(3, 1, &rt_srv);
        }
        for (int ty = 0; ty < 16; ty++)
            for (int tx = 0; tx < 32;) {
                if (!dirty[ty][tx]) { tx++; continue; }
                int t0 = tx;
                while (tx < 32 && dirty[ty][tx]) { dirty[ty][tx] = false; tx++; }
                D3D11_BOX box = {(UINT)(t0 * 32 * S), (UINT)(ty * 32 * S), 0, (UINT)(tx * 32 * S), (UINT)((ty + 1) * 32 * S), 1};
                if (MS > 1) {
                    float r[4] = {(float)box.left, (float)box.top, (float)box.right, (float)box.bottom};
                    set_cb(0, 1.0f, r);
                    D3D11_RECT sc = {(LONG)box.left, (LONG)box.top, (LONG)box.right, (LONG)box.bottom};
                    ctx->RSSetScissorRects(1, &sc);
                    ctx->Draw(4, 0);
                } else {
                    ctx->CopySubresourceRegion(copy, 0, box.left, box.top, 0, rt, 0, &box);
                }
            }
        if (MS > 1) {
            ID3D11ShaderResourceView* none = nullptr;
            ctx->PSSetShaderResources(3, 1, &none);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        any_dirty = false;
    }

    // ------------------------------------------------------------ batching
    // button glyph atlas with a mip chain, so the glyphs stay clean at every internal resolution
    void create_icon_atlas() {
        int aw, ah;
        const u32* px = icon_atlas_pixels(aw, ah);
        if (!px) return;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = aw;
        td.Height = ah;
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) { plog("icons: cannot create glyph atlas\n"); return; }
        ctx->UpdateSubresource(tex, 0, nullptr, px, aw * 4, 0);
        dev->CreateShaderResourceView(tex, nullptr, &icon_srv);
        ctx->GenerateMips(icon_srv);
        tex->Release();
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &icon_smp);
    }

    void create_glyph_atlas() {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = GLYPH_ATLAS;
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &glyph_tex))) { plog("hd text: cannot create the glyph atlas\n"); return; }
        dev->CreateShaderResourceView(glyph_tex, nullptr, &glyph_srv);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = td.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev->CreateRenderTargetView(glyph_tex, &rd, &glyph_rtv);
        td.Width = GLYPH_SRC_W;
        td.Height = GLYPH_SRC_H;
        td.MipLevels = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &glyph_src))) {
            plog("hd text: cannot create the glyph source texture\n");
            glyph_srv->Release();
            glyph_srv = nullptr; // glyph_place then always declines
            return;
        }
        dev->CreateShaderResourceView(glyph_src, nullptr, &glyph_src_srv);
        glyph_reset();
    }

    // reconstruct letter g into the atlas cell at x, y (w x h pixels) on the GPU
    void glyph_draw(const HdGlyph& g, int x, int y, int w, int h) {
        u8 src[GLYPH_SRC_W * GLYPH_SRC_H * 2];
        int fw, fh;
        hd_text_source(g, src, GLYPH_SRC_W, fw, fh);
        const D3D11_BOX sb = {0, 0, 0, (UINT)fw, (UINT)fh, 1};
        ctx->UpdateSubresource(glyph_src, 0, &sb, src, GLYPH_SRC_W * 2, 0);
        const float r[4] = {(float)x, (float)y, (float)g.style, 0};
        set_cb(0, 1.0f, r);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(5, 1, &none); // the atlas, about to be drawn into
        ctx->OMSetRenderTargets(1, &glyph_rtv, nullptr);
        const D3D11_VIEWPORT vp = {(float)x, (float)y, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        const D3D11_RECT sc = {x, y, x + w, y + h};
        ctx->RSSetScissorRects(1, &sc);
        ctx->RSSetState(rs);
        ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_full, nullptr, 0);
        ctx->PSSetShader(ps_glyph, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetShaderResources(6, 1, &glyph_src_srv);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(6, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr); // the next pass binds its own target
    }

    void glyph_reset() {
        static const float zero[4] = {};
        if (glyph_rtv) ctx->ClearRenderTargetView(glyph_rtv, zero);
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
        if (!g || !glyph_srv) return false;
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

    void flush() override {
        if (batch.empty()) return;
        if (glyph_mips && glyph_srv) { ctx->GenerateMips(glyph_srv); glyph_mips = false; }
        bind_target();
        ctx->RSSetScissorRects(1, &batch_scissor);
        ctx->IASetInputLayout(layout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(MS > 1 && aa_per_sample(g_settings.anti_aliasing) ? ps_sample : ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &copy_srv);
        ctx->PSSetShaderResources(2, 1, &icon_srv);
        ctx->PSSetShaderResources(5, 1, &glyph_srv);
        ctx->PSSetSamplers(0, 1, &icon_smp);
        ctx->PSSetSamplers(1, 1, &lin_smp);
        UINT stride = sizeof(HwVertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        size_t done = 0;
        while (done < batch.size()) {
            size_t n = std::min(batch.size() - done, (size_t)VB_SIZE);
            D3D11_MAPPED_SUBRESOURCE m;
            ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            memcpy(m.pData, batch.data() + done, n * sizeof(HwVertex));
            ctx->Unmap(vb, 0);
            static const float factors[4] = {0.5f, 1.0f, 1.0f, 0.25f};
            float bf[4] = {batch_mode == 0 ? 0.5f : 1.0f, batch_mode == 0 ? 0.5f : 1.0f, batch_mode == 0 ? 0.5f : 1.0f, 1.0f};
            if (batch_kind == 0) {
                set_cb(0, 1.0f);
                ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
                ctx->Draw((UINT)n, 0);
            } else if (batch_kind == 1) {
                set_cb(0, factors[batch_mode]);
                ctx->OMSetBlendState(bs_mode[batch_mode], bf, 0xFFFFFFFF);
                ctx->Draw((UINT)n, 0);
            } else {
                set_cb(1, 1.0f);
                ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
                ctx->Draw((UINT)n, 0);
                set_cb(2, factors[batch_mode]);
                ctx->OMSetBlendState(bs_mode[batch_mode], bf, 0xFFFFFFFF);
                ctx->Draw((UINT)n, 0);
            }
            done += n;
        }
        batch.clear();
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    }

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
        D3D11_RECT sc = {st.clip_x1 * S - tox, st.clip_y1 * S - toy, (st.clip_x2 + 1) * S - tox, (st.clip_y2 + 1) * S - toy};
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
    void draw_rect_pass(ID3D11PixelShader* p, ID3D11ShaderResourceView* srv, int x, int y, int w, int h, const float* col) {
        bind_target();
        float r[4] = {(float)(x * S), (float)(y * S), (float)((x + w) * S), (float)((y + h) * S)};
        set_cb(0, 1.0f, r, col);
        D3D11_RECT sc = {x * S - tox, y * S - toy, (x + w) * S - tox, (y + h) * S - toy};
        ctx->RSSetScissorRects(1, &sc);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs_rect, nullptr, 0);
        ctx->PSSetShader(p, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {nullptr, srv};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
        ctx->Draw(4, 0);
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        ctx->PSSetShaderResources(0, 2, none);
        if (!redirect) mark_dirty(x, y, x + w - 1, y + h - 1);
    }

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
        D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
        ctx->UpdateSubresource(up, 0, &box, tmp.data(), w * 4, 0);
        draw_rect_pass(ps_upload, up_srv, x, y, w, h, nullptr);
    }

    void fill(int x, int y, int w, int h, u32 c) override {
        flush();
        if (w <= 0 || h <= 0) return;
        u16 p = (u16)(((c & 0xFF) >> 3) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) shadow[((y + yy) & 511) * 1024 + ((x + xx) & 1023)] = p;
        float col[4] = {(c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, 0};
        if (!g_settings.true_color) for (int k = 0; k < 3; k++) col[k] = std::floor(col[k] * 255.0f / 8.0f) / 31.0f;
        split(x, y, w, h, [&](int a, int b, int cw, int ch) { draw_rect_pass(ps_fill, nullptr, a, b, cw, ch, col); });
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
        if (MS > 1) { // a multisampled texture takes no region copies: draw the source over the destination
            copy_off[0] = (sx - dx) * S;
            copy_off[1] = (sy - dy) * S;
            draw_rect_pass(ps_copy, copy_srv, dx, dy, w, h, nullptr);
            copy_off[0] = copy_off[1] = 0;
            return;
        }
        D3D11_BOX box = {(UINT)(sx * S), (UINT)(sy * S), 0, (UINT)((sx + w) * S), (UINT)((sy + h) * S), 1};
        ctx->CopySubresourceRegion(rt, 0, dx * S, dy * S, 0, copy, 0, &box);
        mark_dirty(dx, dy, dx + w - 1, dy + h - 1);
    }

    void download(int x, int y, int w, int h) {
        // downsample the upscaled VRAM (its single-sample copy, brought up to date), then read it back
        sync_copy();
        bind_target();
        ctx->OMSetRenderTargets(1, &down_rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, 1024, 512, 0, 1};
        ctx->RSSetViewports(1, &vp);
        float r[4] = {(float)(x * S), (float)(y * S), (float)((x + w) * S), (float)((y + h) * S)};
        set_cb(0, 1.0f, r, nullptr);
        D3D11_RECT sc = {x, y, x + w, y + h};
        ctx->RSSetScissorRects(1, &sc);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs_rect, nullptr, 0);
        ctx->PSSetShader(ps_down, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &copy_srv);
        ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
        // vs_rect divides by the scale, so feed it native coordinates scaled by S on a 1024x512 target
        ctx->Draw(4, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
        ctx->CopySubresourceRegion(down_stage, 0, x, y, 0, down_rt, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(down_stage, 0, D3D11_MAP_READ, 0, &m))) {
            for (int yy = 0; yy < h; yy++) {
                const u32* row = (const u32*)((const u8*)m.pData + (y + yy) * m.RowPitch);
                for (int xx = 0; xx < w; xx++) shadow[(y + yy) * 1024 + x + xx] = rgba_to_555(row[x + xx]);
            }
            ctx->Unmap(down_stage, 0);
        }
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

    bool interp_ready(const DisplayInfo& d) override {
        if (d.depth24) return false;
        flush();
        if (targets_stale()) rescale(); // F1/F2/F3: the new resolution or sample count first
        int w = std::min(d.w, 1024 - d.x) * S, h = std::min(d.h, 512 - d.y) * S;
        if (w <= 0 || h <= 0) return false;
        if (w != irt_w || h != irt_h || MS != irt_ms) {
            release(irtv); release(irt_srv); release(irt); release(irt_res_srv); release(irt_res);
            irt_w = irt_h = irt_ms = 0;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = MS;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &irt))) return false;
            dev->CreateRenderTargetView(irt, nullptr, &irtv);
            dev->CreateShaderResourceView(irt, nullptr, &irt_srv);
            if (MS > 1) {
                td.SampleDesc.Count = 1;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(dev->CreateTexture2D(&td, nullptr, &irt_res))) { release(irtv); release(irt_srv); release(irt); return false; }
                dev->CreateShaderResourceView(irt_res, nullptr, &irt_res_srv);
            }
            irt_w = w;
            irt_h = h;
            irt_ms = MS;
        }
        return true;
    }

    bool interp_begin(const DisplayInfo& d) override {
        if (!interp_ready(d)) return false;
        // start from the buffer on screen: whatever the frame does not redraw stays as it is
        if (MS > 1) { // sample by sample (no region copies between multisampled textures)
            ctx->OMSetRenderTargets(1, &irtv, nullptr);
            D3D11_VIEWPORT vp = {0, 0, (float)irt_w, (float)irt_h, 0, 1};
            ctx->RSSetViewports(1, &vp);
            D3D11_RECT sc = {0, 0, irt_w, irt_h};
            ctx->RSSetScissorRects(1, &sc);
            ctx->RSSetState(rs);
            ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
            copy_off[0] = d.x * S;
            copy_off[1] = d.y * S;
            set_cb(0, 1.0f);
            copy_off[0] = copy_off[1] = 0;
            ctx->VSSetConstantBuffers(0, 1, &cb);
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs_full, nullptr, 0);
            ctx->PSSetShader(ps_copy_ms, nullptr, 0);
            ctx->PSSetShaderResources(3, 1, &rt_srv);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            ctx->PSSetShaderResources(3, 1, &none);
        } else {
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            D3D11_BOX box = {(UINT)(d.x * S), (UINT)(d.y * S), 0, (UINT)(d.x * S + irt_w), (UINT)(d.y * S + irt_h), 1};
            ctx->CopySubresourceRegion(irt, 0, 0, 0, 0, rt, 0, &box);
        }
        redirect = true;
        tox = d.x * S;
        toy = d.y * S;
        batch_kind = -1; // new batch: different target and scissor offset
        return true;
    }

    void interp_tri(const VertexF* v, int flags, const GpuDrawState& st) override { tri_f(v, flags, st); }
    void interp_rect(float x, float y, s32 w, s32 h, u8 r, u8 g, u8 b, u8 u0, u8 v0, int flags, const GpuDrawState& st, bool fx, bool fy) override {
        rect_f(x, y, w, h, r, g, b, u0, v0, flags, st, fx, fy);
    }
    void interp_line(const VertexF& a, const VertexF& b, int flags, const GpuDrawState& st) override { line_f(a, b, flags, st); }

    void interp_fill(int x, int y, int w, int h, u32 c) override { // like fill(), without touching VRAM
        flush();
        if (w <= 0 || h <= 0) return;
        float col[4] = {(c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, 0};
        if (!g_settings.true_color) for (int k = 0; k < 3; k++) col[k] = std::floor(col[k] * 255.0f / 8.0f) / 31.0f;
        split(x, y, w, h, [&](int a, int b, int cw, int ch) { draw_rect_pass(ps_fill, nullptr, a, b, cw, ch, col); });
        batch_kind = -1;
    }

    void interp_present(const DisplayInfo& d, bool pace) override {
        flush();
        redirect = false;
        tox = toy = 0;
        batch_kind = -1;
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (MS > 1) ctx->ResolveSubresource(irt_res, 0, irt, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        show(MS > 1 ? irt_res_srv : irt_srv, 0, 0, irt_w, irt_h, irt_w, irt_h, d, pace);
    }

    // ------------------------------------------------------------ the picture on screen
    // FXAA over w x h pixels at (x, y) of src (tw x th) into `post`; false if it could not
    bool fxaa(ID3D11ShaderResourceView* src, int x, int y, int w, int h, int tw, int th) {
        w = std::min(w, tw - x);
        h = std::min(h, th - y);
        if (w <= 0 || h <= 0) return false;
        if (w != post_w || h != post_h) {
            release(post_rtv); release(post_srv); release(post);
            post_w = post_h = 0;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &post))) return false;
            dev->CreateRenderTargetView(post, nullptr, &post_rtv);
            dev->CreateShaderResourceView(post, nullptr, &post_srv);
            post_w = w;
            post_h = h;
        }
        ctx->OMSetRenderTargets(1, &post_rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc = {0, 0, w, h};
        ctx->RSSetScissorRects(1, &sc);
        ctx->RSSetState(rs);
        ctx->OMSetBlendState(bs_opaque, nullptr, 0xFFFFFFFF);
        float r[4] = {(float)x, (float)y, 1.0f / tw, 1.0f / th};
        float lim[4] = {(x + 0.5f) / tw, (y + 0.5f) / th, (x + w - 0.5f) / tw, (y + h - 0.5f) / th};
        set_cb(0, 1.0f, r, lim);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_full, nullptr, 0);
        ctx->PSSetShader(ps_fxaa, nullptr, 0);
        ctx->PSSetShaderResources(4, 1, &src);
        ctx->PSSetSamplers(1, 1, &lin_smp);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(4, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        return true;
    }

    // show w x h pixels at (x, y) of src (tw x th), through FXAA when it is on; pace = wait for the next vblank
    void show(ID3D11ShaderResourceView* src, int x, int y, int w, int h, int tw, int th, const DisplayInfo& d, bool pace) {
        ID3D11ShaderResourceView* srv = src;
        float u0 = (float)x / tw, v0 = (float)y / th, u1 = (float)(x + w) / tw, v1 = (float)(y + h) / th;
        if (g_settings.anti_aliasing == AA_FXAA && fxaa(src, x, y, w, h, tw, th)) {
            srv = post_srv;
            u0 = v0 = 0;
            u1 = v1 = 1;
            w = post_w;
            h = post_h;
        }
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (pace) d3d_present_texture(srv, u0, v0, u1, v1, w, h, d.h, d.enabled);
        else d3d_show(srv, u0, v0, u1, v1, w, h, d.h, d.enabled);
    }

    void present(const DisplayInfo& d) override {
        flush();
        if (targets_stale()) rescale();
        static int dump_at = getenv("TB_VRAM_DUMP") ? atoi(getenv("TB_VRAM_DUMP")) : -1, presents = 0;
        if (++presents == dump_at) { // debugging aid: CPU copy and downsampled GPU copy of VRAM
            std::string dir = getenv("TB_SHOT_DIR") ? getenv("TB_SHOT_DIR") : g_base_dir;
            std::vector<u32> px(1024 * 512);
            for (int i = 0; i < 1024 * 512; i++) px[i] = rgb555_to_rgba(shadow[i]) | 0xFF000000u;
            write_png(dir + "\\vram_cpu.png", (const u8*)px.data(), 1024, 512, 4096);
            download(0, 0, 1024, 512);
            for (int i = 0; i < 1024 * 512; i++) px[i] = rgb555_to_rgba(shadow[i]) | 0xFF000000u;
            write_png(dir + "\\vram_gpu.png", (const u8*)px.data(), 1024, 512, 4096);
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
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (MS > 1) sync_copy(); // the screen shows the resolved copy
        show(MS > 1 ? copy_srv : rt_srv, d.x * S, d.y * S, d.w * S, d.h * S, 1024 * S, 512 * S, d, true);
    }
};

Renderer* renderer_create_d3d11(u16* vram) {
    if (!d3d_device()) return nullptr;
    return new D3DRenderer(vram);
}
