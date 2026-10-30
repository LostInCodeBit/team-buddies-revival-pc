// gte.cpp - Geometry Transformation Engine (COP2), bit-accurate fixed point implementation.
#include "psx.h"
#include <algorithm>

static u32 D[32]; // data registers
static u32 C[32]; // control registers
static u32 FLAG;
static u8 s_unr[0x101];
bool g_gpu_widescreen_hack = false;
int g_gpu_widescreen_q16 = 49152; // horizontal squeeze of projected X: 3/4 = 16:9, smaller for wider windows

enum {
    F_MAC1P = 1u << 30, F_MAC2P = 1u << 29, F_MAC3P = 1u << 28,
    F_MAC1N = 1u << 27, F_MAC2N = 1u << 26, F_MAC3N = 1u << 25,
    F_IR1 = 1u << 24, F_IR2 = 1u << 23, F_IR3 = 1u << 22,
    F_R = 1u << 21, F_G = 1u << 20, F_B = 1u << 19,
    F_SZ = 1u << 18, F_DIV = 1u << 17, F_MAC0P = 1u << 16, F_MAC0N = 1u << 15,
    F_SX = 1u << 14, F_SY = 1u << 13, F_IR0 = 1u << 12,
};

void gte_init() {
    memset(D, 0, sizeof D);
    memset(C, 0, sizeof C);
    FLAG = 0;
    for (int i = 0; i < 0x101; i++) s_unr[i] = (u8)std::max(0, (0x40000 / (i + 0x100) + 1) / 2 - 0x101);
}

// ---------------------------------------------------------------- field access
static inline s16 lo16(u32 v) { return (s16)(v & 0xFFFF); }
static inline s16 hi16(u32 v) { return (s16)(v >> 16); }
static inline s16 M(int base, int k) { u32 w = C[base + k / 2]; return (k & 1) ? hi16(w) : lo16(w); }
static inline s16 VX(int n) { return lo16(D[n * 2]); }
static inline s16 VY(int n) { return hi16(D[n * 2]); }
static inline s16 VZ(int n) { return lo16(D[n * 2 + 1]); }
static inline s32 IR(int n) { return (s32)(s16)D[8 + n]; }
static inline void setIR(int n, s32 v) { D[8 + n] = (u32)(s32)(s16)v; }
static inline s32 MAC(int n) { return (s32)D[24 + n]; }
static inline u8 RGBC(int n) { return (u8)(D[6] >> (8 * n)); }

// ---------------------------------------------------------------- flag helpers
static inline s64 chkmac(int i, s64 v) {
    if (v > 0x7FFFFFFFFFFLL) FLAG |= (i == 1 ? F_MAC1P : i == 2 ? F_MAC2P : F_MAC3P);
    else if (v < -0x80000000000LL) FLAG |= (i == 1 ? F_MAC1N : i == 2 ? F_MAC2N : F_MAC3N);
    return (v << 20) >> 20; // sign-extend 44 bits
}
static inline s64 chkmac0(s64 v) {
    if (v > 0x7FFFFFFFLL) FLAG |= F_MAC0P;
    else if (v < -0x80000000LL) FLAG |= F_MAC0N;
    return v;
}
static inline s32 satIR(int i, s32 v, bool lm) {
    s32 lo = lm ? 0 : -0x8000;
    if (v < lo) { FLAG |= (i == 1 ? F_IR1 : i == 2 ? F_IR2 : F_IR3); return lo; }
    if (v > 0x7FFF) { FLAG |= (i == 1 ? F_IR1 : i == 2 ? F_IR2 : F_IR3); return 0x7FFF; }
    return v;
}
static inline void set_mac_ir(int i, s64 v, int shift, bool lm) {
    v = chkmac(i, v);
    s32 m = (s32)(v >> shift);
    D[24 + i] = (u32)m;
    setIR(i, satIR(i, m, lm));
}
static inline u8 satcol(u32 flag, s32 v) {
    if (v < 0) { FLAG |= flag; return 0; }
    if (v > 255) { FLAG |= flag; return 255; }
    return (u8)v;
}
static void push_rgb() {
    D[20] = D[21];
    D[21] = D[22];
    u32 r = satcol(F_R, MAC(1) >> 4), g = satcol(F_G, MAC(2) >> 4), b = satcol(F_B, MAC(3) >> 4);
    D[22] = r | (g << 8) | (b << 16) | (D[6] & 0xFF000000u);
}
static void push_sz(s32 z) {
    if (z < 0) { z = 0; FLAG |= F_SZ; }
    if (z > 0xFFFF) { z = 0xFFFF; FLAG |= F_SZ; }
    D[16] = D[17]; D[17] = D[18]; D[18] = D[19]; D[19] = (u32)z;
}
static void push_sxy(s32 x, s32 y) {
    if (x < -0x400) { x = -0x400; FLAG |= F_SX; }
    if (x > 0x3FF) { x = 0x3FF; FLAG |= F_SX; }
    if (y < -0x400) { y = -0x400; FLAG |= F_SY; }
    if (y > 0x3FF) { y = 0x3FF; FLAG |= F_SY; }
    D[12] = D[13]; D[13] = D[14];
    D[14] = ((u32)x & 0xFFFF) | ((u32)y << 16);
}

