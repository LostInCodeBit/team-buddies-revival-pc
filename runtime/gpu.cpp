// gpu.cpp - GPU command processor (GP0/GP1), VRAM transfers and display state.
// Drawing is delegated to a Renderer (software reference or Direct3D 11 with upscaling).
#include "psx.h"
#include <algorithm>
#include <cstdlib>

static u16* s_vram;           // CPU-visible copy of VRAM (1024x512, 15-bit)
Renderer* g_renderer;

static GpuDrawState st;
static u32 s_e1;              // last GP0(E1) value
static s16 s_offx, s_offy;
static bool s_tex_disable_allowed;

// display
static u32 s_disp_x, s_disp_y, s_hrange = 0xC60260, s_vrange = 0x3FC10, s_dmode;
static bool s_disp_off = true;
static u32 s_dma_dir;
static bool s_irq;
static u32 s_field;

// command assembly
static u32 s_cmd[64];
static int s_cmd_len, s_cmd_need;
static bool s_polyline;

// CPU -> VRAM transfer
static bool s_upload;
static int s_up_x, s_up_y, s_up_w, s_up_h, s_up_n, s_up_total;
static std::vector<u16> s_up_buf;
// VRAM -> CPU transfer
static bool s_download;
static int s_dn_x, s_dn_y, s_dn_w, s_dn_h, s_dn_i, s_dn_total;
static u32 s_gpuread;

u16* gpu_vram() { return s_vram; }

void gpu_init() {
    s_vram = new u16[1024 * 512];
    memset(s_vram, 0, 1024 * 512 * 2);
    gpu_reset();
}

void gpu_reset() {
    st = GpuDrawState{}; // not memset: icon must stay -1, or untextured primitives get drawn as glyphs
    st.clip_x2 = 1023; st.clip_y2 = 511;
    s_e1 = 0;
    s_offx = s_offy = 0;
    s_disp_x = s_disp_y = 0;
    s_hrange = 0xC60260; s_vrange = 0x3FC10;
    s_dmode = 0;
    s_disp_off = true;
    s_dma_dir = 0;
    s_irq = false;
    s_cmd_len = 0;
    s_upload = s_download = false;
}

static u32 gpustat() {
    u32 v = s_e1 & 0x7FF;
    if (st.set_mask) v |= 1u << 11;
    if (st.check_mask) v |= 1u << 12;
    v |= 1u << 13;                         // interlace field (always "odd" when not interlaced)
    if (s_e1 & 0x800) v |= 1u << 15;
    v |= ((s_dmode >> 6) & 1) << 16;
    v |= (s_dmode & 3) << 17;
    v |= ((s_dmode >> 2) & 1) << 19;
    v |= ((s_dmode >> 3) & 1) << 20;
    v |= ((s_dmode >> 4) & 1) << 21;
    v |= ((s_dmode >> 5) & 1) << 22;
    if (s_disp_off) v |= 1u << 23;
    if (s_irq) v |= 1u << 24;
    v |= 1u << 26;                         // ready for command
    if (s_download) v |= 1u << 27;         // ready to send VRAM
    v |= 1u << 28;                         // ready for DMA block
    v |= s_dma_dir << 29;
    switch (s_dma_dir) {
    case 1: case 2: v |= 1u << 25; break;
    case 3: if (s_download) v |= 1u << 25; break;
    }
    // even/odd line: toggles per scanline (or per field when interlaced)
    u64 line = (cpu_now() / CYCLES_PER_LINE) % LINES_PER_FRAME;
    bool interlaced = (s_dmode & 0x24) == 0x24;
    if (interlaced) { if (s_field) v |= 1u << 31; v &= ~(1u << 13); if (s_field) v |= 1u << 13; }
    else if (line & 1) v |= 1u << 31;
    if (line >= 240) v &= ~(1u << 31);
    return v;
}

DisplayInfo gpu_display() {
    static const int hres[4] = {256, 320, 512, 640};
    DisplayInfo d;
    d.x = (int)s_disp_x;
    d.y = (int)s_disp_y;
    d.w = (s_dmode & 0x40) ? 368 : hres[s_dmode & 3];
    int y1 = (int)(s_vrange & 0x3FF), y2 = (int)((s_vrange >> 10) & 0x3FF);
    int h = y2 - y1;
    if (h <= 0 || h > 256) h = 240;
    d.interlaced = (s_dmode & 0x24) == 0x24;
    d.h = d.interlaced ? h * 2 : h;
    d.depth24 = (s_dmode & 0x10) != 0;
    d.enabled = !s_disp_off;
    return d;
}

