// mem.cpp - physical memory map and I/O register dispatch.
#include "psx.h"
#include <cstdlib>

u8* g_ram;
u8 g_scratch[1024];

static u32 memctrl[9] = {0x1F000000, 0x1F802000, 0x0013243F, 0x00003022, 0x0013243F, 0x200931E1, 0x00020843, 0x00070777, 0x00031125};
static u32 ram_size = 0x00000B88;
static u32 cache_ctrl = 0;
static u8 s_exp2[0x80];

void mem_init() {
    g_ram = (u8*)calloc(1, 2 * 1024 * 1024);
    memset(g_scratch, 0, sizeof g_scratch);
}

// ---------------------------------------------------------------- dispatch
u32 hw_read(u32 p, int size) {
    if (p >= 0x1F801000 && p < 0x1F801024) return memctrl[(p - 0x1F801000) >> 2];
    if (p >= 0x1F801040 && p < 0x1F801050) return sio_read(p - 0x1F801040);
    if (p >= 0x1F801050 && p < 0x1F801060) return p == 0x1F801054 ? 0x5 : 0;
    if (p >= 0x1F801060 && p < 0x1F801064) return ram_size;
    if (p >= 0x1F801070 && p < 0x1F801074) return g_istat >> ((p & 3) * 8);
    if (p >= 0x1F801074 && p < 0x1F801078) return g_imask >> ((p & 3) * 8);
    if (p >= 0x1F801080 && p < 0x1F801100) return dma_read(p - 0x1F801080) >> ((p & 3) * 8);
    if (p >= 0x1F801100 && p < 0x1F801130) return timers_read(p - 0x1F801100);
    if (p >= 0x1F801800 && p < 0x1F801804) {
        if (size == 1) return cd_read(p & 3);
        u32 v = 0;
        for (int i = 0; i < size; i++) v |= (u32)cd_read((p + i) & 3) << (i * 8);
        return v;
    }
    if (p == 0x1F801810 || p == 0x1F801814) return gpu_read(p & 0xF);
    if (p == 0x1F801820 || p == 0x1F801824) return mdec_read(p & 0xF);
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        if (size == 4) return spu_read(p - 0x1F801C00) | ((u32)spu_read(p - 0x1F801C00 + 2) << 16);
        u16 v = spu_read((p - 0x1F801C00) & ~1u);
        return size == 1 ? (p & 1 ? v >> 8 : v & 0xFF) : v;
    }
    if (p >= 0x1F802000 && p < 0x1F802080) return s_exp2[p & 0x7F];
    log_once("hwr", "unhandled hw read %08X (size %d)\n", p, size);
    return 0;
}

void hw_write(u32 p, u32 v, int size) {
    if (p >= 0x1F801000 && p < 0x1F801024) { memctrl[(p - 0x1F801000) >> 2] = v; return; }
    if (p >= 0x1F801040 && p < 0x1F801050) { sio_write(p - 0x1F801040, v); return; }
    if (p >= 0x1F801050 && p < 0x1F801060) return;
    if (p >= 0x1F801060 && p < 0x1F801064) { ram_size = v; return; }
    if (p >= 0x1F801070 && p < 0x1F801074) {
        // writing 0 bits acknowledges interrupts
        u32 sh = (p & 3) * 8;
        u32 mask = size == 4 ? 0xFFFFFFFF : size == 2 ? 0xFFFF : 0xFF;
        g_istat &= (v << sh) | ~(mask << sh);
        irq_update();
        return;
    }
    if (p >= 0x1F801074 && p < 0x1F801078) {
        u32 sh = (p & 3) * 8;
        u32 mask = size == 4 ? 0xFFFFFFFF : size == 2 ? 0xFFFF : 0xFF;
        g_imask = ((g_imask & ~(mask << sh)) | ((v & mask) << sh)) & 0x7FF;
        irq_update();
        return;
    }
    if (p >= 0x1F801080 && p < 0x1F801100) {
        if (size == 4) dma_write(p - 0x1F801080, v);
        else {
            u32 al = (p - 0x1F801080) & ~3u, sh = (p & 3) * 8;
            u32 mask = size == 2 ? 0xFFFF : 0xFF;
            u32 old = dma_read(al);
            dma_write(al, (old & ~(mask << sh)) | ((v & mask) << sh));
        }
        return;
    }
    if (p >= 0x1F801100 && p < 0x1F801130) { timers_write(p - 0x1F801100, v); return; }
    if (p >= 0x1F801800 && p < 0x1F801804) {
        for (int i = 0; i < size && i < 4; i++) cd_write((p + i) & 3, (u8)(v >> (i * 8)));
        return;
    }
    if (p == 0x1F801810 || p == 0x1F801814) { gpu_write(p & 0xF, v); return; }
    if (p == 0x1F801820 || p == 0x1F801824) { mdec_write(p & 0xF, v); return; }
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        u32 off = p - 0x1F801C00;
        if (size == 4) { spu_write(off, (u16)v); spu_write(off + 2, (u16)(v >> 16)); }
        else spu_write(off & ~1u, (u16)v);
        return;
    }
    if (p >= 0x1F802000 && p < 0x1F802080) {
        s_exp2[p & 0x7F] = (u8)v;
        if (p == 0x1F802023 || p == 0x1F802080) plog("%c", (char)v); // debug UART
        return;
    }
    log_once("hww", "unhandled hw write %08X = %08X (size %d)\n", p, v, size);
}

static inline bool is_hw(u32 p) { return p >= 0x1F801000 && p < 0x1F803000; }

u8 io_read8(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (is_hw(p)) return (u8)hw_read(p, 1);
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFF;  // expansion 1: nothing there
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;     // no BIOS ROM in the port
    log_once("r8", "read8 from unmapped %08X\n", a);
    return 0;
}
u16 io_read16(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (is_hw(p)) return (u16)hw_read(p, 2);
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFFFF;
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;
    log_once("r16", "read16 from unmapped %08X\n", a);
    return 0;
}
u32 io_read32(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (is_hw(p)) return hw_read(p, 4);
    if (a == 0xFFFE0130) return cache_ctrl;
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFFFFFFFF;
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;
    static bool once = false;
    if (!once) { once = true; plog("read32 from unmapped %08X\n", a); watch_hit(a, 0, 4); }
    return 0;
}
void io_write8(u32 a, u8 v) {
    u32 p = a & 0x1FFFFFFF;
    if (is_hw(p)) { hw_write(p, v, 1); return; }
    log_once("w8", "write8 to unmapped %08X\n", a);
}
void io_write16(u32 a, u16 v) {
    u32 p = a & 0x1FFFFFFF;
    if (is_hw(p)) { hw_write(p, v, 2); return; }
    log_once("w16", "write16 to unmapped %08X\n", a);
}
void io_write32(u32 a, u32 v) {
    u32 p = a & 0x1FFFFFFF;
    if (is_hw(p)) { hw_write(p, v, 4); return; }
    if (a == 0xFFFE0130) { cache_ctrl = v; return; }
    static bool once = false;
    if (!once) { once = true; plog("write32 to unmapped %08X\n", a); watch_hit(a, v, 4); } // logs who did it
}
