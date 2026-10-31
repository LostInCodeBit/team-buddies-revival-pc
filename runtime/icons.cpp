// icons.cpp - button prompt icons that follow the input device in use (keyboard / PlayStation / Xbox).
//
// The game draws its prompts (cross, circle, square, triangle, start, ...) as small 4bpp sprites.
// Each textured primitive's texture rectangle is identified by a hash of its decoded texels (so it
// does not matter where in VRAM or with which CLUT the game put them); a match is redrawn by the
// renderer from a high-resolution glyph atlas (icon_atlas.cpp) instead of from VRAM.
//
// TB_GPU_TRACE=frame[:count][,frame[:count]...] is the debugging aid used to catalogue the icons: it
// logs every textured primitive of those frames with its hash and dumps each small texture as PNG.
#include "psx.h"
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>
#include <unordered_map>

void write_png(const std::string& path, const u8* rgba, int w, int h, int pitch);

InputDevice g_input_device = DEV_PLAYSTATION;

// ---------------------------------------------------------------- texel decoding (as the GPU samples)
static u16 texel(const u16* vram, u16 texpage, u16 clut, int u, int v) {
    int tx = (texpage & 0xF) * 64, ty = ((texpage >> 4) & 1) * 256, depth = (texpage >> 7) & 3;
    int cx = (clut & 0x3F) * 16, cy = (clut >> 6) & 0x1FF;
    int row = ((ty + v) & 511) * 1024;
    switch (depth) {
    case 0: {
        u16 w = vram[row + ((tx + u / 4) & 1023)];
        return vram[cy * 1024 + ((cx + ((w >> ((u & 3) * 4)) & 0xF)) & 1023)];
    }
    case 1: {
        u16 w = vram[row + ((tx + u / 2) & 1023)];
        return vram[cy * 1024 + ((cx + ((w >> ((u & 1) * 8)) & 0xFF)) & 1023)];
    }
    default: return vram[row + ((tx + u) & 1023)];
    }
}

static u64 hash_rect(u16 texpage, u16 clut, int u0, int v0, int w, int h) {
    u64 x = 1469598103934665603ull;
    auto mix = [&](u32 b) { x = (x ^ b) * 1099511628211ull; };
    mix((u32)w);
    mix((u32)h);
    const u16* vram = gpu_vram();
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) mix(texel(vram, texpage, clut, (u0 + i) & 255, (v0 + j) & 255) & 0x7FFF);
    return x;
}

// ---------------------------------------------------------------- catalogue
// Texture rectangles of the game's prompts (hash of the decoded 15-bit texels, see hash_rect).
struct KnownIcon { u64 hash; IconId id; int w, h; };
static const KnownIcon k_icons[] = {
#include "icon_hashes.inc"
};

// cache: same primitive key and no VRAM upload since -> same answer
struct Key {
    u16 texpage, clut;
    u8 u0, v0, w, h;
    bool operator==(const Key& o) const { return memcmp(this, &o, sizeof *this) == 0; }
};
struct KeyHash { size_t operator()(const Key& k) const { u64 v; memcpy(&v, &k, 8); return std::hash<u64>()(v); } };
static std::unordered_map<Key, int, KeyHash> s_cache;
static u32 s_cache_gen = ~0u;
static u32 s_vram_gen = 0;

void icons_vram_changed() { s_vram_gen++; }

static const bool s_any_icons = sizeof(k_icons) / sizeof(k_icons[0]) > 1; // first entry is a sentinel

// returns the icon drawn by a textured primitive whose texels span [u0,u0+w) x [v0,v0+h), or ICON_NONE
IconId icons_lookup(const GpuDrawState& st, int u0, int v0, int w, int h) {
    if (!s_any_icons || w <= 0 || h <= 0 || w > 64 || h > 64) return ICON_NONE;
    bool size_ok = false; // cheap reject: thousands of 3D polygons per frame come through here
    for (const KnownIcon& ki : k_icons) if (ki.w == w && ki.h == h) { size_ok = true; break; }
    if (!size_ok) return ICON_NONE;
    if (s_cache_gen != s_vram_gen) { s_cache.clear(); s_cache_gen = s_vram_gen; }
    if (s_cache.size() > 8192) s_cache.clear();
    Key k = {st.texpage, st.clut, (u8)u0, (u8)v0, (u8)w, (u8)h};
    auto it = s_cache.find(k);
    if (it != s_cache.end()) return (IconId)it->second;
    u64 hv = hash_rect(st.texpage, st.clut, u0, v0, w, h);
    IconId id = ICON_NONE;
    for (const KnownIcon& ki : k_icons) if (ki.hash == hv) { id = ki.id; break; }
    s_cache[k] = id;
    return id;
}

