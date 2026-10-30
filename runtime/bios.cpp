// bios.cpp - high level replacement for the PlayStation kernel (no BIOS ROM needed).
#include "psx.h"
#include <cstdlib>
#include <cctype>
#include <cstdarg>
#include <deque>
#include <algorithm>

static inline u32 rd32(u32 a) { u32 v; memcpy(&v, g_ram + (a & 0x1FFFFC), 4); return v; }
static inline void wr32(u32 a, u32 v) { memcpy(g_ram + (a & 0x1FFFFC), &v, 4); }
static inline u8* ptr(u32 a) { return g_ram + (a & 0x1FFFFF); }
static std::string rdstr(u32 a, size_t max = 256) {
    std::string s;
    for (size_t i = 0; i < max; i++) { char ch = (char)mem_read8(a + (u32)i); if (!ch) break; s += ch; }
    return s;
}

const u32 EXC_STACK = 0x8000FF00;

// ---------------------------------------------------------------- events
struct Event { u32 cls, spec, mode, func, status; };
enum { EvStUNUSED = 0, EvStWAIT = 0x1000, EvStACTIVE = 0x2000, EvStALREADY = 0x4000 };
enum { EvMdINTR = 0x1000, EvMdNOINTR = 0x2000 };
static Event s_events[32];

static void deliver_event(CPU* c, u32 cls, u32 spec) {
    for (auto& e : s_events) {
        if (e.status == EvStUNUSED || e.cls != cls || e.spec != spec) continue;
        if (e.status != EvStACTIVE) continue;
        if (e.mode == EvMdINTR) {
            if (e.func) {
                CPU saved = *c;
                c->r[31] = 0;
                rt_call(c, e.func);
                *c = saved;
            }
        } else {
            e.status = EvStALREADY;
        }
    }
}

// ---------------------------------------------------------------- kernel state
static u32 s_custom_exit = 0;
static u32 s_chains[4];
static bool s_clear_rcnt[4] = {true, true, true, true};
static bool s_clear_pad = true;
static u32 s_pad_buf[2], s_pad_len[2];
static bool s_pad_started = false;
static u32 s_old_pad_buf = 0;
static bool s_card_started = false;

// card events are delivered a little later, like the real hardware does
struct PendingEv { u32 cls, spec; };
static std::deque<PendingEv> s_card_events;
static void ev_card(int) {
    while (!s_card_events.empty()) {
        PendingEv p = s_card_events.front();
        s_card_events.pop_front();
        deliver_event(&g_cpu, p.cls, p.spec);
    }
}
static void queue_card_event(u32 spec) {
    s_card_events.push_back({0xF4000001, spec}); // SwCARD
    s_card_events.push_back({0xF0000011, spec}); // HwCARD
    if (!sched_active(EV_CARD)) sched_set(EV_CARD, 20000, ev_card);
}
enum { EvSpIOE = 0x0004, EvSpERROR = 0x8000, EvSpTIMOUT = 0x0100, EvSpNEW = 0x2000, EvSpINT = 0x0002 };

// ---------------------------------------------------------------- heap (A0:33..39)
static u32 s_heap_base = 0, s_heap_end = 0;
// block header: size (bytes incl. header) | 1 if used

static u32 heap_malloc(u32 size) {
    if (!s_heap_base) return 0;
    size = (size + 7) & ~7u;
    u32 need = size + 8;
    for (u32 p = s_heap_base; p + 8 <= s_heap_end;) {
        u32 h = rd32(p), bs = h & ~7u;
        if (bs == 0) break;
        if (!(h & 1)) {
            // merge following free blocks
            while (p + bs < s_heap_end) {
                u32 n = rd32(p + bs);
                if ((n & 1) || (n & ~7u) == 0) break;
                bs += n & ~7u;
            }
            wr32(p, bs);
            if (bs >= need) {
                if (bs - need >= 16) {
                    wr32(p, need | 1);
                    wr32(p + need, bs - need);
                } else wr32(p, bs | 1);
                return p + 8;
            }
        }
        p += bs;
    }
    log_once("heap", "malloc(%u) failed\n", size);
    return 0;
}
static void heap_free(u32 a) {
    if (!a || a < s_heap_base + 8 || a >= s_heap_end) return;
    u32 p = a - 8;
    wr32(p, rd32(p) & ~7u);
}
static u32 heap_size(u32 a) { return (rd32(a - 8) & ~7u) - 8; }

