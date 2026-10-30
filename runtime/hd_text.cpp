// hd_text.cpp - the game's letters drawn from smooth high-resolution reconstructions (hardware renderer).
//
// The game writes with three fonts, 8, 12 and 16 texels high: 4bpp sprites anti-aliased against black through
// a palette (white letter, grey edge texels). Upscaled, they show blocks (nearest), blur (bilinear) or
// jagged stairs (xBR, which expects hard-edged pixel art). Each letter sprite is recognised by a hash of its
// palette indices (glyph_hashes.inc, written by tools/glyph_hashes.py), so where the
// font sits in VRAM and which palette draws it don't matter. The renderer then draws the sprite from a
// reconstruction at HD_TEXT_K times the resolution, made on the GPU when the letter is first drawn (ps_glyph in
// gpu_d3d11.cpp): the palette's brightness gives every texel's coverage, Catmull-Rom interpolation turns it into
// a smooth field, half coverage is the letter's edge and the field's foot just outside it a dark rim of even
// width. Same shapes and colours as the original, with hard, smooth edges at any internal resolution.
#include "psx.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

struct KnownGlyph { u64 hash; u8 w, h, style; }; // style: see HdGlyph
static const KnownGlyph k_glyphs[] = {
#include "glyph_hashes.inc"
};

static void mix(u64& x, u32 b) { x = (x ^ b) * 1099511628211ull; }

// letters seen so far, with the palette they were first drawn with (a letter in a different palette is a
// letter of its own); ids index s_glyphs
static std::vector<HdGlyph> s_glyphs;
static std::unordered_map<u64, int> s_by_key;
static const size_t MAX_GLYPHS = 4096; // a few hundred in practice; beyond this the game's sprites are kept

// cache: same sprite rectangle and no VRAM change since -> same answer
struct Key {
    u16 texpage, clut;
    u8 u0, v0, w, h;
    bool operator==(const Key& o) const { return memcmp(this, &o, sizeof *this) == 0; }
};
struct KeyHash { size_t operator()(const Key& k) const { u64 v; memcpy(&v, &k, 8); return std::hash<u64>()(v); } };
static std::unordered_map<Key, int, KeyHash> s_cache;
static u32 s_vram_gen = 0, s_cache_gen = ~0u;

void hd_text_vram_changed() { s_vram_gen++; }

const HdGlyph* hd_text_glyph(int id) { return id >= 0 && id < (int)s_glyphs.size() ? &s_glyphs[id] : nullptr; }

static int lum(u16 c) { return (c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31); }

// every opaque colour the letter uses lies (within a step of 5-bit rounding) on the line from the rim colour to
// the letter colour, so drawing it with those two blended by coverage keeps its look; a gradient through other
// colours (the park map's red-to-yellow titles) keeps the game's sprite
static bool two_tone(const HdGlyph& g) {
    auto rgb = [](u16 c, int* o) { o[0] = c & 31; o[1] = (c >> 5) & 31; o[2] = (c >> 10) & 31; };
    int a[3], b[3], d[3], p[3];
    rgb(g.pal[g.lo], a);
    rgb(g.pal[g.hi], b);
    for (int k = 0; k < 3; k++) d[k] = b[k] - a[k];
    const int dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    for (u8 k : g.idx) {
        if (!g.pal[k]) continue;
        rgb(g.pal[k], p);
        const float t = dd ? std::clamp(((p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1] + (p[2] - a[2]) * d[2]) / (float)dd, 0.0f, 1.0f) : 0.0f;
        float e = 0;
        for (int c = 0; c < 3; c++) { const float q = p[c] - (a[c] + t * d[c]); e += q * q; }
        if (e > 3.0f) return false;
    }
    return true;
}