void gpu_vblank() {
    s_field ^= 1;
    if (g_renderer) {
        DisplayInfo d = gpu_display();
        if (!interp_present(d)) g_renderer->present(d); // frame_rate 60: an in-between frame instead
    }
    extern u64 g_dbg_frames;
    icons_frame_start(g_dbg_frames + 1);
}

// ---------------------------------------------------------------- command helpers
static inline s32 sx11(u32 v) { return ((s32)(v << 21)) >> 21; }

static void set_texpage(u32 v) {
    // polygon texpage attribute (bits 0-8 and 11) or full E1 word
    s_e1 = (s_e1 & ~0x9FFu) | (v & 0x9FF);
    st.texpage = (u16)(s_e1 & 0x3FFF);
}


static void draw_polygon() {
    u32 cmd = s_cmd[0] >> 24;
    bool gouraud = cmd & 0x10, quad = cmd & 0x08, tex = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
    int n = quad ? 4 : 3;
    Vertex v[4];
    int i = 1;
    u32 col0 = s_cmd[0] & 0xFFFFFF;
    u32 uvw[4] = {0, 0, 0, 0};
    for (int k = 0; k < n; k++) {
        if (k > 0 && gouraud) col0 = s_cmd[i++] & 0xFFFFFF;
        u32 xy = s_cmd[i++];
        v[k].x = sx11(xy) + s_offx;
        v[k].y = sx11(xy >> 16) + s_offy;
        v[k].r = (u8)col0; v[k].g = (u8)(col0 >> 8); v[k].b = (u8)(col0 >> 16);
        v[k].u = v[k].v = 0;
        if (tex) { uvw[k] = s_cmd[i++]; v[k].u = (u8)uvw[k]; v[k].v = (u8)(uvw[k] >> 8); }
        if (!gouraud) col0 = s_cmd[0] & 0xFFFFFF;
    }
    if (tex) {
        st.clut = (u16)(uvw[0] >> 16);
        set_texpage(uvw[1] >> 16);
    }
    int flags = (tex ? PF_TEXTURED : 0) | (semi ? PF_SEMI : 0) | (raw && tex ? PF_RAW : 0) | (gouraud ? PF_GOURAUD : 0);
    if (tex && (s_e1 & 0x800) && s_tex_disable_allowed) flags &= ~PF_TEXTURED;
    GpuDrawState ds = st;
    ds.dither = (s_e1 & 0x200) && ((flags & PF_GOURAUD) || ((flags & PF_TEXTURED) && !(flags & PF_RAW)));
    auto big = [](const Vertex& a, const Vertex& b, const Vertex& c) {
        s32 minx = std::min(a.x, std::min(b.x, c.x)), maxx = std::max(a.x, std::max(b.x, c.x));
        s32 miny = std::min(a.y, std::min(b.y, c.y)), maxy = std::max(a.y, std::max(b.y, c.y));
        return (maxx - minx) >= 1024 || (maxy - miny) >= 512;
    };
    if (tex) {
        int u0 = 255, v0 = 255, u1 = 0, v1 = 0, x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
        for (int k = 0; k < n; k++) {
            u0 = std::min<int>(u0, v[k].u); u1 = std::max<int>(u1, v[k].u); v0 = std::min<int>(v0, v[k].v); v1 = std::max<int>(v1, v[k].v);
            x0 = std::min(x0, v[k].x); x1 = std::max(x1, v[k].x); y0 = std::min(y0, v[k].y); y1 = std::max(y1, v[k].y);
        }
        icons_trace(quad ? "quad" : "tri", x0, y0, x1 - x0, y1 - y0, u0, v0, u1 + 1, v1 + 1, flags, ds);
        if (quad) { icons_apply(ds, u0, v0, u1 - u0, v1 - v0); if (ds.icon < 0) icons_apply(ds, u0, v0, u1 - u0 + 1, v1 - v0 + 1); }
    }
    if (g_interp_on) interp_rec_poly(v, n, flags, ds);
    if (!big(v[0], v[1], v[2])) g_renderer->triangle(v, flags, ds);
    if (quad) {
        Vertex t[3] = {v[1], v[2], v[3]};
        if (!big(t[0], t[1], t[2])) g_renderer->triangle(t, flags, ds);
    }
}