// ---------------------------------------------------------------- printf
static std::string guest_format(CPU* c, u32 fmt, int first_arg) {
    std::string out;
    int argi = first_arg;
    auto arg = [&]() -> u32 {
        u32 v = argi < 4 ? c->r[4 + argi] : rd32(c->r[29] + argi * 4);
        argi++;
        return v;
    };
    std::string f = rdstr(fmt, 1024);
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%') { out += f[i]; continue; }
        std::string spec = "%";
        i++;
        while (i < f.size() && strchr("-+ #0123456789.lh", f[i])) { if (f[i] != 'l' && f[i] != 'h') spec += f[i]; i++; }
        if (i >= f.size()) break;
        char t = f[i];
        char buf[512];
        switch (t) {
        case 'd': case 'i': snprintf(buf, sizeof buf, (spec + "d").c_str(), (s32)arg()); out += buf; break;
        case 'u': case 'x': case 'X': case 'o': snprintf(buf, sizeof buf, (spec + t).c_str(), arg()); out += buf; break;
        case 'c': out += (char)arg(); break;
        case 'p': snprintf(buf, sizeof buf, "%08X", arg()); out += buf; break;
        case 's': { std::string s = rdstr(arg()); snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str()); out += buf; break; }
        case '%': out += '%'; break;
        default: out += t; break;
        }
    }
    return out;
}

// ---------------------------------------------------------------- memory card file system
struct CardFile { bool open; int slot; int first_block; u32 pos, size; u32 mode; };
static CardFile s_fd[16];
static u32 s_find_slot, s_find_index;
static std::string s_find_pattern;

static u8 dir_frame(int slot, int n, u8* f) { card_read_frame(slot, n, f); return f[0]; }

static void dir_write(int slot, int n, u8* f) {
    u8 x = 0;
    for (int i = 0; i < 127; i++) x ^= f[i];
    f[127] = x;
    card_write_frame(slot, n, f);
}

static bool name_match(const std::string& pat, const char* name) {
    size_t i = 0;
    for (; i < pat.size(); i++) {
        if (pat[i] == '*') return true;
        if (!name[i]) return false;
        if (pat[i] != '?' && toupper((u8)pat[i]) != toupper((u8)name[i])) return false;
    }
    return name[i] == 0;
}

static bool parse_card_path(const std::string& path, int& slot, std::string& name) {
    if (path.size() < 5 || tolower(path[0]) != 'b' || tolower(path[1]) != 'u') return false;
    slot = path[2] - '0';
    if (slot < 0 || slot > 1) return false;
    size_t c = path.find(':');
    name = c == std::string::npos ? "" : path.substr(c + 1);
    return true;
}

static int find_file_block(int slot, const std::string& name) {
    u8 f[128];
    for (int n = 1; n < 16; n++) {
        if (dir_frame(slot, n, f) != 0x51) continue;
        if (_stricmp((const char*)f + 10, name.c_str()) == 0) return n;
    }
    return -1;
}

static int next_block(int slot, int b) {
    u8 f[128];
    dir_frame(slot, b, f);
    u16 nx = f[8] | (f[9] << 8);
    return nx == 0xFFFF ? -1 : nx + 1;
}

