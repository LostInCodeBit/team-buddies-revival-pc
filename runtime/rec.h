// rec.h - interface between the recompiled game code and the native runtime.
#pragma once
#include <cstdint>
#include <cstring>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

struct CPU {
    u32 r[32];
    u32 hi, lo;
    u32 pc;        // interpreter / diagnostics only
    u32 cop0[32];  // 12 = SR, 13 = CAUSE, 14 = EPC
    u32 escape;    // pending non-local return target (see recomp: "escapes"), 0 = none
};

extern CPU g_cpu;

// ------------------------------------------------------------------ timing
// g_cycles counts executed guest instructions; the scheduler converts to CPU cycles.
extern u64 g_cycles;
extern u64 g_next_event;
void rt_events(CPU* c);

#define CYCLES(n) (g_cycles += (n))
#define SAFEPOINT() do { if (g_cycles >= g_next_event) rt_events(c); } while (0)

// ------------------------------------------------------------------ memory
extern u8* g_ram;          // 2 MB main RAM
extern u8 g_scratch[1024]; // data cache used as scratchpad

u8 io_read8(u32 a);
u16 io_read16(u32 a);
u32 io_read32(u32 a);
void io_write8(u32 a, u8 v);
void io_write16(u32 a, u16 v);
void io_write32(u32 a, u32 v);

static inline u8 mem_read8(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x00800000) return g_ram[p & 0x1FFFFF];
    if ((p & 0xFFFFFC00) == 0x1F800000) return g_scratch[p & 0x3FF];
    return io_read8(a);
}
static inline u16 mem_read16(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    u16 v;
    if (p < 0x00800000) { memcpy(&v, g_ram + (p & 0x1FFFFE), 2); return v; }
    if ((p & 0xFFFFFC00) == 0x1F800000) { memcpy(&v, g_scratch + (p & 0x3FE), 2); return v; }
    return io_read16(a);
}
static inline u32 mem_read32(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    u32 v;
    if (p < 0x00800000) { memcpy(&v, g_ram + (p & 0x1FFFFC), 4); return v; }
    if ((p & 0xFFFFFC00) == 0x1F800000) { memcpy(&v, g_scratch + (p & 0x3FC), 4); return v; }
    return io_read32(a);
}
// debugging aid: writes to [g_watch_lo, g_watch_lo + g_watch_len) are reported (TB_WATCH)
extern u32 g_watch_lo, g_watch_len;
void watch_hit(u32 a, u32 v, int size);

static inline void mem_write8(u32 a, u8 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x00800000) {
        if ((p & 0x1FFFFF) - g_watch_lo < g_watch_len) watch_hit(a, v, 1);
        g_ram[p & 0x1FFFFF] = v;
        return;
    }
    if ((p & 0xFFFFFC00) == 0x1F800000) { g_scratch[p & 0x3FF] = v; return; }
    io_write8(a, v);
}
static inline void mem_write16(u32 a, u16 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x00800000) {
        if ((p & 0x1FFFFE) - g_watch_lo < g_watch_len) watch_hit(a, v, 2);
        memcpy(g_ram + (p & 0x1FFFFE), &v, 2);
        return;
    }
    if ((p & 0xFFFFFC00) == 0x1F800000) { memcpy(g_scratch + (p & 0x3FE), &v, 2); return; }
    io_write16(a, v);
}
static inline void mem_write32(u32 a, u32 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x00800000) {
        if ((p & 0x1FFFFC) - g_watch_lo < g_watch_len) watch_hit(a, v, 4);
        memcpy(g_ram + (p & 0x1FFFFC), &v, 4);
        return;
    }
    if ((p & 0xFFFFFC00) == 0x1F800000) { memcpy(g_scratch + (p & 0x3FC), &v, 4); return; }
    io_write32(a, v);
}