static void draw_rect() {
    u32 cmd = s_cmd[0] >> 24;
    bool tex = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
    int size = (cmd >> 3) & 3;
    int i = 1;
    u32 xy = s_cmd[i++];
    s32 x = sx11(xy) + s_offx, y = sx11(xy >> 16) + s_offy;
    u8 u = 0, vv = 0;
    if (tex) { u32 uv = s_cmd[i++]; u = (u8)uv; vv = (u8)(uv >> 8); st.clut = (u16)(uv >> 16); }
    s32 w, h;
    switch (size) {
    case 0: { u32 wh = s_cmd[i++]; w = wh & 0x3FF; h = (wh >> 16) & 0x1FF; break; }
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
    }
    if (w == 0 || h == 0) return;
    u32 col = s_cmd[0] & 0xFFFFFF;
    int flags = (tex ? PF_TEXTURED : 0) | (semi ? PF_SEMI : 0) | (raw && tex ? PF_RAW : 0);
    GpuDrawState ds = st;
    ds.dither = false;
    if (tex && !(s_e1 & 0x3000)) {
        icons_apply(ds, u, vv, w, h);   // button prompts follow the input device
        hd_text_apply(ds, u, vv, w, h); // letters drawn from their HD reconstruction
    }
    if (!tex && gpu_tracing()) plog("  rect color %06X\n", col);
    icons_trace("rect", x, y, w, h, u, vv, u + w, vv + h, flags, ds);
    if (g_interp_on) interp_rec_rect(x, y, w, h, col, u, vv, flags, ds, (s_e1 >> 12) & 1, (s_e1 >> 13) & 1);
    g_renderer->rect(x, y, w, h, (u8)col, (u8)(col >> 8), (u8)(col >> 16), u, vv, flags, ds, (s_e1 >> 12) & 1, (s_e1 >> 13) & 1);
}

static void draw_line_seg(u32 c0, u32 xy0, u32 c1, u32 xy1, bool semi, bool gouraud) {
    Vertex a, b;
    a.x = sx11(xy0) + s_offx; a.y = sx11(xy0 >> 16) + s_offy;
    b.x = sx11(xy1) + s_offx; b.y = sx11(xy1 >> 16) + s_offy;
    a.r = (u8)c0; a.g = (u8)(c0 >> 8); a.b = (u8)(c0 >> 16);
    if (!gouraud) c1 = c0;
    b.r = (u8)c1; b.g = (u8)(c1 >> 8); b.b = (u8)(c1 >> 16);
    a.u = a.v = b.u = b.v = 0;
    if (std::abs(a.x - b.x) >= 1024 || std::abs(a.y - b.y) >= 512) return;
    GpuDrawState ds = st;
    ds.dither = (s_e1 & 0x200) && gouraud;
    int flags = (semi ? PF_SEMI : 0) | (gouraud ? PF_GOURAUD : 0);
    if (g_interp_on) interp_rec_line(a, b, flags, ds);
    g_renderer->line(a, b, flags, ds);
}

static int cmd_length(u32 w) {
    u32 c = w >> 24;
    if (c >= 0x20 && c < 0x40) {
        bool g = c & 0x10, q = c & 0x08, t = c & 0x04;
        int n = q ? 4 : 3;
        return 1 + n * (1 + (t ? 1 : 0)) + (g ? n - 1 : 0);
    }
    if (c >= 0x40 && c < 0x60) {
        if (c & 0x08) return -1; // polyline, variable
        return (c & 0x10) ? 4 : 3;
    }
    if (c >= 0x60 && c < 0x80) {
        int len = 2;
        if (c & 0x04) len++;
        if (((c >> 3) & 3) == 0) len++;
        return len;
    }
    if (c == 0x02) return 3;
    if (c >= 0x80 && c < 0xA0) return 4;
    if (c >= 0xA0 && c < 0xE0) return 3;
    return 1;
}

