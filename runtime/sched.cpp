// sched.cpp - event scheduler, interrupt controller, root counters and DMA.
#include "psx.h"
#include <chrono>

// ---------------------------------------------------------------- scheduler
struct Ev {
    bool active;
    u64 when;      // in instruction ticks (g_cycles units)
    EventFn fn;
};
static Ev s_ev[EV_COUNT];

static inline double cpi() { return g_settings.cpu_cpi > 0.1 ? g_settings.cpu_cpi : 2.0; }

u64 cpu_now() { return (u64)((double)g_cycles * cpi()); }

static void recompute_next() {
    u64 n = g_cycles + 1000000;
    for (auto& e : s_ev) if (e.active && e.when < n) n = e.when;
    g_next_event = n;
    if (irq_take_now(&g_cpu)) g_next_event = 0;
}

void sched_init() {
    memset(s_ev, 0, sizeof s_ev);
    g_next_event = 0;
}

void sched_set(int id, u64 cyc, EventFn fn) {
    u64 ticks = (u64)((double)cyc / cpi());
    if (ticks < 1) ticks = 1;
    s_ev[id].active = true;
    s_ev[id].when = g_cycles + ticks;
    s_ev[id].fn = fn;
    if (s_ev[id].when < g_next_event) g_next_event = s_ev[id].when;
}

// periodic events: schedule relative to the previous deadline so they never drift
void sched_repeat(int id, u64 cyc, EventFn fn) {
    u64 ticks = (u64)((double)cyc / cpi());
    if (ticks < 1) ticks = 1;
    u64 base = s_ev[id].when ? s_ev[id].when : g_cycles;
    s_ev[id].active = true;
    s_ev[id].when = base + ticks;
    if (s_ev[id].when + ticks * 4 < g_cycles) s_ev[id].when = g_cycles + ticks; // fell far behind
    s_ev[id].fn = fn;
    if (s_ev[id].when < g_next_event) g_next_event = s_ev[id].when;
}

void sched_cancel(int id) { s_ev[id].active = false; }
bool sched_active(int id) { return s_ev[id].active; }
u64 sched_remaining(int id) {
    if (!s_ev[id].active || s_ev[id].when <= g_cycles) return 0;
    return (u64)((double)(s_ev[id].when - g_cycles) * cpi());
}

void sched_run() {
    for (int guard = 0; guard < 64; guard++) {
        int best = -1;
        for (int i = 0; i < EV_COUNT; i++)
            if (s_ev[i].active && s_ev[i].when <= g_cycles && (best < 0 || s_ev[i].when < s_ev[best].when)) best = i;
        if (best < 0) break;
        s_ev[best].active = false;
        s_ev[best].fn(best);
    }
    recompute_next();
}

// ---------------------------------------------------------------- interrupt controller
u32 g_istat, g_imask;

bool irq_take_now(const CPU* c) {
    return (g_istat & g_imask) != 0 && (c->cop0[12] & 0x401) == 0x401;
}

void irq_raise(int n) {
    g_istat |= 1u << n;
    irq_update();
}

void irq_update() {
    if (irq_take_now(&g_cpu)) g_next_event = 0;
}

// ---------------------------------------------------------------- vblank / frame
u64 g_dbg_frames, g_dbg_exceptions, g_dbg_bios;
u32 g_dbg_last_bios;
static u64 g_dbg_present_us;