static u32 unr_divide(u32 h, u32 sz3) {
    if (h >= sz3 * 2) { FLAG |= F_DIV; return 0x1FFFF; }
    int z = 0;
    for (u32 t = sz3 & 0xFFFF; z < 16 && !(t & 0x8000); z++) t <<= 1;
    u64 n = (u64)h << z;
    u32 d = (sz3 << z) & 0xFFFF;
    u32 u = s_unr[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    u64 r = (n * d + 0x8000) >> 16;
    return r > 0x1FFFF ? 0x1FFFF : (u32)r;
}

// ---------------------------------------------------------------- building blocks
// MAC = (T*0x1000 + M*V) >> shift for matrix base mb, translation T (or none)
static void mat_vec(int mb, const s32* T, s32 vx, s32 vy, s32 vz, int shift, bool lm) {
    s32 v[3] = {vx, vy, vz};
    for (int i = 0; i < 3; i++) {
        s64 acc = T ? ((s64)T[i] << 12) : 0;
        acc = chkmac(i + 1, acc + (s64)M(mb, i * 3 + 0) * v[0]);
        acc = chkmac(i + 1, acc + (s64)M(mb, i * 3 + 1) * v[1]);
        acc = chkmac(i + 1, acc + (s64)M(mb, i * 3 + 2) * v[2]);
        set_mac_ir(i + 1, acc, shift, lm);
    }
}

static void interp_color(s64 m1, s64 m2, s64 m3, int shift, bool lm) {
    s64 in[3] = {m1, m2, m3};
    for (int i = 0; i < 3; i++) set_mac_ir(i + 1, ((s64)(s32)C[21 + i] << 12) - in[i], shift, false);
    for (int i = 0; i < 3; i++) set_mac_ir(i + 1, (s64)IR(i + 1) * IR(0) + in[i], shift, lm);
}

static void rtp(int n, int shift, bool lm, bool last) {
    s32 tr[3] = {(s32)C[5], (s32)C[6], (s32)C[7]};
    s32 v[3] = {VX(n), VY(n), VZ(n)};
    s64 r[3];
    for (int i = 0; i < 3; i++) {
        s64 acc = (s64)tr[i] << 12;
        acc = chkmac(i + 1, acc + (s64)M(0, i * 3 + 0) * v[0]);
        acc = chkmac(i + 1, acc + (s64)M(0, i * 3 + 1) * v[1]);
        acc = chkmac(i + 1, acc + (s64)M(0, i * 3 + 2) * v[2]);
        r[i] = acc;
        D[25 + i] = (u32)(s32)(acc >> shift);
    }
    setIR(1, satIR(1, MAC(1), lm));
    setIR(2, satIR(2, MAC(2), lm));
    if (shift == 0) {
        s64 t = r[2] >> 12;
        if (t < -0x8000 || t > 0x7FFF) FLAG |= F_IR3;
        s32 m = MAC(3), lo = lm ? 0 : -0x8000;
        setIR(3, m < lo ? lo : m > 0x7FFF ? 0x7FFF : m);
    } else {
        setIR(3, satIR(3, MAC(3), lm));
    }
    push_sz((s32)(r[2] >> 12));
    u32 q = unr_divide(C[26] & 0xFFFF, D[19] & 0xFFFF);
    s64 sx = (s64)q * IR(1);
    if (g_gpu_widescreen_hack) sx = sx * g_gpu_widescreen_q16 / 65536;
    sx = chkmac0(sx + (s32)C[24]);
    s64 sy = chkmac0((s64)q * IR(2) + (s32)C[25]);
    push_sxy((s32)(sx >> 16), (s32)(sy >> 16));
    if (last) {
        s64 dq = chkmac0((s64)q * (s16)C[27] + (s32)C[28]);
        D[24] = (u32)(s32)dq;
        s64 i0 = dq >> 12;
        if (i0 < 0) { i0 = 0; FLAG |= F_IR0; }
        if (i0 > 0x1000) { i0 = 0x1000; FLAG |= F_IR0; }
        D[8] = (u32)(s32)i0;
    }
}

// normal -> color: IR = LLM*V, IR = BK + LCM*IR
static void light(int n, int shift, bool lm) {
    mat_vec(8, nullptr, VX(n), VY(n), VZ(n), shift, lm);
    s32 bk[3] = {(s32)C[13], (s32)C[14], (s32)C[15]};
    mat_vec(16, bk, IR(1), IR(2), IR(3), shift, lm);
}

static void ncs(int n, int shift, bool lm) {
    light(n, shift, lm);
    push_rgb();
}
static void nccs(int n, int shift, bool lm) {
    light(n, shift, lm);
    for (int i = 0; i < 3; i++) set_mac_ir(i + 1, ((s64)RGBC(i) * IR(i + 1)) << 4, shift, lm);
    push_rgb();
}
static void ncds(int n, int shift, bool lm) {
    light(n, shift, lm);
    interp_color(((s64)RGBC(0) * IR(1)) << 4, ((s64)RGBC(1) * IR(2)) << 4, ((s64)RGBC(2) * IR(3)) << 4, shift, lm);
    push_rgb();
}
static void dpcs_rgb(u32 rgb, int shift, bool lm) {
    interp_color((s64)(rgb & 0xFF) << 16, (s64)((rgb >> 8) & 0xFF) << 16, (s64)((rgb >> 16) & 0xFF) << 16, shift, lm);
    push_rgb();
}

void gte_command(u32 op) {
    FLAG = 0;
    int shift = (op & (1 << 19)) ? 12 : 0;
    bool lm = (op & (1 << 10)) != 0;
    switch (op & 0x3F) {
    case 0x01: rtp(0, shift, lm, true); break;
    case 0x30: rtp(0, shift, lm, false); rtp(1, shift, lm, false); rtp(2, shift, lm, true); break;
    case 0x06: { // NCLIP
        s32 x0 = lo16(D[12]), y0 = hi16(D[12]), x1 = lo16(D[13]), y1 = hi16(D[13]), x2 = lo16(D[14]), y2 = hi16(D[14]);
        s64 v = (s64)x0 * y1 + (s64)x1 * y2 + (s64)x2 * y0 - (s64)x0 * y2 - (s64)x1 * y0 - (s64)x2 * y1;
        D[24] = (u32)(s32)chkmac0(v);
        break;
    }
    case 0x0C: { // OP
        s32 d1 = M(0, 0), d2 = M(0, 4), d3 = M(0, 8);
        s32 i1 = IR(1), i2 = IR(2), i3 = IR(3);
        set_mac_ir(1, (s64)d2 * i3 - (s64)d3 * i2, shift, lm);
        set_mac_ir(2, (s64)d3 * i1 - (s64)d1 * i3, shift, lm);
        set_mac_ir(3, (s64)d1 * i2 - (s64)d2 * i1, shift, lm);
        break;
    }
    case 0x10: dpcs_rgb(D[6], shift, lm); break;
    case 0x11: interp_color((s64)IR(1) << 12, (s64)IR(2) << 12, (s64)IR(3) << 12, shift, lm); push_rgb(); break;
    case 0x12: { // MVMVA
        int mx = (op >> 17) & 3, vv = (op >> 15) & 3, cv = (op >> 13) & 3;
        s32 vx, vy, vz;
        if (vv == 3) { vx = IR(1); vy = IR(2); vz = IR(3); }
        else { vx = VX(vv); vy = VY(vv); vz = VZ(vv); }
        s32 mtx[9];
        if (mx == 3) {
            s32 r = RGBC(0) << 4;
            s32 m[9] = {-r, r, IR(0), M(0, 2), M(0, 2), M(0, 2), M(0, 4), M(0, 4), M(0, 4)};
            memcpy(mtx, m, sizeof m);
        } else {
            int mb = mx * 8;
            for (int k = 0; k < 9; k++) mtx[k] = M(mb, k);
        }
        s32 t[3] = {0, 0, 0};
        if (cv == 0) { t[0] = (s32)C[5]; t[1] = (s32)C[6]; t[2] = (s32)C[7]; }
        else if (cv == 1) { t[0] = (s32)C[13]; t[1] = (s32)C[14]; t[2] = (s32)C[15]; }
        else if (cv == 2) { t[0] = (s32)C[21]; t[1] = (s32)C[22]; t[2] = (s32)C[23]; }
        for (int i = 0; i < 3; i++) {
            if (cv == 2) {
                // hardware bug: the far color translation only affects the flags
                s64 bug = chkmac(i + 1, ((s64)t[i] << 12) + (s64)mtx[i * 3] * vx);
                satIR(i + 1, (s32)(bug >> shift), false);
                s64 acc = chkmac(i + 1, (s64)mtx[i * 3 + 1] * vy);
                acc = chkmac(i + 1, acc + (s64)mtx[i * 3 + 2] * vz);
                set_mac_ir(i + 1, acc, shift, lm);
            } else {
                s64 acc = (s64)t[i] << 12;
                acc = chkmac(i + 1, acc + (s64)mtx[i * 3] * vx);
                acc = chkmac(i + 1, acc + (s64)mtx[i * 3 + 1] * vy);
                acc = chkmac(i + 1, acc + (s64)mtx[i * 3 + 2] * vz);
                set_mac_ir(i + 1, acc, shift, lm);
            }
        }
        break;
    }
    case 0x13: ncds(0, shift, lm); break;
    case 0x14: { // CDP
        s32 bk[3] = {(s32)C[13], (s32)C[14], (s32)C[15]};
        mat_vec(16, bk, IR(1), IR(2), IR(3), shift, lm);
        interp_color(((s64)RGBC(0) * IR(1)) << 4, ((s64)RGBC(1) * IR(2)) << 4, ((s64)RGBC(2) * IR(3)) << 4, shift, lm);
        push_rgb();
        break;
    }
    case 0x16: ncds(0, shift, lm); ncds(1, shift, lm); ncds(2, shift, lm); break;
    case 0x1B: nccs(0, shift, lm); break;
    case 0x1C: { // CC
        s32 bk[3] = {(s32)C[13], (s32)C[14], (s32)C[15]};
        mat_vec(16, bk, IR(1), IR(2), IR(3), shift, lm);
        for (int i = 0; i < 3; i++) set_mac_ir(i + 1, ((s64)RGBC(i) * IR(i + 1)) << 4, shift, lm);
        push_rgb();
        break;
    }
    case 0x1E: ncs(0, shift, lm); break;
    case 0x20: ncs(0, shift, lm); ncs(1, shift, lm); ncs(2, shift, lm); break;
    case 0x28: // SQR
        for (int i = 1; i <= 3; i++) set_mac_ir(i, (s64)IR(i) * IR(i), shift, lm);
        break;
    case 0x29: // DCPL
        interp_color(((s64)RGBC(0) * IR(1)) << 4, ((s64)RGBC(1) * IR(2)) << 4, ((s64)RGBC(2) * IR(3)) << 4, shift, lm);
        push_rgb();
        break;
    case 0x2A: for (int k = 0; k < 3; k++) dpcs_rgb(D[20], shift, lm); break; // DPCT
    case 0x2D: { // AVSZ3
        s64 v = (s64)(s16)C[29] * ((D[17] & 0xFFFF) + (D[18] & 0xFFFF) + (D[19] & 0xFFFF));
        D[24] = (u32)(s32)chkmac0(v);
        s64 o = v >> 12;
        if (o < 0) { o = 0; FLAG |= F_SZ; }
        if (o > 0xFFFF) { o = 0xFFFF; FLAG |= F_SZ; }
        D[7] = (u32)o;
        break;
    }
    case 0x2E: { // AVSZ4
        s64 v = (s64)(s16)C[30] * ((D[16] & 0xFFFF) + (D[17] & 0xFFFF) + (D[18] & 0xFFFF) + (D[19] & 0xFFFF));
        D[24] = (u32)(s32)chkmac0(v);
        s64 o = v >> 12;
        if (o < 0) { o = 0; FLAG |= F_SZ; }
        if (o > 0xFFFF) { o = 0xFFFF; FLAG |= F_SZ; }
        D[7] = (u32)o;
        break;
    }
    case 0x3D: // GPF
        for (int i = 1; i <= 3; i++) set_mac_ir(i, (s64)IR(i) * IR(0), shift, lm);
        push_rgb();
        break;
    case 0x3E: // GPL
        for (int i = 1; i <= 3; i++) set_mac_ir(i, ((s64)MAC(i) << shift) + (s64)IR(i) * IR(0), shift, lm);
        push_rgb();
        break;
    case 0x3F: nccs(0, shift, lm); nccs(1, shift, lm); nccs(2, shift, lm); break;
    default:
        log_once(("gte" + std::to_string(op & 0x3F)).c_str(), "unknown GTE command %02X\n", op & 0x3F);
        break;
    }
    if (FLAG & 0x7F87E000) FLAG |= 0x80000000u;
}

// ---------------------------------------------------------------- register transfers
static u32 orgb() {
    auto c5 = [](s32 v) -> u32 { v >>= 7; return v < 0 ? 0 : v > 0x1F ? 0x1F : (u32)v; };
    return c5(IR(1)) | (c5(IR(2)) << 5) | (c5(IR(3)) << 10);
}

u32 gte_read_data(u32 r) {
    switch (r & 31) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: return (u32)(s32)(s16)D[r];
    case 7: case 16: case 17: case 18: case 19: return D[r] & 0xFFFF;
    case 15: return D[14];
    case 28: case 29: return orgb();
    default: return D[r & 31];
    }
}