static void exec_command() {
    u32 w = s_cmd[0];
    u32 c = w >> 24;
    if (c >= 0x20 && c < 0x40) { draw_polygon(); return; }
    if (c >= 0x60 && c < 0x80) { draw_rect(); return; }
    if (c >= 0x40 && c < 0x60) {
        bool g = c & 0x10, semi = c & 0x02;
        if (g) draw_line_seg(s_cmd[0], s_cmd[1], s_cmd[2], s_cmd[3], semi, true);
        else draw_line_seg(s_cmd[0], s_cmd[1], s_cmd[0], s_cmd[2], semi, false);
        return;
    }
    switch (c) {
    case 0x00: case 0x01: break;
    case 0x02: {
        u32 xy = s_cmd[1], wh = s_cmd[2];
        int x = xy & 0x3F0, y = (xy >> 16) & 0x1FF;
        int fw = ((wh & 0x3FF) + 0xF) & ~0xF, fh = (wh >> 16) & 0x1FF;
        if (gpu_tracing()) plog("  fill %d,%d %dx%d c=%06X\n", x, y, fw, fh, w & 0xFFFFFF);
        if (g_interp_on) interp_rec_fill(x, y, fw, fh, w & 0xFFFFFF);
        g_renderer->fill(x, y, fw, fh, w & 0xFFFFFF);
        break;
    }
    case 0x1F: s_irq = true; irq_raise(IRQ_GPU); break;
    case 0xE1: s_e1 = w & 0x3FFF; st.texpage = (u16)s_e1; break;
    case 0xE2: st.tex_mask_x = w & 0x1F; st.tex_mask_y = (w >> 5) & 0x1F; st.tex_off_x = (w >> 10) & 0x1F; st.tex_off_y = (w >> 15) & 0x1F; break;
    case 0xE3: st.clip_x1 = w & 0x3FF; st.clip_y1 = (w >> 10) & 0x1FF; break;
    case 0xE4: st.clip_x2 = w & 0x3FF; st.clip_y2 = (w >> 10) & 0x1FF; break;
    case 0xE5: s_offx = (s16)sx11(w); s_offy = (s16)sx11(w >> 11); break;
    case 0xE6:
        st.set_mask = w & 1; st.check_mask = (w >> 1) & 1;
        if (gpu_tracing()) plog("  e6 %X\n", w & 3);
        break;
    default:
        if (c >= 0x80 && c < 0xA0) {
            u32 s = s_cmd[1], d = s_cmd[2], wh = s_cmd[3];
            int cw = ((wh & 0x3FF) - 1) & 0x3FF, ch = (((wh >> 16) & 0x1FF) - 1) & 0x1FF;
            if (gpu_tracing()) plog("  copy %d,%d -> %d,%d %dx%d\n", s & 0x3FF, (s >> 16) & 0x1FF, d & 0x3FF, (d >> 16) & 0x1FF, cw + 1, ch + 1);
            g_renderer->vram_copy(s & 0x3FF, (s >> 16) & 0x1FF, d & 0x3FF, (d >> 16) & 0x1FF, cw + 1, ch + 1, st);
            icons_vram_changed();
            hd_text_vram_changed();
        } else if (c >= 0xA0 && c < 0xC0) {
            u32 xy = s_cmd[1], wh = s_cmd[2];
            s_up_x = xy & 0x3FF; s_up_y = (xy >> 16) & 0x1FF;
            s_up_w = (((wh & 0x3FF) - 1) & 0x3FF) + 1;
            s_up_h = ((((wh >> 16) & 0x1FF) - 1) & 0x1FF) + 1;
            s_up_total = s_up_w * s_up_h;
            s_up_n = 0;
            s_up_buf.resize(s_up_total + 1);
            s_upload = true;
        } else if (c >= 0xC0 && c < 0xE0) {
            u32 xy = s_cmd[1], wh = s_cmd[2];
            s_dn_x = xy & 0x3FF; s_dn_y = (xy >> 16) & 0x1FF;
            s_dn_w = (((wh & 0x3FF) - 1) & 0x3FF) + 1;
            s_dn_h = ((((wh >> 16) & 0x1FF) - 1) & 0x1FF) + 1;
            s_dn_total = s_dn_w * s_dn_h;
            s_dn_i = 0;
            if (gpu_tracing()) plog("  read %d,%d %dx%d\n", s_dn_x, s_dn_y, s_dn_w, s_dn_h);
            g_renderer->vram_read(s_dn_x, s_dn_y, s_dn_w, s_dn_h);
            s_download = true;
        }
        break;
    }
}

static void finish_upload() {
    s_upload = false;
    if (gpu_tracing()) plog("  write %d,%d %dx%d\n", s_up_x, s_up_y, s_up_w, s_up_h);
    g_renderer->vram_write(s_up_x, s_up_y, s_up_w, s_up_h, s_up_buf.data(), st);
    icons_vram_changed();
    hd_text_vram_changed();
}

