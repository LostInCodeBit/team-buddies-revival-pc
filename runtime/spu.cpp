// spu.cpp - Sound Processing Unit: 24 ADPCM voices, ADSR, reverb, CD audio mixing.
#include "psx.h"
#include <algorithm>

static u8 s_ram[512 * 1024];
static u16 s_reg[0x200];          // raw register file (0x1F801C00 .. 0x1F801FFF) in halfwords

enum AdsrPhase { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

struct Voice {
    u32 addr;          // current block address
    u32 repeat;        // repeat address
    u32 counter;       // pitch counter (12 bit fraction)
    s16 hist[4];       // last decoded samples for interpolation
    s16 buf[28];
    int buf_pos;
    s32 prev1, prev2;  // ADPCM filter history
    u8 flags;          // flags of the current block
    AdsrPhase phase;
    s32 level;
    s32 wait;
    s16 out;           // last output sample (for pitch modulation)
    bool repeat_set;
};
static Voice s_v[24];
static u32 s_endx;
static u32 s_xfer_addr;
static u32 s_capture_pos;
static s32 s_noise = 1, s_noise_cnt;
static u32 s_rev_addr;
static s32 s_rev_l_in, s_rev_r_in, s_rev_out_l, s_rev_out_r;
static bool s_rev_phase;
static bool s_irq_flag;

#define R(off) s_reg[(off) >> 1]

static inline u16 spucnt() { return R(0x1AA); }

static void check_irq(u32 addr) {
    if (!(spucnt() & 0x40)) return;
    if ((addr & ~7u) == ((u32)R(0x1A4) << 3) && !s_irq_flag) {
        s_irq_flag = true;
        irq_raise(IRQ_SPU);
    }
}

void spu_init() {
    memset(s_ram, 0, sizeof s_ram);
    memset(s_reg, 0, sizeof s_reg);
    memset(s_v, 0, sizeof s_v);
    s_endx = 0;
}

// ---------------------------------------------------------------- ADPCM
static void decode_block(Voice& v) {
    static const int K0[5] = {0, 60, 115, 98, 122}, K1[5] = {0, 0, -52, -55, -60};
    u32 a = v.addr & 0x7FFF0;
    check_irq(a);
    const u8* b = s_ram + a;
    int shift = b[0] & 0x0F, filt = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    v.flags = b[1];
    if (v.flags & 4) { v.repeat = a; }
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        s32 s = (s16)(nib << 12) >> shift;
        s += (v.prev1 * K0[filt] + v.prev2 * K1[filt] + 32) >> 6;
        s = std::clamp(s, -32768, 32767);
        v.prev2 = v.prev1;
        v.prev1 = s;
        v.buf[i] = (s16)s;
    }
    v.buf_pos = 0;
}

static void next_block(Voice& v, int idx) {
    if (v.flags & 1) {
        s_endx |= 1u << idx;
        if (v.flags & 2) v.addr = v.repeat;
        else { v.phase = PH_RELEASE; v.level = 0; v.addr += 16; }
    } else {
        v.addr += 16;
    }
    v.addr &= 0x7FFF0;
    decode_block(v);
}

// ---------------------------------------------------------------- envelope
static void env_step(s32& level, s32& wait, bool exp, bool dec, int shift, int step) {
    if (wait > 0) { wait--; return; }
    int cycles = 1 << std::max(0, shift - 11);
    s32 st = step << std::max(0, 11 - shift);
    if (exp && !dec && level > 0x6000) cycles *= 4;
    if (exp && dec) st = (st * level) >> 15;
    wait = cycles - 1;
    level = std::clamp(level + st, 0, 0x7FFF);
}

static void adsr(Voice& v, int idx) {
    u16 a1 = R(idx * 16 + 8), a2 = R(idx * 16 + 10);
    switch (v.phase) {
    case PH_ATTACK:
        env_step(v.level, v.wait, a1 & 0x8000, false, (a1 >> 10) & 0x1F, 7 - ((a1 >> 8) & 3));
        if (v.level >= 0x7FFF) { v.phase = PH_DECAY; v.wait = 0; }
        break;
    case PH_DECAY: {
        s32 sus = ((a1 & 0xF) + 1) * 0x800;
        env_step(v.level, v.wait, true, true, (a1 >> 4) & 0xF, -8);
        if (v.level <= sus) { v.phase = PH_SUSTAIN; v.wait = 0; }
        break;
    }
    case PH_SUSTAIN: {
        bool dec = a2 & 0x4000;
        int s = (a2 >> 6) & 3;
        env_step(v.level, v.wait, a2 & 0x8000, dec, (a2 >> 8) & 0x1F, dec ? -8 + s : 7 - s);
        break;
    }
    case PH_RELEASE:
        env_step(v.level, v.wait, a2 & 0x20, true, a2 & 0x1F, -8);
        if (v.level <= 0) { v.phase = PH_OFF; v.level = 0; }
        break;
    default: break;
    }
    R(idx * 16 + 12) = (u16)v.level;
}