static int card_open(const std::string& path, u32 mode) {
    int slot;
    std::string name;
    if (!parse_card_path(path, slot, name) || !card_present(slot)) return -1;
    int fdn = -1;
    for (int i = 2; i < 16; i++) if (!s_fd[i].open) { fdn = i; break; }
    if (fdn < 0) return -1;
    int blk = find_file_block(slot, name);
    if (mode & 0x200) {
        if (blk >= 0) return -1; // already exists
        int nblocks = (int)(mode >> 16);
        if (nblocks < 1) nblocks = 1;
        std::vector<int> freeb;
        u8 f[128];
        for (int n = 1; n < 16 && (int)freeb.size() < nblocks; n++) {
            u8 st = dir_frame(slot, n, f);
            if (st == 0xA0 || st == 0xA1 || st == 0xA2 || st == 0xA3 || st == 0x00) freeb.push_back(n);
        }
        if ((int)freeb.size() < nblocks) return -1;
        for (int k = 0; k < nblocks; k++) {
            memset(f, 0, 128);
            f[0] = k == 0 ? 0x51 : (k == nblocks - 1 ? 0x53 : 0x52);
            if (nblocks == 1) f[0] = 0x51;
            u32 sz = k == 0 ? nblocks * 8192 : 0;
            memcpy(f + 4, &sz, 4);
            u16 nx = k == nblocks - 1 ? 0xFFFF : (u16)(freeb[k + 1] - 1);
            f[8] = nx & 0xFF; f[9] = nx >> 8;
            if (k == 0) strncpy((char*)f + 10, name.c_str(), 20);
            dir_write(slot, freeb[k], f);
        }
        blk = freeb[0];
        plog("memcard: created %s (%d blocks) in slot %d\n", name.c_str(), nblocks, slot + 1);
    }
    if (blk < 0) return -1;
    u8 f[128];
    dir_frame(slot, blk, f);
    u32 size;
    memcpy(&size, f + 4, 4);
    s_fd[fdn] = {true, slot, blk, 0, size, mode};
    return fdn;
}

static u32 card_frame_of(const CardFile& cf, u32 pos) {
    int b = cf.first_block;
    for (u32 k = 0; k < pos / 8192 && b >= 0; k++) b = next_block(cf.slot, b);
    if (b < 0) return 0xFFFFFFFF;
    return (u32)b * 64 + (pos % 8192) / 128;
}

static s32 card_rw(int fdn, u32 buf, u32 len, bool write) {
    if (fdn < 2 || fdn >= 16 || !s_fd[fdn].open) return -1;
    CardFile& cf = s_fd[fdn];
    u32 done = 0;
    while (done + 128 <= len && cf.pos < cf.size) {
        u32 fr = card_frame_of(cf, cf.pos);
        if (fr == 0xFFFFFFFF) break;
        if (write) card_write_frame(cf.slot, fr, ptr(buf + done));
        else card_read_frame(cf.slot, fr, ptr(buf + done));
        done += 128;
        cf.pos += 128;
    }
    queue_card_event(EvSpIOE);
    // files opened non-blocking (mode bit 15) return 0 = "request accepted"; completion is the event.
    // Team Buddies loops on read() until it returns 0.
    return (cf.mode & 0x8000) ? 0 : (s32)done;
}

static u32 fill_dirent(int slot, int n, u32 de) {
    u8 f[128];
    dir_frame(slot, n, f);
    memset(ptr(de), 0, 40);
    memcpy(ptr(de), f + 10, 20);
    u32 sz;
    memcpy(&sz, f + 4, 4);
    wr32(de + 0x14, 0x50);
    wr32(de + 0x18, sz);
    wr32(de + 0x20, n);
    return de;
}

static u32 card_find(u32 de) {
    u8 f[128];
    for (; s_find_index < 16; s_find_index++) {
        if (dir_frame(s_find_slot, s_find_index, f) != 0x51) continue;
        if (!name_match(s_find_pattern, (const char*)f + 10)) continue;
        u32 r = fill_dirent(s_find_slot, s_find_index, de);
        s_find_index++;
        return r;
    }
    return 0;
}

static bool card_delete(const std::string& path) {
    int slot;
    std::string name;
    if (!parse_card_path(path, slot, name)) return false;
    int b = find_file_block(slot, name);
    if (b < 0) return false;
    while (b >= 0) {
        u8 f[128];
        dir_frame(slot, b, f);
        int nx = next_block(slot, b);
        f[0] = (u8)(0xA0 | (f[0] & 0x0F));
        dir_write(slot, b, f);
        b = nx;
    }
    return true;
}