static void ev_vblank(int id) {
    sched_repeat(id, CYCLES_PER_FRAME, ev_vblank);
    if (++g_dbg_frames % 60 == 0 && g_settings.log_level >= 2) {
        DisplayInfo d = gpu_display();
        plog("[frame %llu] cycles=%llu exc=%llu bios=%llu last=%03X istat=%03X imask=%03X sr=%08X disp=%dx%d@%d,%d%s%s\n",
             g_dbg_frames, g_cycles, g_dbg_exceptions, g_dbg_bios, g_dbg_last_bios, g_istat, g_imask, g_cpu.cop0[12],
             d.w, d.h, d.x, d.y, d.enabled ? "" : " off", d.depth24 ? " 24bit" : "");
        extern int g_dbg_audio_peak;
        extern int g_dbg_xa_peak, g_dbg_cdvol;
        plog("    audio peak %d xa %d cdmix %05X spu cdvol %04X/%04X main %04X/%04X cnt %04X\n", g_dbg_audio_peak, g_dbg_xa_peak,
             g_dbg_cdvol, spu_read(0x1B0), spu_read(0x1B2), spu_read(0x180), spu_read(0x182), spu_read(0x1AA));
        g_dbg_xa_peak = 0;
        g_dbg_audio_peak = 0;
        // wall-clock speed and the share spent drawing/presenting (pacing sleep included in present)
        static auto t0 = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        t0 = t1;
        u64 h = 1469598103934665603ull; // RAM fingerprint: identical runs must match (netplay determinism)
        for (u32 i = 0; i < 0x200000; i += 8) { u64 w; memcpy(&w, g_ram + i, 8); h = (h ^ w) * 1099511628211ull; }
        plog("    ram %016llX\n", h);
        int platform_private_mb();
        plog("    speed %.1f fps, present %.1f ms/frame, mem %d MB\n", 60000.0 / ms, g_dbg_present_us / 60000.0, platform_private_mb());
        g_dbg_present_us = 0;
    }
    // test harness: TB_RAM_DUMP=vblank writes main RAM (ram.bin, 2 MB) and the CPU copy of VRAM (vram.bin, 1 MB,
    // raw 16-bit words) to the shot folder at that vblank
    static const long long ram_dump_at = getenv("TB_RAM_DUMP") ? atoll(getenv("TB_RAM_DUMP")) : -1;
    if ((long long)g_dbg_frames == ram_dump_at) {
        std::string dir = getenv("TB_SHOT_DIR") ? getenv("TB_SHOT_DIR") : g_base_dir;
        if (FILE* f = fopen((dir + PATH_SEP "ram.bin").c_str(), "wb")) { fwrite(g_ram, 1, 0x200000, f); fclose(f); }
        if (FILE* f = fopen((dir + PATH_SEP "vram.bin").c_str(), "wb")) { fwrite(gpu_vram(), 2, 1024 * 512, f); fclose(f); }
        plog("ram and vram dumped to %s\n", dir.c_str());
    }
    // test harness: TB_RAM_TRACE=from:to logs the cycle count and a RAM fingerprint at every vblank in that range
    // (where two builds that should run the same first part ways)
    static long long trace_from = -1, trace_to = -1;
    if (trace_from < 0) { trace_from = 0; if (const char* e = getenv("TB_RAM_TRACE")) sscanf(e, "%lld:%lld", &trace_from, &trace_to); }
    if ((long long)g_dbg_frames >= trace_from && (long long)g_dbg_frames <= trace_to) {
        u64 h = 1469598103934665603ull;
        for (u32 i = 0; i < 0x200000; i += 8) { u64 w; memcpy(&w, g_ram + i, 8); h = (h ^ w) * 1099511628211ull; }
        plog("vb %llu cycles=%llu ram=%016llX\n", (unsigned long long)g_dbg_frames, (unsigned long long)g_cycles, (unsigned long long)h);
    }
    static const char* poke = getenv("TB_POKE8"); // test harness: "hexaddr=value", a RAM byte forced every vblank
    if (poke) {
        unsigned a = 0, v = 0;
        if (sscanf(poke, "%x=%u", &a, &v) == 2) g_ram[a & 0x1FFFFF] = (u8)v;
    }
    if (gpu_tracing()) { // the code the vblank interrupts: usually the game's VSync wait
        static int stacks = 0;
        if (stacks++ < 4) { plog("  vblank stack:\n"); void platform_log_stack_here(); platform_log_stack_here(); }
    }
    irq_raise(IRQ_VBLANK);
    timers_vblank(true);
    auto p0 = std::chrono::steady_clock::now();
    gpu_vblank();
    g_dbg_present_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - p0).count();
    bios_vblank();
    card_vblank();
}