// ------------------------------------------------------------------ instruction helpers
static inline void op_mult(CPU* c, u32 a, u32 b) {
    s64 r = (s64)(s32)a * (s64)(s32)b;
    c->lo = (u32)r;
    c->hi = (u32)((u64)r >> 32);
}
static inline void op_multu(CPU* c, u32 a, u32 b) {
    u64 r = (u64)a * b;
    c->lo = (u32)r;
    c->hi = (u32)(r >> 32);
}
static inline void op_div(CPU* c, u32 a, u32 b) {
    s32 n = (s32)a, d = (s32)b;
    if (d == 0) { c->hi = a; c->lo = n >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (a == 0x80000000u && d == -1) { c->lo = 0x80000000u; c->hi = 0; }
    else { c->lo = (u32)(n / d); c->hi = (u32)(n % d); }
}
static inline void op_divu(CPU* c, u32 a, u32 b) {
    if (b == 0) { c->lo = 0xFFFFFFFFu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}
static inline u32 op_lwl(u32 r, u32 a) {
    u32 w = mem_read32(a & ~3u);
    switch (a & 3) {
    case 0: return (r & 0x00FFFFFF) | (w << 24);
    case 1: return (r & 0x0000FFFF) | (w << 16);
    case 2: return (r & 0x000000FF) | (w << 8);
    default: return w;
    }
}
static inline u32 op_lwr(u32 r, u32 a) {
    u32 w = mem_read32(a & ~3u);
    switch (a & 3) {
    case 0: return w;
    case 1: return (r & 0xFF000000) | (w >> 8);
    case 2: return (r & 0xFFFF0000) | (w >> 16);
    default: return (r & 0xFFFFFF00) | (w >> 24);
    }
}
static inline void op_swl(u32 a, u32 r) {
    u32 al = a & ~3u, m = mem_read32(al);
    switch (a & 3) {
    case 0: m = (m & 0xFFFFFF00) | (r >> 24); break;
    case 1: m = (m & 0xFFFF0000) | (r >> 16); break;
    case 2: m = (m & 0xFF000000) | (r >> 8); break;
    default: m = r; break;
    }
    mem_write32(al, m);
}
static inline void op_swr(u32 a, u32 r) {
    u32 al = a & ~3u, m = mem_read32(al);
    switch (a & 3) {
    case 0: m = r; break;
    case 1: m = (m & 0x000000FF) | (r << 8); break;
    case 2: m = (m & 0x0000FFFF) | (r << 16); break;
    default: m = (m & 0x00FFFFFF) | (r << 24); break;
    }
    mem_write32(al, m);
}

// ------------------------------------------------------------------ coprocessors
u32 cop0_read(CPU* c, u32 reg);
void cop0_write(CPU* c, u32 reg, u32 v);
void cop0_rfe(CPU* c);

u32 gte_read_data(u32 reg);
void gte_write_data(u32 reg, u32 v);
u32 gte_read_ctrl(u32 reg);
void gte_write_ctrl(u32 reg, u32 v);
void gte_command(u32 op);

// ------------------------------------------------------------------ control flow
void rt_call(CPU* c, u32 addr);                 // indirect / cross-overlay call
void rt_jump(CPU* c, u32 target, u32 ra_entry); // indirect jump that left the function
void rt_bios(CPU* c, u32 vector);               // A0/B0/C0 kernel call (function number in t1)
void rt_syscall(CPU* c, u32 pc);
void rt_break(CPU* c, u32 pc);
void rt_invalid(CPU* c, u32 pc);

// ------------------------------------------------------------------ tables emitted by recomp
typedef void (*RecFn)(CPU*);
struct RecFunc {
    u32 addr;
    int img;
    RecFn fn;
    u32 sig0, sig1;
};
struct RecCheck { u32 a, w; };
struct RecImage {
    const char* name;
    u32 base, end;
    int nchk;
    RecCheck chk[8];
};
extern const RecFunc g_rec_funcs[];
extern const int g_rec_func_count;
extern const RecImage g_rec_images[];
extern const int g_rec_image_count;