// ---------------------------------------------------------------- pads
PadState g_pads[4];

static void write_pad(u8* d, const PadState& p) {
    u16 b = (u16)~p.buttons;
    if (!p.connected) { d[0] = 0xFF; d[1] = 0xFF; d[2] = 0xFF; d[3] = 0xFF; return; }
    d[0] = 0x00;
    d[1] = 0x41;
    d[2] = b & 0xFF;
    d[3] = b >> 8;
}

static void pad_update() {
    if (s_pad_started) {
        for (int port = 0; port < 2; port++) {
            if (!s_pad_buf[port]) continue;
            u8* d = ptr(s_pad_buf[port]);
            if (port == 0 && g_settings.multitap && s_pad_len[port] >= 34) {
                d[0] = 0x00;
                d[1] = 0x80;
                for (int k = 0; k < 4; k++) {
                    u8* s = d + 2 + k * 8;
                    memset(s, 0xFF, 8);
                    write_pad(s, g_pads[k]);
                }
            } else {
                write_pad(d, g_pads[port]);
            }
        }
    }
    if (s_old_pad_buf) {
        u32 v = 0;
        for (int port = 0; port < 2; port++) {
            u16 b = g_pads[port].connected ? (u16)~g_pads[port].buttons : 0xFFFF;
            u32 sw = ((b & 0xFF) << 8) | (b >> 8);
            v |= sw << (16 * port);
        }
        wr32(s_old_pad_buf, v);
    }
}

void bios_vblank() {}

// ---------------------------------------------------------------- exception (interrupt) entry
extern u64 g_dbg_exceptions;
void bios_irq_exception(CPU* c) {
    g_dbg_exceptions++;
    CPU saved = *c;
    u32 sr = c->cop0[12];
    c->cop0[12] = (sr & ~0x3Fu) | ((sr << 2) & 0x3C);
    c->cop0[13] = (c->cop0[13] & ~0x7Cu);
    g_exception_depth++;
    try {
        c->r[29] = EXC_STACK;
        // kernel handlers: pads on vblank, root counter events
        if (g_istat & g_imask & 1) {
            pad_update();
            if (s_clear_pad && s_pad_started) g_istat &= ~1u;
        }
        static const int rc_irq[4] = {IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_VBLANK};
        for (int n = 0; n < 4; n++) {
            u32 bit = 1u << rc_irq[n];
            if (!(g_istat & g_imask & bit)) continue;
            deliver_event(c, 0xF2000000u + n, EvSpINT);
            if (s_clear_rcnt[n]) g_istat &= ~bit;
        }
        // handlers registered with SysEnqIntRP
        for (int pr = 0; pr < 4; pr++) {
            u32 node = s_chains[pr];
            for (int guard = 0; node && guard < 32; guard++) {
                u32 next = rd32(node), second = rd32(node + 4), first = rd32(node + 8);
                if (first) {
                    c->r[31] = 0;
                    rt_call(c, first);
                    c->escape = 0; // a handler can't unwind past the kernel
                    u32 v = c->r[2];
                    if (v && second) {
                        c->r[4] = v;
                        c->r[31] = 0;
                        rt_call(c, second);
                        c->escape = 0;
                    }
                }
                node = next;
            }
        }
        if (s_custom_exit) {
            u32 b = s_custom_exit;
            c->r[31] = rd32(b + 0);
            c->r[29] = rd32(b + 4);
            c->r[30] = rd32(b + 8);
            for (int i = 0; i < 8; i++) c->r[16 + i] = rd32(b + 12 + i * 4);
            c->r[28] = rd32(b + 44);
            c->r[2] = 1;
            rt_call(c, c->r[31]);
        } else {
            g_istat &= ~g_imask;
        }
    } catch (ExceptionReturn&) {
    }
    g_exception_depth--;
    *c = saved;
    if (irq_take_now(c)) g_next_event = 0;
}