// ---------------------------------------------------------------- root counters
struct Timer {
    u32 mode, target;
    double base;      // cpu time at which the counter was zero
    bool irq_done;    // one-shot already fired
    bool paused;
    u32 paused_value;
};
static Timer s_tm[3];

static double timer_rate(int n) {
    u32 src = (s_tm[n].mode >> 8) & 3;
    if (n == 0 && (src & 1)) {
        // dot clock: depends on the horizontal resolution
        DisplayInfo d = gpu_display();
        int div = d.w >= 640 ? 4 : d.w >= 512 ? 5 : d.w >= 368 ? 7 : d.w >= 320 ? 8 : 10;
        return div * (33.8688 / 53.693175);
    }
    if (n == 1 && (src & 1)) return (double)CYCLES_PER_LINE;
    if (n == 2 && (src & 2)) return 8.0;
    return 1.0;
}

static u32 timer_period(int n) {
    const Timer& t = s_tm[n];
    if (t.mode & 0x8) return (t.target ? t.target : 0xFFFF) + 1;
    return 0x10000;
}

static u32 timer_value(int n) {
    const Timer& t = s_tm[n];
    if (t.paused) return t.paused_value;
    double el = ((double)cpu_now() - t.base) / timer_rate(n);
    if (el < 0) el = 0;
    return (u32)((u64)el % timer_period(n));
}

static void timer_schedule(int n);

static void ev_timer(int id) {
    int n = id - EV_TIMER0;
    Timer& t = s_tm[n];
    if (t.irq_done && !(t.mode & 0x40)) return;
    u32 v = timer_value(n);
    bool hit_t = (t.mode & 0x10) && (v == t.target || (t.mode & 8));
    bool hit_f = (t.mode & 0x20);
    if (hit_t) t.mode |= 0x800;
    if (hit_f && !(t.mode & 8)) t.mode |= 0x1000;
    if (t.mode & 0x80) t.mode ^= 0x400; else t.mode &= ~0x400u;
    irq_raise(IRQ_TIMER0 + n);
    if (!(t.mode & 0x80)) t.mode |= 0x400;
    t.irq_done = true;
    if (t.mode & 0x40) timer_schedule(n);
}

static void timer_schedule(int n) {
    Timer& t = s_tm[n];
    sched_cancel(EV_TIMER0 + n);
    if (t.paused) return;
    if (!(t.mode & 0x30)) return;
    double rate = timer_rate(n);
    u32 v = timer_value(n);
    u32 period = timer_period(n);
    u32 stop;
    if ((t.mode & 0x10) && (t.target > v || !(t.mode & 0x20))) stop = t.target;
    else stop = 0xFFFF;
    if (stop > period - 1) stop = period - 1;
    u32 dist = stop > v ? stop - v : period - v + stop;
    if (dist == 0) dist = period;
    sched_set(EV_TIMER0 + n, (u64)(dist * rate) + 1, ev_timer);
}

void timers_init() {
    memset(s_tm, 0, sizeof s_tm);
    for (int n = 0; n < 3; n++) s_tm[n].mode = 0x400;
}

void timers_vblank(bool start) {
    // timer 1 synchronisation with vblank
    Timer& t = s_tm[1];
    if (!start || !(t.mode & 1)) return;
    u32 sm = (t.mode >> 1) & 3;
    if (sm == 1 || sm == 2) { t.base = (double)cpu_now(); timer_schedule(1); }
}

void timers_hblank_reset() {}

u32 timers_read(u32 off) {
    int n = off >> 4;
    if (n > 2) return 0;
    Timer& t = s_tm[n];
    switch (off & 0xF) {
    case 0: return timer_value(n);
    case 4: {
        // refresh reached flags
        u32 v = t.mode;
        t.mode &= ~0x1800u;
        return v;
    }
    case 8: return t.target;
    }
    return 0;
}

