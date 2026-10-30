// mdec.cpp - motion decoder (FMV macroblock decompression).
#include "psx.h"
#include <algorithm>

void dma_mdec_ready();

static u8 s_qt[128];              // luma[64] + chroma[64]
static s16 s_scale[64];
static u32 s_cmd;
static u32 s_remaining;           // parameter words still expected
static std::vector<u32> s_params;
static std::vector<u32> s_out;
static size_t s_out_pos;
static u32 s_ctrl;
static bool s_busy;

static const u8 k_zigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
static u8 s_zagzig[64];

void mdec_init() {
    for (int i = 0; i < 64; i++) s_zagzig[k_zigzag[i]] = (u8)i;
    s_cmd = 0;
    s_remaining = 0;
    s_out.clear();
    s_out_pos = 0;
}

static inline s32 sx10(u32 v) { return ((s32)(v << 22)) >> 22; }

static void idct(s16* blk) {
    s64 temp[64];
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            s64 sum = 0;
            for (int u = 0; u < 8; u++) sum += (s32)blk[u * 8 + x] * (s32)s_scale[u * 8 + y];
            temp[x + y * 8] = sum;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            s64 sum = 0;
            for (int u = 0; u < 8; u++) sum += temp[u + y * 8] * (s32)s_scale[u * 8 + x];
            s32 v = (s32)((sum >> 32) + ((sum >> 31) & 1));
            v = ((s32)(v << 23)) >> 23; // sign extend 9 bits
            blk[x + y * 8] = (s16)std::clamp(v, -128, 127);
        }
}

// returns false when the input ran out
static bool decode_block(const u16*& p, const u16* end, s16* blk, const u8* qt) {
    for (int i = 0; i < 64; i++) blk[i] = 0;
    u16 n;
    do {
        if (p >= end) return false;
        n = *p++;
    } while (n == 0xFE00);
    int k = 0;
    s32 q = (n >> 10) & 0x3F;
    s32 val = q == 0 ? sx10(n) * 2 : sx10(n) * qt[0];
    for (;;) {
        val = std::clamp(val, -0x400, 0x3FF);
        if (q > 0) blk[s_zagzig[k]] = (s16)val;
        else blk[k] = (s16)val;
        if (p >= end) break;
        n = *p++;
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = q == 0 ? sx10(n) * 2 : (sx10(n) * qt[k] * q + 4) / 8;
    }
    idct(blk);
    return true;
}

static void output_rgb(const s16* cr, const s16* cb, const s16* y4[4]) {
    u32 depth = (s_cmd >> 27) & 3;
    bool sign = (s_cmd >> 26) & 1, b15 = (s_cmd >> 25) & 1;
    u32 px[256];
    for (int b = 0; b < 4; b++) {
        int xx = (b & 1) * 8, yy = (b >> 1) * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int ci = ((x + xx) >> 1) + ((y + yy) >> 1) * 8;
                s32 R = cr[ci], B = cb[ci];
                s32 G = (s32)(-0.3437 * B - 0.7143 * R);
                R = (s32)(1.402 * R);
                B = (s32)(1.772 * B);
                s32 Y = y4[b][x + y * 8];
                s32 r = std::clamp(Y + R, -128, 127), g = std::clamp(Y + G, -128, 127), bl = std::clamp(Y + B, -128, 127);
                if (!sign) { r ^= 0x80; g ^= 0x80; bl ^= 0x80; }
                px[(x + xx) + (y + yy) * 16] = (u32)(r & 0xFF) | ((u32)(g & 0xFF) << 8) | ((u32)(bl & 0xFF) << 16);
            }
    }
    if (depth == 2) {
        // 24-bit: pack 3 bytes per pixel
        u8 bytes[768];
        for (int i = 0; i < 256; i++) { bytes[i * 3] = (u8)px[i]; bytes[i * 3 + 1] = (u8)(px[i] >> 8); bytes[i * 3 + 2] = (u8)(px[i] >> 16); }
        for (int i = 0; i < 192; i++) s_out.push_back(bytes[i * 4] | (bytes[i * 4 + 1] << 8) | (bytes[i * 4 + 2] << 16) | ((u32)bytes[i * 4 + 3] << 24));
    } else {
        for (int i = 0; i < 256; i += 2) {
            u32 w = 0;
            for (int k = 0; k < 2; k++) {
                u32 c = px[i + k];
                u16 h = (u16)(((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) | ((((c >> 16) & 0xFF) >> 3) << 10));
                if (b15) h |= 0x8000;
                w |= (u32)h << (16 * k);
            }
            s_out.push_back(w);
        }
    }
}