// ---------------------------------------------------------------- boot
void bios_init() {
    memset(s_events, 0, sizeof s_events);
    memset(s_chains, 0, sizeof s_chains);
    memset(s_fd, 0, sizeof s_fd);
    s_custom_exit = 0;
    // fake kernel function tables (same places as the real kernel): libraries that look up or
    // patch kernel functions get addresses that rt_call maps back to the HLE kernel
    for (u32 i = 0; i < 0xC0; i++) wr32(0x80000200 + i * 4, 0x80001000 + i * 4);
    for (u32 i = 0; i < 0x60; i++) wr32(0x80000874 + i * 4, 0x80001400 + i * 4);
    for (u32 i = 0; i < 0x20; i++) wr32(0x80000674 + i * 4, 0x80001800 + i * 4);
}

void bios_boot(const std::vector<u8>& exe) {
    u32 pc0 = *(u32*)&exe[0x10], gp = *(u32*)&exe[0x14];
    u32 taddr = *(u32*)&exe[0x18], tsize = *(u32*)&exe[0x1C];
    u32 baddr = *(u32*)&exe[0x28], bsize = *(u32*)&exe[0x2C];
    u32 sp = *(u32*)&exe[0x30] ? *(u32*)&exe[0x30] + *(u32*)&exe[0x34] : 0x801FFFF0;
    if (tsize > exe.size() - 0x800) tsize = (u32)exe.size() - 0x800;
    memcpy(ptr(taddr), &exe[0x800], tsize);
    if (bsize) memset(ptr(baddr), 0, bsize);
    g_cpu.r[28] = gp;
    g_cpu.r[29] = sp;
    g_cpu.r[30] = sp;
    g_cpu.r[31] = 0;
    g_cpu.pc = pc0;
    plog("boot: entry %08X sp %08X gp %08X text %08X+%X\n", pc0, sp, gp, taddr, tsize);
}