// ---------------------------------------------------------------- TB_GPU_TRACE
static std::vector<std::pair<u64, u64>> s_trace_ranges; // [first, last]
static bool s_tracing = false;
bool gpu_tracing() { return s_tracing; }
static int s_trace_n = 0;

void icons_frame_start(u64 frame) {
    static bool init = false;
    if (!init) {
        init = true;
        if (const char* t = getenv("TB_GPU_TRACE")) {
            for (const char* p = t; *p;) {
                u64 f = strtoull(p, (char**)&p, 10), n = 4;
                if (*p == ':') n = strtoull(p + 1, (char**)&p, 10);
                s_trace_ranges.push_back({f, f + n - 1});
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
    }
    s_tracing = false;
    for (auto& r : s_trace_ranges) if (frame >= r.first && frame <= r.second) s_tracing = true;
    if (s_tracing) plog("gpu trace: frame %llu\n", frame);
}

static u32 rgba_of(u16 c) {
    if (c == 0) return 0; // fully transparent texel
    u32 r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    return r | g << 8 | b << 16 | 0xFF000000u;
}

// uv rectangle [u0,u1) x [v0,v1) drawn at screen (x, y, w, h)
void icons_trace(const char* kind, s32 x, s32 y, s32 w, s32 h, int u0, int v0, int u1, int v1, int flags, const GpuDrawState& st) {
    if (!s_tracing) return;
    if (!(flags & PF_TEXTURED)) {
        plog("  %s scr=%d,%d %dx%d fl=%X mask=%d/%d clip=%d,%d-%d,%d\n", kind, x, y, w, h, flags, st.set_mask, st.check_mask,
             st.clip_x1, st.clip_y1, st.clip_x2, st.clip_y2);
        return;
    }
    int tw = u1 - u0, th = v1 - v0;
    bool small = tw > 0 && th > 0 && tw <= 64 && th <= 64;
    u64 hv = small ? hash_rect(st.texpage, st.clut, u0, v0, tw, th) : 0;
    plog("  %s scr=%d,%d %dx%d tpage=%04X clut=%04X uv=%d,%d %dx%d fl=%X hash=%016llX\n", kind, x, y, w, h, st.texpage, st.clut,
         u0, v0, tw, th, flags, hv);
    if (!small) return;
    static std::set<u64> seen;
    if (!seen.insert(hv).second) return;
    std::vector<u32> px(tw * th);
    for (int j = 0; j < th; j++)
        for (int i = 0; i < tw; i++) px[j * tw + i] = rgba_of(texel(gpu_vram(), st.texpage, st.clut, (u0 + i) & 255, (v0 + j) & 255));
    const char* dir = getenv("TB_SHOT_DIR");
    char name[96];
    snprintf(name, sizeof name, PATH_SEP "tex_%016llX_%dx%d.png", (unsigned long long)hv, tw, th);
    write_png(std::string(dir ? dir : g_base_dir.c_str()) + name, (const u8*)px.data(), tw, th, tw * 4);
    s_trace_n++;
}

// ---------------------------------------------------------------- replacement
void icons_apply(GpuDrawState& ds, int u0, int v0, int w, int h) {
    ds.icon = -1;
    if (g_input_device == DEV_PLAYSTATION) return; // the game's own icons
    int slot = icon_slot(g_input_device, icons_lookup(ds, u0, v0, w, h));
    if (slot < 0) return;
    ds.icon = (s8)slot;
    ds.icon_u = (u8)u0;
    ds.icon_v = (u8)v0;
    ds.icon_w = (u8)w;
    ds.icon_h = (u8)h;
}