void timers_write(u32 off, u32 v) {
    int n = off >> 4;
    if (n > 2) return;
    Timer& t = s_tm[n];
    switch (off & 0xF) {
    case 0:
        t.base = (double)cpu_now() - (double)(v & 0xFFFF) * timer_rate(n);
        if (t.paused) t.paused_value = v & 0xFFFF;
        break;
    case 4:
        t.mode = (v & 0x3FF) | 0x400;
        t.base = (double)cpu_now();
        t.irq_done = false;
        t.paused = (n == 2 && (t.mode & 1) && (((t.mode >> 1) & 3) == 0 || ((t.mode >> 1) & 3) == 3));
        t.paused_value = 0;
        break;
    case 8:
        t.target = v & 0xFFFF;
        break;
    }
    timer_schedule(n);
}

// ---------------------------------------------------------------- DMA
struct DmaCh { u32 madr, bcr, chcr; };
static DmaCh s_dma[7];
static u32 s_dpcr = 0x07654321, s_dicr = 0;
static bool s_mdec_out_pending = false;

static void dicr_update() {
    bool m = (s_dicr & 0x8000) || ((s_dicr & 0x800000) && ((s_dicr >> 16) & (s_dicr >> 24) & 0x7F));
    bool old = (s_dicr & 0x80000000u) != 0;
    if (m) s_dicr |= 0x80000000u; else s_dicr &= ~0x80000000u;
    if (m && !old) irq_raise(IRQ_DMA);
}

static void dma_finish(int n) {
    s_dma[n].chcr &= ~0x11000000u;
    if (s_dicr & (1u << (16 + n))) s_dicr |= 1u << (24 + n);
    dicr_update();
}

static inline u32 r32(u32 a) { u32 v; memcpy(&v, g_ram + (a & 0x1FFFFC), 4); return v; }
static inline void w32(u32 a, u32 v) {
    if ((a & 0x1FFFFC) - g_watch_lo < g_watch_len) watch_hit(a, v, 40);
    memcpy(g_ram + (a & 0x1FFFFC), &v, 4);
}

static void dma_run(int n) {
    DmaCh& c = s_dma[n];
    u32 sync = (c.chcr >> 9) & 3;
    bool from_ram = c.chcr & 1;
    int step = (c.chcr & 2) ? -4 : 4;
    u32 addr = c.madr & 0x1FFFFC;

    if (n == 2 && sync == 2) {
        // GPU linked list
        u32 guard = 0;
        static std::vector<u32> buf;
        while (guard++ < 0x100000) {
            u32 hdr = r32(addr);
            u32 cnt = hdr >> 24;
            buf.resize(cnt);
            for (u32 k = 0; k < cnt; k++) buf[k] = r32(addr + 4 + k * 4);
            g_gpu_packet = addr; // frame interpolation matches primitives by the order the game built them
            if (cnt) gpu_dma_write(buf.data(), (int)cnt);
            g_gpu_packet = 0;
            if (hdr & 0x800000) break;
            addr = hdr & 0x1FFFFC;
        }
        c.madr = 0x00FFFFFF;
        dma_finish(n);
        return;
    }

    u32 words = sync == 0 ? ((c.bcr & 0xFFFF) ? (c.bcr & 0xFFFF) : 0x10000) : (c.bcr & 0xFFFF) * (c.bcr >> 16);
    switch (n) {
    case 0: { // MDEC in
        std::vector<u32> buf(words);
        for (u32 k = 0; k < words; k++) { buf[k] = r32(addr); addr += step; }
        mdec_dma_in(buf.data(), (int)words);
        break;
    }
    case 1: { // MDEC out
        if (!mdec_out_ready()) { s_mdec_out_pending = true; return; }
        std::vector<u32> buf(words);
        mdec_dma_out(buf.data(), (int)words);
        for (u32 k = 0; k < words; k++) { w32(addr, buf[k]); addr += step; }
        break;
    }
    case 2:
        if (from_ram) {
            std::vector<u32> buf(words);
            for (u32 k = 0; k < words; k++) { buf[k] = r32(addr); addr += step; }
            gpu_dma_write(buf.data(), (int)words);
        } else {
            for (u32 k = 0; k < words; k++) { w32(addr, gpu_read_data()); addr += step; }
        }
        break;
    case 3: { // CD-ROM: never write past the sector data actually waiting in the fifo
        u32 cd_fifo_remaining();
        u32 avail = (cd_fifo_remaining() + 3) / 4;
        if (sync == 0 && (c.bcr & 0xFFFF) == 0) avail = 0; // CdGetSector(buf, 0) after the last sector
        if (words > avail) { log_once("dma3clamp", "dma3: request of %u words clamped to %u\n", words, avail); words = avail; }
        for (u32 k = 0; k < words; k++) { w32(addr, cd_dma_read()); addr += step; }
        break;
    }
        break;
    case 4: // SPU
        if (from_ram) spu_dma_write(g_ram + addr, words * 4);
        else spu_dma_read(g_ram + addr, words * 4);
        addr += words * 4;
        break;
    case 6: // ordering table clear
        for (u32 k = 0; k < words; k++) {
            w32(addr, k == words - 1 ? 0x00FFFFFF : ((addr - 4) & 0x1FFFFF));
            addr -= 4;
        }
        break;
    default:
        log_once("dma5", "DMA channel %d not supported\n", n);
        break;
    }
    c.madr = addr & 0xFFFFFF;
    if (sync == 1) c.bcr &= 0xFFFF;
    dma_finish(n);
}