// ---------------------------------------------------------------- dispatch
static void bios_a0(CPU* c, u32 fn) {
    u32 a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    u32& v0 = c->r[2];
    switch (fn) {
    case 0x00: v0 = (u32)card_open(rdstr(a0), a1); break;
    case 0x03: if (a0 == 1) { plog("%.*s", (int)std::min<u32>(a2, 4096), (const char*)ptr(a1)); v0 = a2; } else v0 = (u32)card_rw((int)a0, a1, a2, true); break;
    case 0x04: if (a0 < 16) s_fd[a0].open = false; v0 = a0; break;
    case 0x0E: v0 = (u32)abs((s32)a0); break;
    case 0x10: v0 = (u32)atoi(rdstr(a0).c_str()); break;
    case 0x15: { std::string s = rdstr(a0) + rdstr(a1); memcpy(ptr(a0), s.c_str(), s.size() + 1); v0 = a0; break; }
    case 0x17: v0 = (u32)strcmp(rdstr(a0).c_str(), rdstr(a1).c_str()); break;
    case 0x18: v0 = (u32)strncmp(rdstr(a0).c_str(), rdstr(a1).c_str(), a2); break;
    case 0x19: { std::string s = rdstr(a1); memcpy(ptr(a0), s.c_str(), s.size() + 1); v0 = a0; break; }
    case 0x1A: { std::string s = rdstr(a1); for (u32 i = 0; i < a2; i++) ptr(a0)[i] = i < s.size() ? s[i] : 0; v0 = a0; break; }
    case 0x1B: v0 = (u32)rdstr(a0, 0x100000).size(); break;
    case 0x25: v0 = (u32)toupper((int)(a0 & 0xFF)); break;
    case 0x26: v0 = (u32)tolower((int)(a0 & 0xFF)); break;
    case 0x27: memmove(ptr(a1), ptr(a0), a2); break;
    case 0x28: memset(ptr(a0), 0, a1); break;
    case 0x29: v0 = (u32)memcmp(ptr(a0), ptr(a1), a2); break;
    case 0x2A: memmove(ptr(a0), ptr(a1), a2); v0 = a0; break;
    case 0x2B: memset(ptr(a0), (int)(a1 & 0xFF), a2); v0 = a0; break;
    case 0x2C: memmove(ptr(a0), ptr(a1), a2); v0 = a0; break;
    case 0x2D: v0 = (u32)memcmp(ptr(a0), ptr(a1), a2); break;
    case 0x2F: { static u32 seed = 0x24040001; seed = seed * 1103515245 + 12345; v0 = (seed >> 16) & 0x7FFF; break; }
    case 0x33: v0 = heap_malloc(a0); break;
    case 0x34: heap_free(a0); break;
    case 0x37: v0 = heap_malloc(a0 * a1); if (v0) memset(ptr(v0), 0, a0 * a1); break;
    case 0x38: {
        if (!a0) { v0 = heap_malloc(a1); break; }
        if (!a1) { heap_free(a0); v0 = 0; break; }
        u32 old = heap_size(a0);
        u32 n = heap_malloc(a1);
        if (n) { memmove(ptr(n), ptr(a0), old < a1 ? old : a1); heap_free(a0); }
        v0 = n;
        break;
    }
    case 0x39:
        s_heap_base = (a0 + 7) & ~7u;
        s_heap_end = (a0 + a1) & ~7u;
        wr32(s_heap_base, s_heap_end - s_heap_base);
        plog("InitHeap %08X size %X\n", a0, a1);
        break;
    case 0x3C: plog("%c", (char)a0); break;
    case 0x3E: plog("%s\n", rdstr(a0).c_str()); break;
    case 0x3F: plog("%s", guest_format(c, a0, 1).c_str()); break;
    case 0x44: break; // FlushCache
    case 0x49: gpu_gp0(a0); break;
    case 0x4D: v0 = gpu_read(4); break;
    case 0x55: case 0x70: v0 = 1; break; // _bu_init
    case 0x56: case 0x72: break;         // CdRemove
    case 0xAB: // _card_info
        v0 = 1;
        queue_card_event(card_present((a0 >> 4) & 1) ? EvSpIOE : EvSpTIMOUT);
        break;
    case 0xAC: // _card_load
        v0 = 1;
        queue_card_event(card_present((a0 >> 4) & 1) ? EvSpIOE : EvSpTIMOUT);
        break;
    case 0xAD: v0 = 0; break;
    default:
        log_once(("A0" + std::to_string(fn)).c_str(), "unimplemented BIOS A0:%02X\n", fn);
        v0 = 0;
        break;
    }
}