static int find(u16 texpage, u16 clut, int u0, int v0, int w, int h) {
    static std::unordered_map<u64, u8> known; // hash -> style
    static bool size_ok[HD_TEXT_MAX_W + 1][HD_TEXT_MAX_H + 1];
    if (known.empty())
        for (const KnownGlyph& k : k_glyphs) { known[k.hash] = k.style; size_ok[k.w][k.h] = true; }
    if (w > HD_TEXT_MAX_W || h > HD_TEXT_MAX_H || !size_ok[w][h]) return -1; // cheap reject: most sprites and every polygon
    const u16* vram = gpu_vram();
    const int tx = (texpage & 0xF) * 64, ty = ((texpage >> 4) & 1) * 256;
    HdGlyph g = {};
    g.w = (u8)w;
    g.h = (u8)h;
    g.idx.resize(w * h);
    u64 x = 1469598103934665603ull;
    mix(x, w);
    mix(x, h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const int u = (u0 + i) & 255;
            const u16 t = vram[((ty + v0 + j) & 511) * 1024 + ((tx + u / 4) & 1023)];
            const u8 k = (t >> ((u & 3) * 4)) & 0xF;
            g.idx[j * w + i] = k;
            mix(x, k);
        }
    const auto kn = known.find(x);
    if (kn == known.end()) return -1;
    g.style = kn->second;
    const int cx = (clut & 0x3F) * 16, cy = (clut >> 6) & 0x1FF;
    u64 key = x;
    for (int k = 0; k < 16; k++) {
        g.pal[k] = vram[cy * 1024 + ((cx + k) & 1023)];
        mix(key, g.pal[k]);
    }
    auto it = s_by_key.find(key);
    if (it != s_by_key.end()) return it->second;
    // rim and letter colours: the darkest and brightest opaque entries the letter uses
    int lo = -1, hi = -1;
    for (u8 k : g.idx) {
        if (!g.pal[k]) continue; // 0x0000 is see-through
        if (lo < 0 || lum(g.pal[k]) < lum(g.pal[lo])) lo = k;
        if (hi < 0 || lum(g.pal[k]) > lum(g.pal[hi])) hi = k;
    }
    if (lo < 0 || s_glyphs.size() >= MAX_GLYPHS) return -1;
    g.lo = (u8)lo;
    g.hi = (u8)hi;
    if (!two_tone(g)) return s_by_key[key] = -1;
    s_glyphs.push_back(std::move(g));
    s_by_key[key] = (int)s_glyphs.size() - 1;
    return (int)s_glyphs.size() - 1;
}

void hd_text_apply(GpuDrawState& ds, int u0, int v0, int w, int h) {
    ds.glyph = -1;
    if (!g_settings.hd_text || g_settings.renderer != 1 || ds.icon >= 0 || w <= 0 || h <= 0) return;
    if (((ds.texpage >> 7) & 3) != 0 || ds.tex_mask_x || ds.tex_mask_y) return; // 4bpp, no texture window
    if (s_cache_gen != s_vram_gen) { s_cache.clear(); s_cache_gen = s_vram_gen; }
    if (s_cache.size() > 8192) s_cache.clear();
    const Key k = {ds.texpage, ds.clut, (u8)u0, (u8)v0, (u8)std::min(w, 255), (u8)std::min(h, 255)};
    auto it = s_cache.find(k);
    const int id = it != s_cache.end() ? it->second : (s_cache[k] = find(ds.texpage, ds.clut, u0, v0, w, h));
    ds.glyph = (s16)id;
}

// ---------------------------------------------------------------- reconstruction source
// The renderer redraws each letter at HD_TEXT_K times its size on the GPU (ps_glyph in gpu_d3d11.cpp) from these
// per-texel pairs: opacity and coverage, the latter being the brightness between the rim and the letter colour.
// The letter's texels sit HD_TEXT_PAD texels in from the padded grid, which has two more zero texels on every
// side so the interpolation never reads outside it.
void hd_text_source(const HdGlyph& g, u8* rg, int pitch, int& fw, int& fh) {
    const int P = HD_TEXT_PAD;
    fw = g.w + 2 * P + 4;
    fh = g.h + 2 * P + 4;
    for (int j = 0; j < fh; j++) memset(rg + j * pitch * 2, 0, fw * 2);
    const int l0 = lum(g.pal[g.lo]), l1 = lum(g.pal[g.hi]);
    for (int j = 0; j < g.h; j++)
        for (int i = 0; i < g.w; i++) {
            const u16 col = g.pal[g.idx[j * g.w + i]];
            if (!col) continue; // 0x0000 is see-through
            u8* t = rg + ((j + P + 2) * pitch + i + P + 2) * 2;
            t[0] = 255;
            t[1] = l1 > l0 ? (u8)std::clamp((lum(col) - l0) * 255 / (l1 - l0), 0, 255) : 255;
        }
}