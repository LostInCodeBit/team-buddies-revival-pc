// cpu.cpp - dispatch between recompiled functions, the fallback interpreter, COP0 and syscalls.
#include "psx.h"
#include <unordered_map>
#include <unordered_set>
#include <mutex>

CPU g_cpu;
u64 g_cycles;
u64 g_next_event;
int g_exception_depth;

static std::unordered_map<u32, std::vector<int>> s_index;
static std::vector<char> s_img_shared;

void cpu_init() {
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.cop0[12] = 0x40000000; // COP2 usable, interrupts off until the game enables them
    g_cpu.cop0[15] = 0x00000002;
    for (int i = 0; i < g_rec_func_count; i++) s_index[g_rec_funcs[i].addr].push_back(i);
    s_img_shared.assign(g_rec_image_count, 0);
    for (int i = 0; i < g_rec_image_count; i++)
        for (int j = 0; j < g_rec_image_count; j++)
            if (i != j && g_rec_images[i].base < g_rec_images[j].end && g_rec_images[j].base < g_rec_images[i].end &&
                g_rec_images[i].nchk > 0)
                s_img_shared[i] = 1;
    plog("recompiled functions: %d in %d images\n", g_rec_func_count, g_rec_image_count);
}

static inline u32 ram32(u32 a) { u32 v; memcpy(&v, g_ram + (a & 0x1FFFFC), 4); return v; }

static bool image_active(int img) {
    const RecImage& im = g_rec_images[img];
    for (int k = 0; k < im.nchk; k++)
        if (ram32(im.chk[k].a) != im.chk[k].w) return false;
    return true;
}

struct LookupCache { u32 addr; const std::vector<int>* v; };
static LookupCache s_cache[8192];

RecFn rec_lookup(u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p >= 0x00800000) return nullptr;
    a = 0x80000000u | (p & 0x1FFFFF);
    LookupCache& lc = s_cache[(a >> 2) & 8191];
    const std::vector<int>* v;
    if (lc.addr == a && lc.v) v = lc.v;
    else {
        auto it = s_index.find(a);
        if (it == s_index.end()) return nullptr;
        v = &it->second;
        lc.addr = a;
        lc.v = v;
    }
    u32 w0 = ram32(a), w1 = ram32(a + 4);
    for (int idx : *v) {
        const RecFunc& f = g_rec_funcs[idx];
        if (w0 != f.sig0 || w1 != f.sig1) continue;
        if (s_img_shared[f.img] && !image_active(f.img)) continue;
        return f.fn;
    }
    return nullptr;
}

// ---------------------------------------------------------------- misses (fed back into the recompiler)
static std::unordered_set<u32> s_missed;
static void note_miss(u32 pc) {
    pc = 0x80000000u | (pc & 0x1FFFFF);
    if (!s_missed.insert(pc).second) return;
    const char* id = "*";
    static std::string ids[32];
    for (int i = 0; i < g_rec_image_count; i++) {
        const RecImage& im = g_rec_images[i];
        if (pc >= im.base && pc < im.end && (!s_img_shared[i] || image_active(i))) {
            ids[i] = im.name;
            ids[i] = ids[i].substr(0, ids[i].find('.'));
            if (i == 0) ids[i] = "EXE";
            id = ids[i].c_str();
            break;
        }
    }
    plog("interpreter: entering %08X (%s)\n", pc, id);
    FILE* f = fopen((g_base_dir + "misses.txt").c_str(), "a");
    if (f) { fprintf(f, "%s %08X\n", id, pc); fclose(f); }
}

// ---------------------------------------------------------------- calls
void rt_call(CPU* c, u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p == 0xA0 || p == 0xB0 || p == 0xC0) { rt_bios(c, p); return; }
    if (p < 0x10000) {
        // kernel area: entries of the fake A0/B0/C0 tables map back to the HLE kernel
        if (p >= 0x1000 && p < 0x1C00) {
            u32 vec = p < 0x1400 ? 0xA0 : p < 0x1800 ? 0xB0 : 0xC0;
            c->r[9] = (p - (vec == 0xA0 ? 0x1000 : vec == 0xB0 ? 0x1400 : 0x1800)) >> 2;
            rt_bios(c, vec);
            return;
        }
        char key[32];
        snprintf(key, sizeof key, "kcall%X", p);
        log_once(key, "call into kernel memory %08X ignored (ra=%08X a0=%08X v0=%08X v1=%08X tbl=%08X %08X %08X %08X %08X)\n", a, c->r[31], c->r[4], c->r[2], c->r[3], mem_read32(0x800198D8), mem_read32(0x800198DC), mem_read32(0x800198E0), mem_read32(0x800198E4), mem_read32(0x800198E8));
        return;
    }
    RecFn fn = rec_lookup(a);
    if (fn) { fn(c); return; }
    interp_call(c, a);
}

void rt_jump(CPU* c, u32 t, u32 ra_entry) {
    if (t == ra_entry) return;
    rt_call(c, t);
}