static void bios_b0(CPU* c, u32 fn) {
    u32 a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    u32& v0 = c->r[2];
    switch (fn) {
    case 0x07: deliver_event(c, a0, a1); break;
    case 0x08: {
        v0 = 0xFFFFFFFF;
        for (int i = 0; i < 32; i++) {
            if (s_events[i].status != EvStUNUSED) continue;
            s_events[i] = {a0, a1, a2, a3, EvStWAIT};
            v0 = 0xF1000000u | i;
            break;
        }
        break;
    }
    case 0x09: s_events[a0 & 31].status = EvStUNUSED; v0 = 1; break;
    case 0x0A: {
        Event& e = s_events[a0 & 31];
        for (int guard = 0; e.status == EvStACTIVE && guard < 100000; guard++) {
            if (g_next_event > g_cycles) g_cycles = g_next_event;
            rt_events(c);
        }
        if (e.status == EvStALREADY) { e.status = EvStACTIVE; v0 = 1; } else v0 = 0;
        break;
    }
    case 0x0B: {
        Event& e = s_events[a0 & 31];
        if (e.status == EvStALREADY) { e.status = EvStACTIVE; v0 = 1; } else v0 = 0;
        break;
    }
    case 0x0C: if (s_events[a0 & 31].status != EvStUNUSED) s_events[a0 & 31].status = EvStACTIVE; v0 = 1; break;
    case 0x0D: if (s_events[a0 & 31].status != EvStUNUSED) s_events[a0 & 31].status = EvStWAIT; v0 = 1; break;
    case 0x12: // InitPAD
        s_pad_buf[0] = a0; s_pad_len[0] = a1; s_pad_buf[1] = a2; s_pad_len[1] = a3;
        if (a0) memset(ptr(a0), 0xFF, a1);
        if (a2) memset(ptr(a2), 0xFF, a3);
        v0 = 1;
        break;
    case 0x13: // StartPAD
        s_pad_started = true;
        g_imask |= 1;
        c->cop0[12] |= 0x401;
        irq_update();
        v0 = 1;
        break;
    case 0x14: s_pad_started = false; v0 = 1; break;
    case 0x15: // OutdatedPadInitAndStart
        s_old_pad_buf = a1;
        if (a1) wr32(a1, 0xFFFFFFFF);
        g_imask |= 1;
        c->cop0[12] |= 0x401;
        irq_update();
        v0 = 2;
        break;
    case 0x16: { u32 v = 0; for (int p = 0; p < 2; p++) v |= (u32)g_pads[p].buttons << (16 * p); v0 = v; break; }
    case 0x17: // ReturnFromException
        if (g_exception_depth > 0) throw ExceptionReturn();
        break;
    case 0x18: s_custom_exit = 0; break;
    case 0x19: s_custom_exit = a0; break;
    case 0x20: for (auto& e : s_events) if (e.cls == a0 && e.spec == a1 && e.status == EvStALREADY) e.status = EvStACTIVE; break;
    case 0x32: v0 = (u32)card_open(rdstr(a0), a1); break;
    case 0x33: {
        if (a0 >= 16 || !s_fd[a0].open) { v0 = 0xFFFFFFFF; break; }
        if (a2 == 0) s_fd[a0].pos = a1; else if (a2 == 1) s_fd[a0].pos += a1;
        v0 = s_fd[a0].pos;
        break;
    }
    case 0x34: v0 = (u32)card_rw((int)a0, a1, a2, false); break;
    case 0x35:
        if (a0 == 1) { plog("%.*s", (int)std::min<u32>(a2, 4096), (const char*)ptr(a1)); v0 = a2; break; } // stdout
        v0 = (u32)card_rw((int)a0, a1, a2, true);
        break;
    case 0x36: if (a0 < 16) s_fd[a0].open = false; v0 = a0; break;
    case 0x3D: plog("%c", (char)a0); break;
    case 0x3F: plog("%s\n", rdstr(a0).c_str()); break;
    case 0x41: { // FormatDevice
        int slot;
        std::string n;
        v0 = 0;
        if (parse_card_path(rdstr(a0), slot, n)) {
            u8 f[128];
            for (int b = 1; b < 16; b++) { memset(f, 0, 128); f[0] = 0xA0; f[8] = f[9] = 0xFF; dir_write(slot, b, f); }
            v0 = 1;
        }
        break;
    }
    case 0x42: {
        int slot;
        std::string pat;
        v0 = 0;
        if (parse_card_path(rdstr(a0), slot, pat) && card_present(slot)) {
            s_find_slot = slot;
            s_find_pattern = pat.empty() ? "*" : pat;
            s_find_index = 1;
            v0 = card_find(a1);
        }
        break;
    }
    case 0x43: v0 = card_find(a0); break;
    case 0x44: { // FileRename
        int s1, s2;
        std::string n1, n2;
        v0 = 0;
        if (parse_card_path(rdstr(a0), s1, n1) && parse_card_path(rdstr(a1), s2, n2) && s1 == s2) {
            int b = find_file_block(s1, n1);
            if (b >= 0 && find_file_block(s1, n2) < 0) {
                u8 f[128];
                dir_frame(s1, b, f);
                memset(f + 10, 0, 21);
                strncpy((char*)f + 10, n2.c_str(), 20);
                dir_write(s1, b, f);
                v0 = 1;
            }
        }
        break;
    }
    case 0x45: v0 = card_delete(rdstr(a0)) ? 1 : 0; break;
    case 0x4A: s_card_started = false; v0 = 1; break; // InitCARD
    case 0x4B: s_card_started = true; v0 = 1; break;  // StartCARD
    case 0x4C: s_card_started = false; v0 = 1; break; // StopCARD
    case 0x4E: { // write_card_sector(port, sector, src)
        int slot = (a0 >> 4) & 1;
        v0 = card_present(slot) && card_write_frame(slot, (int)a1, ptr(a2)) ? 1 : 0;
        queue_card_event(v0 ? EvSpIOE : EvSpTIMOUT);
        v0 = 1;
        break;
    }
    case 0x4F: { // read_card_sector(port, sector, dst)
        int slot = (a0 >> 4) & 1;
        v0 = card_present(slot) && card_read_frame(slot, (int)a1, ptr(a2)) ? 1 : 0;
        queue_card_event(v0 ? EvSpIOE : EvSpTIMOUT);
        v0 = 1;
        break;
    }
    case 0x50: v0 = 1; break; // allow_new_card
    case 0x54: case 0x55: v0 = 0; break;
    case 0x56: v0 = 0x80000674; break; // GetC0Table
    case 0x57: v0 = 0x80000874; break; // GetB0Table
    case 0x5B: s_clear_pad = a0 != 0; break;
    case 0x5C: v0 = 0x01; break;
    default:
        log_once(("B0" + std::to_string(fn)).c_str(), "unimplemented BIOS B0:%02X\n", fn);
        v0 = 0;
        break;
    }
}