void gpu_gp0(u32 w) {
    if (s_upload) {
        for (int k = 0; k < 2 && s_up_n < s_up_total; k++) s_up_buf[s_up_n++] = (u16)(w >> (16 * k));
        if (s_up_n >= s_up_total) finish_upload();
        return;
    }
    if (s_cmd_len == 0) {
        s_cmd[0] = w;
        s_cmd_len = 1;
        s_cmd_need = cmd_length(w);
        s_polyline = s_cmd_need < 0;
        if (!s_polyline && s_cmd_need == 1) { exec_command(); s_cmd_len = 0; }
        return;
    }
    if (s_polyline) {
        u32 c = s_cmd[0] >> 24;
        bool g = c & 0x10;
        // terminator is accepted where a new vertex starts (after the first segment)
        int per = g ? 2 : 1;
        bool at_vertex = s_cmd_len >= (g ? 4 : 3) && ((s_cmd_len - 1) % per) == 0;
        if (at_vertex && (w & 0xF000F000) == 0x50005000) { s_cmd_len = 0; return; }
        s_cmd[s_cmd_len++] = w;
        // draw the newest segment when it is complete
        if (g) {
            if (s_cmd_len >= 4 && (s_cmd_len % 2) == 0) {
                int n = s_cmd_len;
                draw_line_seg(s_cmd[n - 4], s_cmd[n - 3], s_cmd[n - 2], s_cmd[n - 1], c & 0x02, true);
                if (s_cmd_len >= 62) { s_cmd[0] = (s_cmd[n - 2] & 0xFFFFFF) | (s_cmd[0] & 0xFF000000); s_cmd[1] = s_cmd[n - 1]; s_cmd_len = 2; }
            }
        } else {
            if (s_cmd_len >= 3) {
                int n = s_cmd_len;
                draw_line_seg(s_cmd[0], s_cmd[n - 2], s_cmd[0], s_cmd[n - 1], c & 0x02, false);
                if (s_cmd_len >= 62) { s_cmd[1] = s_cmd[n - 1]; s_cmd_len = 2; }
            }
        }
        return;
    }
    s_cmd[s_cmd_len++] = w;
    if (s_cmd_len >= s_cmd_need) {
        exec_command();
        s_cmd_len = 0;
    }
}

void gpu_gp1(u32 v) {
    u32 c = (v >> 24) & 0x3F;
    switch (c) {
    case 0x00: gpu_reset(); break;
    case 0x01: s_cmd_len = 0; s_upload = false; break;
    case 0x02: s_irq = false; break;
    case 0x03: s_disp_off = v & 1; break;
    case 0x04: s_dma_dir = v & 3; break;
    case 0x05: {
        u32 ox = s_disp_x, oy = s_disp_y;
        s_disp_x = v & 0x3FE; s_disp_y = (v >> 10) & 0x1FF;
        if (s_disp_x != ox || s_disp_y != oy) interp_flip(gpu_display()); // a finished frame is shown
        if (gpu_tracing()) {
            plog("  display %d,%d\n", s_disp_x, s_disp_y);
            static int stacks = 0; // the first few flips: who presents (finds the game's main loop)
            if (stacks++ < 2) { void platform_log_stack_here(); platform_log_stack_here(); }
        }
        break;
    }
    case 0x06: s_hrange = v & 0xFFFFFF; break;
    case 0x07: s_vrange = v & 0xFFFFF; break;
    case 0x08: s_dmode = v & 0x7F; break;
    case 0x09: s_tex_disable_allowed = v & 1; break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (v & 7) {
        case 2: s_gpuread = st.tex_mask_x | (st.tex_mask_y << 5) | (st.tex_off_x << 10) | (st.tex_off_y << 15); break;
        case 3: s_gpuread = st.clip_x1 | (st.clip_y1 << 10); break;
        case 4: s_gpuread = st.clip_x2 | (st.clip_y2 << 10); break;
        case 5: s_gpuread = ((u32)s_offx & 0x7FF) | (((u32)s_offy & 0x7FF) << 11); break;
        case 7: s_gpuread = 2; break;
        }
        break;
    default: break;
    }
}

u32 gpu_read_data() {
    if (!s_download) return s_gpuread;
    u32 v = 0;
    for (int k = 0; k < 2; k++) {
        if (s_dn_i < s_dn_total) {
            int x = (s_dn_x + s_dn_i % s_dn_w) & 1023, y = (s_dn_y + s_dn_i / s_dn_w) & 511;
            v |= (u32)s_vram[y * 1024 + x] << (16 * k);
            s_dn_i++;
        }
    }
    if (s_dn_i >= s_dn_total) s_download = false;
    s_gpuread = v;
    return v;
}

void gpu_dma_write(const u32* words, int n) {
    for (int i = 0; i < n; i++) {
        if (s_upload) {
            // fast path for large uploads
            while (i < n && s_up_n + 2 <= s_up_total) {
                s_up_buf[s_up_n++] = (u16)words[i];
                s_up_buf[s_up_n++] = (u16)(words[i] >> 16);
                i++;
            }
            if (i >= n) { if (s_up_n >= s_up_total) finish_upload(); break; }
        }
        gpu_gp0(words[i]);
    }
}

u32 gpu_read(u32 off) {
    if (off == 0) return gpu_read_data();
    return gpustat();
}

void gpu_write(u32 off, u32 v) {
    if (off == 0) gpu_gp0(v);
    else gpu_gp1(v);
}