static void key_on(int i) {
    Voice& v = s_v[i];
    v.addr = ((u32)R(i * 16 + 6) << 3) & 0x7FFF0;
    if (!v.repeat_set) v.repeat = v.addr;
    v.counter = 0;
    v.prev1 = v.prev2 = 0;
    memset(v.hist, 0, sizeof v.hist);
    v.phase = PH_ATTACK;
    v.level = 0;
    v.wait = 0;
    s_endx &= ~(1u << i);
    decode_block(v);
}

static void key_off(int i) {
    if (s_v[i].phase != PH_OFF) { s_v[i].phase = PH_RELEASE; s_v[i].wait = 0; }
}

// ---------------------------------------------------------------- reverb
static inline s16 rv_read(u32 off) {
    u32 base = (u32)R(0x1A2) << 3;
    u32 a = s_rev_addr + off;
    u32 size = 0x80000 - base;
    if (size == 0) return 0;
    a = base + ((a - base) % size);
    return *(s16*)(s_ram + (a & 0x7FFFE));
}
static inline void rv_write(u32 off, s32 v) {
    u32 base = (u32)R(0x1A2) << 3;
    u32 a = s_rev_addr + off;
    u32 size = 0x80000 - base;
    if (size == 0) return;
    a = base + ((a - base) % size);
    *(s16*)(s_ram + (a & 0x7FFFE)) = (s16)std::clamp(v, -32768, 32767);
}
static inline s32 mul(s32 a, s32 b) { return (a * b) >> 15; }

static void reverb(s32 lin, s32 rin) {
    auto reg = [](u32 o) -> s32 { return (s16)R(o); };
    auto adr = [](u32 o) -> u32 { return (u32)R(o) << 3; };
    s32 vIIR = reg(0x1C4), vWALL = reg(0x1CE), vAPF1 = reg(0x1D0), vAPF2 = reg(0x1D2);
    s32 c1 = reg(0x1C6), c2 = reg(0x1C8), c3 = reg(0x1CA), c4 = reg(0x1CC);
    u32 dAPF1 = adr(0x1C0), dAPF2 = adr(0x1C2);
    u32 mLSAME = adr(0x1D4), mRSAME = adr(0x1D6), mLCOMB1 = adr(0x1D8), mRCOMB1 = adr(0x1DA), mLCOMB2 = adr(0x1DC), mRCOMB2 = adr(0x1DE);
    u32 dLSAME = adr(0x1E0), dRSAME = adr(0x1E2), mLDIFF = adr(0x1E4), mRDIFF = adr(0x1E6), mLCOMB3 = adr(0x1E8), mRCOMB3 = adr(0x1EA);
    u32 mLCOMB4 = adr(0x1EC), mRCOMB4 = adr(0x1EE), dLDIFF = adr(0x1F0), dRDIFF = adr(0x1F2);
    u32 mLAPF1 = adr(0x1F4), mRAPF1 = adr(0x1F6), mLAPF2 = adr(0x1F8), mRAPF2 = adr(0x1FA);
    s32 Lin = mul(reg(0x1FC), lin), Rin = mul(reg(0x1FE), rin);
    if (spucnt() & 0x80) {
        rv_write(mLSAME, mul(Lin + mul(rv_read(dLSAME), vWALL) - rv_read(mLSAME - 2), vIIR) + rv_read(mLSAME - 2));
        rv_write(mRSAME, mul(Rin + mul(rv_read(dRSAME), vWALL) - rv_read(mRSAME - 2), vIIR) + rv_read(mRSAME - 2));
        rv_write(mLDIFF, mul(Lin + mul(rv_read(dRDIFF), vWALL) - rv_read(mLDIFF - 2), vIIR) + rv_read(mLDIFF - 2));
        rv_write(mRDIFF, mul(Rin + mul(rv_read(dLDIFF), vWALL) - rv_read(mRDIFF - 2), vIIR) + rv_read(mRDIFF - 2));
    }
    s32 Lout = mul(c1, rv_read(mLCOMB1)) + mul(c2, rv_read(mLCOMB2)) + mul(c3, rv_read(mLCOMB3)) + mul(c4, rv_read(mLCOMB4));
    s32 Rout = mul(c1, rv_read(mRCOMB1)) + mul(c2, rv_read(mRCOMB2)) + mul(c3, rv_read(mRCOMB3)) + mul(c4, rv_read(mRCOMB4));
    Lout = Lout - mul(vAPF1, rv_read(mLAPF1 - dAPF1));
    if (spucnt() & 0x80) rv_write(mLAPF1, Lout);
    Lout = mul(Lout, vAPF1) + rv_read(mLAPF1 - dAPF1);
    Rout = Rout - mul(vAPF1, rv_read(mRAPF1 - dAPF1));
    if (spucnt() & 0x80) rv_write(mRAPF1, Rout);
    Rout = mul(Rout, vAPF1) + rv_read(mRAPF1 - dAPF1);
    Lout = Lout - mul(vAPF2, rv_read(mLAPF2 - dAPF2));
    if (spucnt() & 0x80) rv_write(mLAPF2, Lout);
    Lout = mul(Lout, vAPF2) + rv_read(mLAPF2 - dAPF2);
    Rout = Rout - mul(vAPF2, rv_read(mRAPF2 - dAPF2));
    if (spucnt() & 0x80) rv_write(mRAPF2, Rout);
    Rout = mul(Rout, vAPF2) + rv_read(mRAPF2 - dAPF2);
    s_rev_out_l = mul(std::clamp(Lout, -32768, 32767), (s16)R(0x184));
    s_rev_out_r = mul(std::clamp(Rout, -32768, 32767), (s16)R(0x186));
    u32 base = (u32)R(0x1A2) << 3;
    s_rev_addr = std::max(base, (s_rev_addr + 2) & 0x7FFFE);
    if (s_rev_addr < base) s_rev_addr = base;
}