static void bios_c0(CPU* c, u32 fn) {
    u32 a0 = c->r[4], a1 = c->r[5];
    u32& v0 = c->r[2];
    switch (fn) {
    case 0x02: { // SysEnqIntRP(priority, struct)
        u32 pr = a0 & 3;
        wr32(a1, s_chains[pr]);
        s_chains[pr] = a1;
        v0 = 0;
        break;
    }
    case 0x03: { // SysDeqIntRP
        u32 pr = a0 & 3;
        if (s_chains[pr] == a1) s_chains[pr] = rd32(a1);
        else {
            u32 p = s_chains[pr];
            for (int guard = 0; p && guard < 64; guard++) {
                u32 nn = rd32(p);
                if (nn == a1) { wr32(p, rd32(a1)); break; }
                p = nn;
            }
        }
        v0 = 0;
        break;
    }
    case 0x0A: { // ChangeClearRCnt(timer, flag)
        u32 t = a0 & 3;
        v0 = s_clear_rcnt[t] ? 1 : 0;
        s_clear_rcnt[t] = a1 != 0;
        break;
    }
    default:
        log_once(("C0" + std::to_string(fn)).c_str(), "unimplemented BIOS C0:%02X\n", fn);
        v0 = 0;
        break;
    }
}

extern u64 g_dbg_bios;
extern u32 g_dbg_last_bios;
void rt_bios(CPU* c, u32 vec) {
    u32 fn = c->r[9] & 0xFF;
    g_dbg_bios++;
    g_dbg_last_bios = ((vec >> 4) << 8) | fn;
    if (g_settings.log_level >= 3) plog("bios %02X:%02X a0=%08X a1=%08X\n", vec, fn, c->r[4], c->r[5]);
    const u32 a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    switch (vec) {
    case 0xA0: bios_a0(c, fn); break;
    case 0xB0: bios_b0(c, fn); break;
    case 0xC0: bios_c0(c, fn); break;
    }
    // memory card traffic (log_level 2)
    bool card = (vec == 0xA0 && (fn == 0xAB || fn == 0xAC || fn == 0x55 || fn == 0x70 || fn == 0x71)) ||
                (vec == 0xB0 && ((fn >= 0x32 && fn <= 0x36 && !(fn == 0x35 && a0 == 1)) || (fn >= 0x41 && fn <= 0x45) || (fn >= 0x4A && fn <= 0x50)));
    if (card && g_settings.log_level >= 2) {
        std::string s = (fn == 0x32 || fn == 0x41 || fn == 0x42 || fn == 0x45) && vec == 0xB0 ? " \"" + rdstr(a0) + "\"" : "";
        plog("card: %02X:%02X(%08X, %08X, %08X)%s -> %08X ra=%08X\n", vec, fn, a0, a1, a2, s.c_str(), c->r[2], c->r[31]);
    }
}