void gte_write_data(u32 r, u32 v) {
    switch (r & 31) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: D[r] = (u32)(s32)(s16)v; break;
    case 7: case 16: case 17: case 18: case 19: D[r] = v & 0xFFFF; break;
    case 15: D[12] = D[13]; D[13] = D[14]; D[14] = v; break;
    case 28:
        D[28] = v & 0x7FFF;
        setIR(1, (v & 0x1F) << 7);
        setIR(2, ((v >> 5) & 0x1F) << 7);
        setIR(3, ((v >> 10) & 0x1F) << 7);
        break;
    case 29: break;
    case 30: {
        D[30] = v;
        u32 x = (s32)v < 0 ? ~v : v;
        u32 n = 0;
        while (n < 32 && !(x & 0x80000000u)) { x <<= 1; n++; }
        D[31] = n;
        break;
    }
    case 31: break;
    default: D[r & 31] = v; break;
    }
}

u32 gte_read_ctrl(u32 r) {
    switch (r & 31) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: return (u32)(s32)(s16)C[r];
    case 31: return FLAG;
    default: return C[r & 31];
    }
}

void gte_write_ctrl(u32 r, u32 v) {
    switch (r & 31) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: C[r] = (u32)(s32)(s16)v; break;
    case 31:
        FLAG = v & 0x7FFFF000;
        if (FLAG & 0x7F87E000) FLAG |= 0x80000000u;
        break;
    default: C[r & 31] = v; break;
    }
}