void rt_break(CPU* c, u32 pc) {
    (void)c;
    log_once("break", "break instruction at %08X (division by zero?)\n", pc);
}

void rt_invalid(CPU* c, u32 pc) {
    (void)c;
    log_once("inv", "invalid instruction executed at %08X\n", pc);
}

void rt_syscall(CPU* c, u32 pc) {
    (void)pc;
    u32 sr = c->cop0[12];
    switch (c->r[4]) {
    case 1: // EnterCriticalSection
        c->r[2] = ((sr & 0x401) == 0x401) ? 1 : 0;
        c->cop0[12] = sr & ~0x401u;
        break;
    case 2: // ExitCriticalSection
        c->cop0[12] = sr | 0x401;
        irq_update();
        break;
    case 3: // ChangeThreadSubFunction
        c->r[2] = 1;
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- cop0
u32 cop0_read(CPU* c, u32 r) {
    if (r == 13) {
        u32 v = c->cop0[13] & ~0x400u;
        if (g_istat & g_imask) v |= 0x400;
        return v;
    }
    if (r == 15) return 2;
    return c->cop0[r & 31];
}

void cop0_write(CPU* c, u32 r, u32 v) {
    if (r == 13) {
        c->cop0[13] = (c->cop0[13] & ~0x300u) | (v & 0x300);
        return;
    }
    c->cop0[r & 31] = v;
    if (r == 12) irq_update();
}

void cop0_rfe(CPU* c) {
    u32 sr = c->cop0[12];
    c->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xF);
    irq_update();
}

// ---------------------------------------------------------------- events
void rt_events(CPU* c) {
    sched_run();
    if (irq_take_now(c)) bios_irq_exception(c);
}

// ---------------------------------------------------------------- interpreter (fallback for code not found statically)
static inline u32 SIMM(u32 i) { return (u32)(s32)(s16)(i & 0xFFFF); }

static void exec_simple(CPU* c, u32 pc, u32 i) {
    u32 rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31, sa = (i >> 6) & 31;
    u32 imm = i & 0xFFFF;
    u32* R = c->r;
    u32 v = 0;
    int dst = -1;
    switch (i >> 26) {
    case 0:
        switch (i & 63) {
        case 0: v = R[rt] << sa; dst = rd; break;
        case 2: v = R[rt] >> sa; dst = rd; break;
        case 3: v = (u32)((s32)R[rt] >> sa); dst = rd; break;
        case 4: v = R[rt] << (R[rs] & 31); dst = rd; break;
        case 6: v = R[rt] >> (R[rs] & 31); dst = rd; break;
        case 7: v = (u32)((s32)R[rt] >> (R[rs] & 31)); dst = rd; break;
        case 12: rt_syscall(c, pc); break;
        case 13: rt_break(c, pc); break;
        case 16: v = c->hi; dst = rd; break;
        case 17: c->hi = R[rs]; break;
        case 18: v = c->lo; dst = rd; break;
        case 19: c->lo = R[rs]; break;
        case 24: op_mult(c, R[rs], R[rt]); break;
        case 25: op_multu(c, R[rs], R[rt]); break;
        case 26: op_div(c, R[rs], R[rt]); break;
        case 27: op_divu(c, R[rs], R[rt]); break;
        case 32: case 33: v = R[rs] + R[rt]; dst = rd; break;
        case 34: case 35: v = R[rs] - R[rt]; dst = rd; break;
        case 36: v = R[rs] & R[rt]; dst = rd; break;
        case 37: v = R[rs] | R[rt]; dst = rd; break;
        case 38: v = R[rs] ^ R[rt]; dst = rd; break;
        case 39: v = ~(R[rs] | R[rt]); dst = rd; break;
        case 42: v = (s32)R[rs] < (s32)R[rt]; dst = rd; break;
        case 43: v = R[rs] < R[rt]; dst = rd; break;
        default: rt_invalid(c, pc); break;
        }
        break;
    case 8: case 9: v = R[rs] + SIMM(i); dst = rt; break;
    case 10: v = (s32)R[rs] < (s32)SIMM(i); dst = rt; break;
    case 11: v = R[rs] < SIMM(i); dst = rt; break;
    case 12: v = R[rs] & imm; dst = rt; break;
    case 13: v = R[rs] | imm; dst = rt; break;
    case 14: v = R[rs] ^ imm; dst = rt; break;
    case 15: v = imm << 16; dst = rt; break;
    case 16:
        if (rs == 0) { v = cop0_read(c, rd); dst = rt; }
        else if (rs == 4) cop0_write(c, rd, R[rt]);
        else if (rs == 16) cop0_rfe(c);
        break;
    case 18:
        if (i & (1u << 25)) gte_command(i & 0x1FFFFFF);
        else if (rs == 0) { v = gte_read_data(rd); dst = rt; }
        else if (rs == 2) { v = gte_read_ctrl(rd); dst = rt; }
        else if (rs == 4) gte_write_data(rd, R[rt]);
        else if (rs == 6) gte_write_ctrl(rd, R[rt]);
        break;
    case 32: v = (u32)(s32)(s8)mem_read8(R[rs] + SIMM(i)); dst = rt; break;
    case 33: v = (u32)(s32)(s16)mem_read16(R[rs] + SIMM(i)); dst = rt; break;
    case 34: v = op_lwl(R[rt], R[rs] + SIMM(i)); dst = rt; break;
    case 35: v = mem_read32(R[rs] + SIMM(i)); dst = rt; break;
    case 36: v = mem_read8(R[rs] + SIMM(i)); dst = rt; break;
    case 37: v = mem_read16(R[rs] + SIMM(i)); dst = rt; break;
    case 38: v = op_lwr(R[rt], R[rs] + SIMM(i)); dst = rt; break;
    case 40: mem_write8(R[rs] + SIMM(i), (u8)R[rt]); break;
    case 41: mem_write16(R[rs] + SIMM(i), (u16)R[rt]); break;
    case 42: op_swl(R[rs] + SIMM(i), R[rt]); break;
    case 43: mem_write32(R[rs] + SIMM(i), R[rt]); break;
    case 46: op_swr(R[rs] + SIMM(i), R[rt]); break;
    case 50: gte_write_data(rt, mem_read32(R[rs] + SIMM(i))); break;
    case 58: mem_write32(R[rs] + SIMM(i), gte_read_data(rt)); break;
    default: rt_invalid(c, pc); break;
    }
    if (dst > 0) R[dst] = v;
}

static bool is_control(u32 i) {
    u32 op = i >> 26;
    if (op == 0) { u32 f = i & 63; return f == 8 || f == 9; }
    return op >= 1 && op <= 7;
}

void interp_call(CPU* c, u32 pc) {
    note_miss(pc);
    const u32 ra_entry = c->r[31];
    int bad = 0;
    for (;;) {
        u32 i = mem_read32(pc);
        CYCLES(1);
        u32 op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31;
        if (!is_control(i)) {
            exec_simple(c, pc, i);
            pc += 4;
            continue;
        }
        u32 d = mem_read32(pc + 4);
        if (is_control(d)) { if (++bad < 4) plog("interpreter: branch in delay slot at %08X\n", pc); d = 0; }
        CYCLES(1);
        if (op == 0) {
            u32 t = c->r[rs];
            if ((i & 63) == 9) {
                u32 rd = (i >> 11) & 31;
                if (rd) c->r[rd] = pc + 8;
                exec_simple(c, pc + 4, d);
                rt_call(c, t);
                if (c->escape) { if (c->escape == ra_entry) c->escape = 0; return; }
                pc += 8;
                continue;
            }
            exec_simple(c, pc + 4, d);
            if (rs == 31 || t == ra_entry) {
                if (t != ra_entry) c->escape = t; // non-local return, see recomp "escapes"
                return;
            }
            u32 p = t & 0x1FFFFFFF;
            if (p < 0x10000) { rt_call(c, t); return; }
            if (RecFn fn = rec_lookup(t)) { fn(c); return; }
            pc = t;
            continue;
        }
        if (op == 2 || op == 3) {
            u32 t = ((pc + 4) & 0xF0000000) | ((i & 0x3FFFFFF) << 2);
            if (op == 3) {
                c->r[31] = pc + 8;
                exec_simple(c, pc + 4, d);
                rt_call(c, t);
                if (c->escape) { if (c->escape == ra_entry) c->escape = 0; return; }
                pc += 8;
                continue;
            }
            exec_simple(c, pc + 4, d);
            if (RecFn fn = rec_lookup(t)) { fn(c); return; }
            if (t <= pc) SAFEPOINT();
            pc = t;
            continue;
        }
        // conditional branches
        bool cnd = false, link = false;
        u32 a = c->r[rs], b = c->r[rt];
        switch (op) {
        case 1: cnd = (rt & 1) ? (s32)a >= 0 : (s32)a < 0; link = (rt & 0x1E) == 0x10; break;
        case 4: cnd = a == b; break;
        case 5: cnd = a != b; break;
        case 6: cnd = (s32)a <= 0; break;
        case 7: cnd = (s32)a > 0; break;
        }
        u32 t = pc + 4 + (SIMM(i) << 2);
        if (link) c->r[31] = pc + 8;
        exec_simple(c, pc + 4, d);
        if (!cnd) { pc += 8; continue; }
        if (link) {
            rt_call(c, t);
            if (c->escape) { if (c->escape == ra_entry) c->escape = 0; return; }
            pc += 8;
            continue;
        }
        if (t <= pc) SAFEPOINT();
        pc = t;
    }
}

void cpu_run(u32 pc) {
    rt_call(&g_cpu, pc);
    plog("game main returned\n");
}