// ---------------------------------------------------------------- mixing
static inline s32 vol(u16 v) {
    if (v & 0x8000) return 0x7FFF; // sweep mode: approximate as full volume
    return (s16)(v << 1);
}

void spu_tick(int samples) {
    static std::vector<s16> out;
    static std::vector<s16> cd;
    out.resize(samples * 2);
    cd.resize(samples * 2);
    cd_get_audio(cd.data(), samples);
    u16 cnt = spucnt();
    u32 pmon = R(0x190) | ((u32)R(0x192) << 16);
    u32 non = R(0x194) | ((u32)R(0x196) << 16);
    u32 eon = R(0x198) | ((u32)R(0x19A) << 16);
    for (int n = 0; n < samples; n++) {
        // noise generator
        int nshift = (cnt >> 10) & 0xF, nstep = ((cnt >> 8) & 3) + 4;
        s_noise_cnt += nstep << (16 - std::min(nshift, 15));
        if (s_noise_cnt >= 0x210000) {
            s_noise_cnt -= 0x210000;
            int parity = ((s_noise >> 15) ^ (s_noise >> 12) ^ (s_noise >> 11) ^ (s_noise >> 10) ^ 1) & 1;
            s_noise = (s_noise << 1) | parity;
        }
        s32 l = 0, r = 0, rl = 0, rr = 0;
        for (int i = 0; i < 24; i++) {
            Voice& v = s_v[i];
            if (v.phase == PH_OFF) { v.out = 0; continue; }
            // interpolated sample
            s32 frac = v.counter & 0xFFF;
            s32 s = v.hist[3] + (((v.buf[v.buf_pos] - v.hist[3]) * frac) >> 12);
            if (non & (1u << i)) s = (s16)s_noise;
            adsr(v, i);
            s32 smp = (s * v.level) >> 15;
            v.out = (s16)smp;
            s32 vl = (smp * vol(R(i * 16))) >> 15, vr = (smp * vol(R(i * 16 + 2))) >> 15;
            l += vl;
            r += vr;
            if (eon & (1u << i)) { rl += vl; rr += vr; }
            if (i == 1) *(s16*)(s_ram + 0x800 + s_capture_pos) = (s16)smp;
            if (i == 3) *(s16*)(s_ram + 0xC00 + s_capture_pos) = (s16)smp;
            // advance
            u32 step = R(i * 16 + 4);
            if (i > 0 && (pmon & (1u << i))) {
                s32 f = s_v[i - 1].out + 0x8000;
                step = (u32)(((s32)(s16)step * f) >> 15) & 0xFFFF;
            }
            if (step > 0x3FFF) step = 0x3FFF;
            v.counter += step;
            while (v.counter >= 0x1000) {
                v.counter -= 0x1000;
                v.hist[0] = v.hist[1]; v.hist[1] = v.hist[2]; v.hist[2] = v.hist[3];
                v.hist[3] = v.buf[v.buf_pos];
                if (++v.buf_pos >= 28) { next_block(v, i); if (v.phase == PH_OFF) break; }
            }
        }
        // CD audio input
        s32 cl = cd[n * 2], cr = cd[n * 2 + 1];
        *(s16*)(s_ram + 0x000 + s_capture_pos) = (s16)cl;
        *(s16*)(s_ram + 0x400 + s_capture_pos) = (s16)cr;
        s_capture_pos = (s_capture_pos + 2) & 0x3FF;
        if (cnt & 1) {
            s32 xl = (cl * (s16)R(0x1B0)) >> 15, xr = (cr * (s16)R(0x1B2)) >> 15;
            l += xl;
            r += xr;
            if (cnt & 4) { rl += xl; rr += xr; }
        }
        // reverb at 22.05 kHz
        s_rev_phase = !s_rev_phase;
        if (s_rev_phase) reverb(std::clamp(rl, -32768, 32767), std::clamp(rr, -32768, 32767));
        l += s_rev_out_l;
        r += s_rev_out_r;
        l = (l * vol(R(0x180))) >> 15;
        r = (r * vol(R(0x182))) >> 15;
        if (!(cnt & 0x4000) || !(cnt & 0x8000)) l = r = 0;
        l = l * g_settings.volume / 100;
        r = r * g_settings.volume / 100;
        out[n * 2] = (s16)std::clamp(l, -32768, 32767);
        out[n * 2 + 1] = (s16)std::clamp(r, -32768, 32767);
    }
    platform_audio_push(out.data(), samples);
}