static void output_mono(const s16* y) {
    u32 depth = (s_cmd >> 27) & 3;
    bool sign = (s_cmd >> 26) & 1;
    u8 v[64];
    for (int i = 0; i < 64; i++) { s32 a = std::clamp((s32)y[i], -128, 127); v[i] = (u8)(sign ? a : a ^ 0x80); }
    if (depth == 1) {
        for (int i = 0; i < 64; i += 4) s_out.push_back(v[i] | (v[i + 1] << 8) | (v[i + 2] << 16) | ((u32)v[i + 3] << 24));
    } else {
        for (int i = 0; i < 64; i += 8) {
            u32 w = 0;
            for (int k = 0; k < 8; k++) w |= (u32)(v[i + k] >> 4) << (4 * k);
            s_out.push_back(w);
        }
    }
}

static void execute() {
    u32 op = s_cmd >> 29;
    if (op == 1) {
        // compact the output fifo
        if (s_out_pos >= s_out.size()) { s_out.clear(); s_out_pos = 0; }
        std::vector<u16> hw(s_params.size() * 2);
        for (size_t i = 0; i < s_params.size(); i++) { hw[i * 2] = (u16)s_params[i]; hw[i * 2 + 1] = (u16)(s_params[i] >> 16); }
        const u16* p = hw.data();
        const u16* end = p + hw.size();
        u32 depth = (s_cmd >> 27) & 3;
        s16 cr[64], cb[64], y[4][64];
        while (p < end) {
            if (depth >= 2) {
                if (!decode_block(p, end, cr, s_qt + 64)) break;
                if (!decode_block(p, end, cb, s_qt + 64)) break;
                bool ok = true;
                for (int b = 0; b < 4 && ok; b++) ok = decode_block(p, end, y[b], s_qt);
                if (!ok) break;
                const s16* yy[4] = {y[0], y[1], y[2], y[3]};
                output_rgb(cr, cb, yy);
            } else {
                if (!decode_block(p, end, y[0], s_qt)) break;
                output_mono(y[0]);
            }
        }
    } else if (op == 2) {
        int n = (s_cmd & 1) ? 128 : 64;
        for (int i = 0; i < n; i++) s_qt[i] = (u8)(s_params[i / 4] >> ((i & 3) * 8));
    } else if (op == 3) {
        for (int i = 0; i < 64; i++) s_scale[i] = (s16)(s_params[i / 2] >> ((i & 1) * 16));
    }
    s_params.clear();
    s_busy = false;
    dma_mdec_ready();
}

static void feed(u32 w) {
    if (s_remaining == 0) {
        s_cmd = w;
        u32 op = w >> 29;
        if (op == 1) s_remaining = w & 0xFFFF;
        else if (op == 2) s_remaining = (w & 1) ? 32 : 16;
        else if (op == 3) s_remaining = 32;
        else s_remaining = 0;
        s_params.clear();
        s_busy = s_remaining > 0;
        if (!s_remaining && op >= 1 && op <= 3) execute();
        return;
    }
    s_params.push_back(w);
    if (--s_remaining == 0) execute();
}

void mdec_dma_in(const u32* words, int n) { for (int i = 0; i < n; i++) feed(words[i]); }

void mdec_dma_out(u32* words, int n) {
    for (int i = 0; i < n; i++) words[i] = s_out_pos < s_out.size() ? s_out[s_out_pos++] : 0;
    if (s_out_pos >= s_out.size()) { s_out.clear(); s_out_pos = 0; }
}

bool mdec_out_ready() { return s_out_pos < s_out.size(); }

u32 mdec_read(u32 off) {
    if (off == 0) {
        u32 v = s_out_pos < s_out.size() ? s_out[s_out_pos++] : 0;
        return v;
    }
    u32 st = 0;
    if (s_out_pos >= s_out.size()) st |= 1u << 31;
    if (s_busy) st |= 1u << 29;
    if (s_ctrl & (1u << 30)) st |= 1u << 28;
    if ((s_ctrl & (1u << 29)) && s_out_pos < s_out.size()) st |= 1u << 27;
    st |= ((s_cmd >> 27) & 3) << 25;
    st |= ((s_cmd >> 26) & 1) << 24;
    st |= ((s_cmd >> 25) & 1) << 23;
    st |= (s_remaining ? (s_remaining - 1) : 0xFFFF) & 0xFFFF;
    return st;
}

void mdec_write(u32 off, u32 v) {
    if (off == 0) { feed(v); return; }
    if (v & (1u << 31)) {
        s_remaining = 0;
        s_params.clear();
        s_out.clear();
        s_out_pos = 0;
        s_busy = false;
        s_cmd = 0;
    }
    s_ctrl = v;
}