static void dma_try(int n) {
    DmaCh& c = s_dma[n];
    if (!(c.chcr & 0x01000000)) return;
    u32 sync = (c.chcr >> 9) & 3;
    if (sync == 0 && !(c.chcr & 0x10000000)) return;
    if (g_settings.log_level >= 3 && n != 2)
        plog("dma%d start madr=%08X bcr=%08X chcr=%08X dpcr=%08X [loader: sectors=%d bytes=%d dest=%08X]\n", n, c.madr, c.bcr, c.chcr, s_dpcr,
             (s32)r32(0x8001020C), (s32)r32(0x80010214), r32(0x8001021C));
    if (!((s_dpcr >> (n * 4 + 3)) & 1)) return;
    dma_run(n);
}

void dma_init() {
    memset(s_dma, 0, sizeof s_dma);
    s_dpcr = 0x07654321;
    s_dicr = 0;
}

void dma_mdec_ready() {
    if (s_mdec_out_pending) { s_mdec_out_pending = false; dma_try(1); }
}

u32 dma_read(u32 off) {
    off &= ~3u;
    if (off == 0x70) return s_dpcr;
    if (off == 0x74) return s_dicr;
    int n = off >> 4;
    if (n > 6) return 0;
    switch (off & 0xF) {
    case 0: return s_dma[n].madr;
    case 4: return s_dma[n].bcr;
    case 8: return s_dma[n].chcr;
    }
    return 0;
}

void dma_write(u32 off, u32 v) {
    off &= ~3u;
    if (off == 0x70) { s_dpcr = v; for (int n = 0; n < 7; n++) dma_try(n); return; }
    if (off == 0x74) {
        u32 ack = v & 0x7F000000;
        s_dicr = (s_dicr & 0x7F000000 & ~ack) | (v & 0x00FF803F);
        dicr_update();
        return;
    }
    int n = off >> 4;
    if (n > 6) return;
    switch (off & 0xF) {
    case 0: s_dma[n].madr = v & 0xFFFFFF; break;
    case 4: s_dma[n].bcr = v; break;
    case 8:
        if (n == 6) s_dma[n].chcr = (v & 0x51000000) | 2;
        else s_dma[n].chcr = v;
        dma_try(n);
        break;
    }
}

// ---------------------------------------------------------------- startup
void sched_start() {
    sched_set(EV_VBLANK, CYCLES_PER_FRAME, ev_vblank);
}