// ---------------------------------------------------------------- registers
u16 spu_read(u32 off) {
    off &= 0x3FE;
    if (off < 0x180 && (off & 0xF) == 0xC) return (u16)s_v[off >> 4].level;
    switch (off) {
    case 0x19C: return (u16)s_endx;
    case 0x19E: return (u16)(s_endx >> 16);
    case 0x1A6: return (u16)(s_xfer_addr >> 3);
    case 0x1AE: {
        u16 v = spucnt() & 0x3F;
        if (s_irq_flag) v |= 0x40;
        if (s_capture_pos >= 0x200) v |= 0x800;
        return v;
    }
    case 0x1B8: case 0x1BA: return R(off - 0x38);
    }
    if (off >= 0x200 && off < 0x260) return (u16)s_v[(off - 0x200) >> 2].out;
    return R(off);
}

void spu_write(u32 off, u16 v) {
    off &= 0x3FE;
    R(off) = v;
    if (off < 0x180) {
        int i = off >> 4;
        if ((off & 0xF) == 0xE) { s_v[i].repeat = ((u32)v << 3) & 0x7FFF0; s_v[i].repeat_set = true; }
        if ((off & 0xF) == 0x6) s_v[i].repeat_set = false;
        return;
    }
    switch (off) {
    case 0x188: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_on(i); break;
    case 0x18A: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_on(16 + i); break;
    case 0x18C: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_off(i); break;
    case 0x18E: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_off(16 + i); break;
    case 0x19C: case 0x19E: break;
    case 0x1A2: s_rev_addr = (u32)v << 3; break;
    case 0x1A6: s_xfer_addr = ((u32)v << 3) & 0x7FFFF; break;
    case 0x1A8:
        check_irq(s_xfer_addr);
        *(u16*)(s_ram + (s_xfer_addr & 0x7FFFE)) = v;
        s_xfer_addr = (s_xfer_addr + 2) & 0x7FFFF;
        break;
    case 0x1AA:
        if (!(v & 0x40)) s_irq_flag = false;
        break;
    }
}

void spu_dma_write(const u8* src, u32 bytes) {
    for (u32 i = 0; i < bytes; i += 2) {
        if ((i & 15) == 0) check_irq(s_xfer_addr);
        *(u16*)(s_ram + (s_xfer_addr & 0x7FFFE)) = (u16)(src[i] | (src[i + 1] << 8));
        s_xfer_addr = (s_xfer_addr + 2) & 0x7FFFF;
    }
}

void spu_dma_read(u8* dst, u32 bytes) {
    for (u32 i = 0; i < bytes; i += 2) {
        u16 v = *(u16*)(s_ram + (s_xfer_addr & 0x7FFFE));
        dst[i] = (u8)v;
        dst[i + 1] = (u8)(v >> 8);
        s_xfer_addr = (s_xfer_addr + 2) & 0x7FFFF;
    }
}
