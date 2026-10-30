// recomp: static recompiler for the Team Buddies native port.
// Reads the game's executables straight from the user's disc image (CUE/BIN),
// finds every function it can reach, and translates the MIPS R3000A code to C++.
//
// usage: recomp <game.cue> <outdir> [extra_seeds.txt]

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <fstream>
#include <sstream>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;

// ---------------------------------------------------------------- disc

struct Disc {
    FILE* f = nullptr;

    bool open(const std::string& path) {
        std::string bin = path;
        size_t dot = path.find_last_of('.');
        if (dot != std::string::npos && (path.substr(dot) == ".cue" || path.substr(dot) == ".CUE")) {
            std::ifstream cue(path);
            std::string line;
            bin.clear();
            while (std::getline(cue, line)) {
                size_t a = line.find("FILE \"");
                if (a == std::string::npos) continue;
                size_t b = line.find('"', a + 6);
                std::string name = line.substr(a + 6, b - a - 6);
                size_t slash = path.find_last_of("/\\");
                bin = (slash == std::string::npos ? "" : path.substr(0, slash + 1)) + name;
                break;
            }
            if (bin.empty()) return false;
        }
        f = fopen(bin.c_str(), "rb");
        return f != nullptr;
    }

    bool sector(u32 lba, u8* out) {
        u8 raw[2352];
        if (_fseeki64(f, (long long)lba * 2352, SEEK_SET) != 0) return false;
        if (fread(raw, 1, 2352, f) != 2352) return false;
        memcpy(out, raw + 24, 2048);
        return true;
    }

    bool find(const char* name, u32& lba, u32& size) {
        u8 pvd[2048];
        if (!sector(16, pvd)) return false;
        u32 rlba = *(u32*)(pvd + 156 + 2), rsize = *(u32*)(pvd + 156 + 10);
        std::vector<u8> dir(((rsize + 2047) / 2048) * 2048);
        for (u32 i = 0; i < dir.size() / 2048; i++) sector(rlba + i, &dir[i * 2048]);
        size_t nl = strlen(name);
        for (size_t p = 0; p < rsize;) {
            u8 len = dir[p];
            if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }
            u8 n = dir[p + 32];
            const char* nm = (const char*)&dir[p + 33];
            if (n >= nl && _strnicmp(nm, name, nl) == 0 && (n == nl || nm[nl] == ';')) {
                lba = *(u32*)&dir[p + 2];
                size = *(u32*)&dir[p + 10];
                return true;
            }
            p += len;
        }
        return false;
    }

    std::vector<u8> read(const char* name) {
        u32 lba, size;
        std::vector<u8> out;
        if (!find(name, lba, size)) return out;
        out.resize(((size + 2047) / 2048) * 2048);
        for (u32 i = 0; i < out.size() / 2048; i++) sector(lba + i, &out[i * 2048]);
        out.resize(size);
        return out;
    }
};

// ---------------------------------------------------------------- images

struct Image {
    std::string name, id;
    u32 base = 0, end = 0;
    std::vector<u8> data;
    bool valid(u32 a) const { return a >= base && a < end && !(a & 3); }
    bool has(u32 a) const { return a >= base && a + 4 <= base + data.size(); }
    u32 w(u32 a) const { return has(a) ? *(const u32*)&data[a - base] : 0; }
};

static std::vector<Image> imgs;

static int covering(u32 a) {
    int n = 0;
    for (auto& im : imgs) if (im.valid(a)) n++;
    return n;
}

// ---------------------------------------------------------------- decoding

static inline u32 OP(u32 i) { return i >> 26; }
static inline u32 RS(u32 i) { return (i >> 21) & 31; }
static inline u32 RT(u32 i) { return (i >> 16) & 31; }
static inline u32 RD(u32 i) { return (i >> 11) & 31; }
static inline u32 SA(u32 i) { return (i >> 6) & 31; }
static inline u32 FN(u32 i) { return i & 63; }
static inline u32 IMM(u32 i) { return i & 0xFFFF; }
static inline s32 SIMM(u32 i) { return (s16)(i & 0xFFFF); }
static inline u32 JT(u32 pc, u32 i) { return ((pc + 4) & 0xF0000000) | ((i & 0x3FFFFFF) << 2); }
static inline u32 BT(u32 pc, u32 i) { return pc + 4 + ((u32)SIMM(i) << 2); }

enum Kind { K_INVALID, K_NORMAL, K_BRANCH, K_BRANCH_LINK, K_J, K_JAL, K_JR, K_JALR, K_SYSCALL, K_BREAK };

static Kind classify(u32 i) {
    switch (OP(i)) {
    case 0:
        switch (FN(i)) {
        case 0: case 2: case 3: case 4: case 6: case 7: return K_NORMAL;
        case 8: return K_JR;
        case 9: return K_JALR;
        case 12: return K_SYSCALL;
        case 13: return K_BREAK;
        case 16: case 17: case 18: case 19: case 24: case 25: case 26: case 27: return K_NORMAL;
        case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: case 42: case 43: return K_NORMAL;
        default: return K_INVALID;
        }
    case 1: {
        u32 rt = RT(i);
        if (rt == 0 || rt == 1) return K_BRANCH;
        if (rt == 16 || rt == 17) return K_BRANCH_LINK;
        return K_INVALID;
    }
    case 2: return K_J;
    case 3: return K_JAL;
    case 4: case 5: case 6: case 7: return K_BRANCH;
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: return K_NORMAL;
    case 16:
        if (RS(i) == 0 || RS(i) == 4) return K_NORMAL;
        if (RS(i) == 16 && FN(i) == 16) return K_NORMAL;
        return K_INVALID;
    case 18:
        if (i & (1u << 25)) return K_NORMAL;
        if (RS(i) == 0 || RS(i) == 2 || RS(i) == 4 || RS(i) == 6) return K_NORMAL;
        return K_INVALID;
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: return K_NORMAL;
    case 40: case 41: case 42: case 43: case 46: return K_NORMAL;
    case 50: case 58: return K_NORMAL;
    default: return K_INVALID;
    }
}

static bool is_load(u32 i) { u32 o = OP(i); return (o >= 32 && o <= 38); }

// registers read / written by an instruction (bitmasks over r0..r31)
static u32 reads(u32 i) {
    u32 rs = 1u << RS(i), rt = 1u << RT(i);
    switch (OP(i)) {
    case 0:
        switch (FN(i)) {
        case 0: case 2: case 3: return rt;
        case 4: case 6: case 7: return rs | rt;
        case 8: case 9: return rs;
        case 17: case 19: return rs;
        case 24: case 25: case 26: case 27: return rs | rt;
        case 12: return (1u << 4) | (1u << 2);
        default: return (FN(i) >= 32) ? (rs | rt) : 0;
        }
    case 1: return rs;
    case 4: case 5: return rs | rt;
    case 6: case 7: return rs;
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: return rs;
    case 15: return 0;
    case 16: return RS(i) == 4 ? rt : 0;
    case 18: return (!(i & (1u << 25)) && (RS(i) == 4 || RS(i) == 6)) ? rt : 0;
    case 34: case 38: return rs | rt; // lwl/lwr merge into rt
    case 32: case 33: case 35: case 36: case 37: return rs;
    case 40: case 41: case 42: case 43: case 46: return rs | rt;
    case 50: return rs;
    case 58: return rs;
    }
    return 0;
}

static u32 writes(u32 i) {
    switch (OP(i)) {
    case 0:
        switch (FN(i)) {
        case 0: case 2: case 3: case 4: case 6: case 7: case 9: case 16: case 18: return 1u << RD(i);
        default: return (FN(i) >= 32) ? (1u << RD(i)) : 0;
        }
    case 1: return (RT(i) & 16) ? (1u << 31) : 0;
    case 3: return 1u << 31;
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: return 1u << RT(i);
    case 16: return RS(i) == 0 ? (1u << RT(i)) : 0;
    case 18: return (!(i & (1u << 25)) && (RS(i) == 0 || RS(i) == 2)) ? (1u << RT(i)) : 0;
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: return 1u << RT(i);
    }
    return 0;
}

// ---------------------------------------------------------------- disassembler (for comments)

static const char* RN[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                             "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

static std::string disasm(u32 pc, u32 i) {
    char b[128];
    const char* rs = RN[RS(i)]; const char* rt = RN[RT(i)]; const char* rd = RN[RD(i)];
    static const char* sp[64] = {"sll", 0, "srl", "sra", "sllv", 0, "srlv", "srav", "jr", "jalr", 0, 0, "syscall", "break", 0, 0,
                                 "mfhi", "mthi", "mflo", "mtlo", 0, 0, 0, 0, "mult", "multu", "div", "divu", 0, 0, 0, 0,
                                 "add", "addu", "sub", "subu", "and", "or", "xor", "nor", 0, 0, "slt", "sltu"};
    static const char* ops[64] = {0, 0, "j", "jal", "beq", "bne", "blez", "bgtz", "addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui",
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  "lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", 0, "sb", "sh", "swl", "sw", 0, 0, "swr", 0,
                                  0, 0, "lwc2", 0, 0, 0, 0, 0, 0, 0, "swc2"};
    if (i == 0) return "nop";
    switch (OP(i)) {
    case 0: {
        const char* n = sp[FN(i)];
        if (!n) return "???";
        u32 f = FN(i);
        if (f <= 3) snprintf(b, sizeof b, "%s %s, %s, %u", n, rd, rt, SA(i));
        else if (f <= 7) snprintf(b, sizeof b, "%s %s, %s, %s", n, rd, rt, rs);
        else if (f == 8) snprintf(b, sizeof b, "jr %s", rs);
        else if (f == 9) snprintf(b, sizeof b, "jalr %s, %s", rd, rs);
        else if (f == 12 || f == 13) snprintf(b, sizeof b, "%s 0x%X", n, (i >> 6) & 0xFFFFF);
        else if (f == 16 || f == 18) snprintf(b, sizeof b, "%s %s", n, rd);
        else if (f == 17 || f == 19) snprintf(b, sizeof b, "%s %s", n, rs);
        else if (f >= 24 && f <= 27) snprintf(b, sizeof b, "%s %s, %s", n, rs, rt);
        else snprintf(b, sizeof b, "%s %s, %s, %s", n, rd, rs, rt);
        return b;
    }
    case 1: {
        const char* n = RT(i) == 0 ? "bltz" : RT(i) == 1 ? "bgez" : RT(i) == 16 ? "bltzal" : "bgezal";
        snprintf(b, sizeof b, "%s %s, 0x%08X", n, rs, BT(pc, i));
        return b;
    }
    case 2: case 3: snprintf(b, sizeof b, "%s 0x%08X", ops[OP(i)], JT(pc, i)); return b;
    case 4: case 5: snprintf(b, sizeof b, "%s %s, %s, 0x%08X", ops[OP(i)], rs, rt, BT(pc, i)); return b;
    case 6: case 7: snprintf(b, sizeof b, "%s %s, 0x%08X", ops[OP(i)], rs, BT(pc, i)); return b;
    case 15: snprintf(b, sizeof b, "lui %s, 0x%04X", rt, IMM(i)); return b;
    case 12: case 13: case 14: snprintf(b, sizeof b, "%s %s, %s, 0x%04X", ops[OP(i)], rt, rs, IMM(i)); return b;
    case 8: case 9: case 10: case 11: snprintf(b, sizeof b, "%s %s, %s, %d", ops[OP(i)], rt, rs, SIMM(i)); return b;
    case 16:
        if (RS(i) == 0) snprintf(b, sizeof b, "mfc0 %s, cop0r%u", rt, RD(i));
        else if (RS(i) == 4) snprintf(b, sizeof b, "mtc0 %s, cop0r%u", rt, RD(i));
        else snprintf(b, sizeof b, "rfe");
        return b;
    case 18:
        if (i & (1u << 25)) snprintf(b, sizeof b, "cop2 0x%07X", i & 0x1FFFFFF);
        else snprintf(b, sizeof b, "%s %s, gte%u", RS(i) == 0 ? "mfc2" : RS(i) == 2 ? "cfc2" : RS(i) == 4 ? "mtc2" : "ctc2", rt, RD(i));
        return b;
    case 50: case 58: snprintf(b, sizeof b, "%s gte%u, %d(%s)", ops[OP(i)], RT(i), SIMM(i), rs); return b;
    default:
        if (ops[OP(i)]) { snprintf(b, sizeof b, "%s %s, %d(%s)", ops[OP(i)], rt, SIMM(i), rs); return b; }
        return "???";
    }
}

// ---------------------------------------------------------------- analysis

static std::vector<std::set<u32>> seeds;

static bool is_entry(int ii, u32 t) {
    return seeds[ii].count(t) != 0;
}

// Accept a pointer as a function start only when it looks like one.
static bool plausible_func(int ii, u32 a) {
    const Image& im = imgs[ii];
    if (!im.valid(a)) return false;
    u32 first = im.w(a);
    if (classify(first) == K_INVALID) return false;
    bool ok = (a == im.base);
    if ((first >> 16) == 0x27BD && (s16)(first & 0xFFFF) < 0) ok = true; // addiu sp, sp, -N
    if (!ok && a >= im.base + 8 && im.w(a - 8) == 0x03E00008) ok = true;   // jr ra; <delay>; here
    if (!ok) {
        u32 k = a - 4;
        int n = 0;
        while (k > im.base && im.w(k) == 0 && n < 16) { k -= 4; n++; }
        if (n > 0 && (im.w(k) == 0x03E00008 || im.w(k - 4) == 0x03E00008)) ok = true;
    }
    if (!ok) return false;
    // first basic block must decode
    for (u32 p = a; p < a + 64 && im.valid(p); p += 4) {
        Kind k = classify(im.w(p));
        if (k == K_INVALID) return false;
        if (k != K_NORMAL) return classify(im.w(p + 4)) != K_INVALID;
    }
    return true;
}

struct Func {
    int img = 0;
    u32 entry = 0;
    std::set<u32> visited, dslots, labels, invalid, dynjump, outside;
    std::map<u32, std::vector<u32>> jtab;
    std::map<u32, u32> tail;           // pc of j -> target (tail call)
    std::vector<u32> calls;
    std::vector<u32> addr_consts;      // lui/addiu built addresses
};

// abstract value for jump table detection
struct AV { int k = 0; u32 c = 0; }; // 0 unknown, 1 const, 2 const + index

static bool find_jumptable(const Image& im, u32 jrpc, u32 entry, std::vector<u32>& out) {
    u32 ri = RS(im.w(jrpc));
    u32 start = jrpc >= im.base + 32 * 4 ? jrpc - 32 * 4 : im.base;
    if (start < entry) start = entry;
    AV av[32];
    av[0].k = 1; av[0].c = 0;
    u32 table = 0;
    bool found = false;
    for (u32 pc = start; pc < jrpc; pc += 4) {
        u32 i = im.w(pc);
        u32 w = writes(i) & ~1u;
        if (OP(i) == 15) { av[RT(i)] = {1, IMM(i) << 16}; continue; }
        if (OP(i) == 9) {
            AV s = av[RS(i)]; AV r;
            if (s.k == 1) r = {1, s.c + (u32)SIMM(i)};
            else if (s.k == 2) r = {2, s.c + (u32)SIMM(i)};
            if (RT(i)) av[RT(i)] = r;
            continue;
        }
        if (OP(i) == 13) {
            AV s = av[RS(i)]; AV r;
            if (s.k == 1) r = {1, s.c | IMM(i)};
            if (RT(i)) av[RT(i)] = r;
            continue;
        }
        if (OP(i) == 0 && FN(i) == 33) {
            AV a = av[RS(i)], b = av[RT(i)], r;
            if (a.k == 1 && b.k == 1) r = {1, a.c + b.c};
            else if (a.k == 1 && b.k == 0) r = {2, a.c};
            else if (b.k == 1 && a.k == 0) r = {2, b.c};
            if (RD(i)) av[RD(i)] = r;
            continue;
        }
        if (OP(i) == 35 && RT(i) == ri) {
            AV b = av[RS(i)];
            if (b.k == 2) { table = b.c + (u32)SIMM(i); found = true; }
            else found = false;
            av[RT(i)] = AV();
            continue;
        }
        for (int r = 1; r < 32; r++) if (w & (1u << r)) av[r] = AV();
        if (w & (1u << ri)) found = false;
    }
    if (!found) return false;
    // bound from sltiu
    int count = -1;
    for (u32 pc = jrpc - 4; pc >= start && pc >= im.base; pc -= 4) {
        u32 i = im.w(pc);
        if (OP(i) == 11) { count = (int)IMM(i); break; }
        if (pc == im.base) break;
    }
    const Image* tim = nullptr;
    for (auto& x : imgs) if (&x == &im && x.has(table)) tim = &x;
    if (!tim) for (auto& x : imgs) if (x.has(table)) { tim = &x; break; }
    if (!tim) return false;
    int lim = count > 0 ? count : 1024;
    for (int k = 0; k < lim; k++) {
        u32 t = tim->w(table + k * 4);
        if (!im.valid(t) || t < entry || t > entry + 0x40000 || classify(im.w(t)) == K_INVALID) {
            if (count > 0) return false;
            break;
        }
        out.push_back(t);
    }
    return !out.empty();
}

static void analyze(int ii, u32 entry, Func& f) {
    const Image& im = imgs[ii];
    f.img = ii;
    f.entry = entry;
    std::vector<u32> work{entry};
    auto delay = [&](u32 pc) {
        if (!im.valid(pc + 4)) { f.outside.insert(pc + 4); return; }
        f.dslots.insert(pc + 4);
    };
    while (!work.empty()) {
        u32 pc = work.back();
        work.pop_back();
        for (;;) {
            if (!im.valid(pc)) { f.outside.insert(pc); break; }
            if (f.visited.count(pc)) break;
            f.visited.insert(pc);
            u32 i = im.w(pc);
            Kind k = classify(i);
            if (k == K_INVALID) { f.invalid.insert(pc); break; }
            if (k == K_BRANCH || k == K_BRANCH_LINK) {
                delay(pc);
                u32 t = BT(pc, i);
                if (k == K_BRANCH_LINK) f.calls.push_back(t);
                else if (!im.valid(t)) f.outside.insert(t);
                else { f.labels.insert(t); work.push_back(t); }
                bool always = (OP(i) == 4 && RS(i) == 0 && RT(i) == 0) || (OP(i) == 1 && RS(i) == 0 && (RT(i) & 1));
                if (always && k == K_BRANCH) break;
                pc += 8;
                f.labels.insert(pc);
                continue;
            }
            if (k == K_J) {
                delay(pc);
                u32 t = JT(pc, i);
                if (!im.valid(t) || is_entry(ii, t)) f.tail[pc] = t;
                else { f.labels.insert(t); work.push_back(t); }
                break;
            }
            if (k == K_JAL) {
                delay(pc);
                f.calls.push_back(JT(pc, i));
                pc += 8;
                continue;
            }
            if (k == K_JR) {
                delay(pc);
                if (RS(i) == 31) break;
                u32 prev = im.w(pc - 4);
                if (OP(prev) == 9 && RS(prev) == 0 && RT(prev) == RS(i)) break; // constant target (bios stub)
                std::vector<u32> tg;
                if (find_jumptable(im, pc, entry, tg)) {
                    f.jtab[pc] = tg;
                    for (u32 t : tg) { f.labels.insert(t); work.push_back(t); }
                } else {
                    f.dynjump.insert(pc);
                }
                break;
            }
            if (k == K_JALR) {
                delay(pc);
                pc += 8;
                continue;
            }
            pc += 4;
        }
    }
    // (a branch into a delay slot is handled by the loop above: delay slots are tracked
    //  separately from visited instructions, so the target is walked as normal code)

    // collect lui/addiu address constants
    u32 lui[32] = {0};
    bool have[32] = {false};
    std::vector<u32> order(f.visited.begin(), f.visited.end());
    for (u32 d : f.dslots) order.push_back(d);
    std::sort(order.begin(), order.end());
    order.erase(std::unique(order.begin(), order.end()), order.end());
    for (u32 pc : order) {
        if (f.labels.count(pc)) for (int r = 0; r < 32; r++) have[r] = false;
        u32 i = im.w(pc);
        if (OP(i) == 15) { lui[RT(i)] = IMM(i) << 16; have[RT(i)] = true; continue; }
        if ((OP(i) == 9 || OP(i) == 13) && have[RS(i)]) {
            u32 v = OP(i) == 9 ? lui[RS(i)] + (u32)SIMM(i) : (lui[RS(i)] | IMM(i));
            f.addr_consts.push_back(v);
        }
        u32 w = writes(i);
        for (int r = 0; r < 32; r++) if (w & (1u << r)) have[r] = false;
    }
}

// images a call from image ii to target t may land in
static std::vector<int> resolve(int ii, u32 t) {
    std::vector<int> r;
    if (imgs[ii].valid(t)) { r.push_back(ii); return r; }
    for (int j = 0; j < (int)imgs.size(); j++)
        if (imgs[j].valid(t) && classify(imgs[j].w(t)) != K_INVALID) r.push_back(j);
    return r;
}

// ---------------------------------------------------------------- codegen

static std::string H(u32 v) { char b[16]; snprintf(b, sizeof b, "%08X", v); return b; }
static std::string Rr(u32 n) { return n == 0 ? std::string("0u") : "c->r[" + std::to_string(n) + "]"; }
static std::string Rw(u32 n) { return "c->r[" + std::to_string(n) + "]"; }

static std::string fname(int ii, u32 a) { return "f_" + imgs[ii].id + "_" + H(a); }

static std::string addr_expr(u32 i) {
    s32 s = SIMM(i);
    std::string b = Rr(RS(i));
    if (s == 0) return b;
    char t[32];
    if (s < 0) snprintf(t, sizeof t, " - %du", -s);
    else snprintf(t, sizeof t, " + %du", s);
    return "(" + b + t + ")";
}

static int g_hazards = 0;

// straight-line instruction (no control flow)
static std::string emit_simple(u32 pc, u32 i) {
    char b[256];
    u32 rs = RS(i), rt = RT(i), rd = RD(i), sa = SA(i);
    s32 si = SIMM(i);
    u32 ui = IMM(i);
    auto wr = [&](u32 r, const std::string& e) -> std::string {
        if (r == 0) return "";
        return Rw(r) + " = " + e + ";";
    };
    switch (OP(i)) {
    case 0:
        switch (FN(i)) {
        case 0: if (i == 0) return ""; snprintf(b, sizeof b, "%s << %u", Rr(rt).c_str(), sa); return wr(rd, b);
        case 2: snprintf(b, sizeof b, "%s >> %u", Rr(rt).c_str(), sa); return wr(rd, b);
        case 3: snprintf(b, sizeof b, "(u32)((s32)%s >> %u)", Rr(rt).c_str(), sa); return wr(rd, b);
        case 4: return wr(rd, Rr(rt) + " << (" + Rr(rs) + " & 31)");
        case 6: return wr(rd, Rr(rt) + " >> (" + Rr(rs) + " & 31)");
        case 7: return wr(rd, "(u32)((s32)" + Rr(rt) + " >> (" + Rr(rs) + " & 31))");
        case 16: return wr(rd, "c->hi");
        case 17: return "c->hi = " + Rr(rs) + ";";
        case 18: return wr(rd, "c->lo");
        case 19: return "c->lo = " + Rr(rs) + ";";
        case 24: return "op_mult(c, " + Rr(rs) + ", " + Rr(rt) + ");";
        case 25: return "op_multu(c, " + Rr(rs) + ", " + Rr(rt) + ");";
        case 26: return "op_div(c, " + Rr(rs) + ", " + Rr(rt) + ");";
        case 27: return "op_divu(c, " + Rr(rs) + ", " + Rr(rt) + ");";
        case 32: case 33: return wr(rd, Rr(rs) + " + " + Rr(rt));
        case 34: case 35: return wr(rd, Rr(rs) + " - " + Rr(rt));
        case 36: return wr(rd, Rr(rs) + " & " + Rr(rt));
        case 37: return wr(rd, Rr(rs) + " | " + Rr(rt));
        case 38: return wr(rd, Rr(rs) + " ^ " + Rr(rt));
        case 39: return wr(rd, "~(" + Rr(rs) + " | " + Rr(rt) + ")");
        case 42: return wr(rd, "(u32)((s32)" + Rr(rs) + " < (s32)" + Rr(rt) + ")");
        case 43: return wr(rd, "(u32)(" + Rr(rs) + " < " + Rr(rt) + ")");
        case 12: snprintf(b, sizeof b, "rt_syscall(c, 0x%08Xu);", pc); return b;
        case 13: snprintf(b, sizeof b, "rt_break(c, 0x%08Xu);", pc); return b;
        }
        break;
    case 8: case 9:
        if (si < 0) snprintf(b, sizeof b, "%s - %du", Rr(rs).c_str(), -si);
        else snprintf(b, sizeof b, "%s + %du", Rr(rs).c_str(), si);
        if (rs == 0) snprintf(b, sizeof b, "0x%Xu", (u32)si);
        return wr(rt, b);
    case 10: snprintf(b, sizeof b, "(u32)((s32)%s < %d)", Rr(rs).c_str(), si); return wr(rt, b);
    case 11: snprintf(b, sizeof b, "(u32)(%s < 0x%Xu)", Rr(rs).c_str(), (u32)si); return wr(rt, b);
    case 12: snprintf(b, sizeof b, "%s & 0x%Xu", Rr(rs).c_str(), ui); return wr(rt, b);
    case 13: snprintf(b, sizeof b, "%s | 0x%Xu", Rr(rs).c_str(), ui); if (rs == 0) snprintf(b, sizeof b, "0x%Xu", ui); return wr(rt, b);
    case 14: snprintf(b, sizeof b, "%s ^ 0x%Xu", Rr(rs).c_str(), ui); return wr(rt, b);
    case 15: snprintf(b, sizeof b, "0x%08Xu", ui << 16); return wr(rt, b);
    case 16:
        if (rs == 0) return wr(rt, "cop0_read(c, " + std::to_string(rd) + ")");
        if (rs == 4) return "cop0_write(c, " + std::to_string(rd) + ", " + Rr(rt) + ");";
        return "cop0_rfe(c);";
    case 18:
        if (i & (1u << 25)) { snprintf(b, sizeof b, "gte_command(0x%07Xu);", i & 0x1FFFFFF); return b; }
        if (rs == 0) { if (!rt) return "(void)gte_read_data(" + std::to_string(rd) + ");"; return wr(rt, "gte_read_data(" + std::to_string(rd) + ")"); }
        if (rs == 2) { if (!rt) return ""; return wr(rt, "gte_read_ctrl(" + std::to_string(rd) + ")"); }
        if (rs == 4) return "gte_write_data(" + std::to_string(rd) + ", " + Rr(rt) + ");";
        if (rs == 6) return "gte_write_ctrl(" + std::to_string(rd) + ", " + Rr(rt) + ");";
        break;
    case 32: return rt ? wr(rt, "(u32)(s32)(s8)mem_read8(" + addr_expr(i) + ")") : "(void)mem_read8(" + addr_expr(i) + ");";
    case 33: return rt ? wr(rt, "(u32)(s32)(s16)mem_read16(" + addr_expr(i) + ")") : "(void)mem_read16(" + addr_expr(i) + ");";
    case 34: return rt ? wr(rt, "op_lwl(" + Rr(rt) + ", " + addr_expr(i) + ")") : "";
    case 35: return rt ? wr(rt, "mem_read32(" + addr_expr(i) + ")") : "(void)mem_read32(" + addr_expr(i) + ");";
    case 36: return rt ? wr(rt, "(u32)mem_read8(" + addr_expr(i) + ")") : "(void)mem_read8(" + addr_expr(i) + ");";
    case 37: return rt ? wr(rt, "(u32)mem_read16(" + addr_expr(i) + ")") : "(void)mem_read16(" + addr_expr(i) + ");";
    case 38: return rt ? wr(rt, "op_lwr(" + Rr(rt) + ", " + addr_expr(i) + ")") : "";
    case 40: return "mem_write8(" + addr_expr(i) + ", (u8)" + Rr(rt) + ");";
    case 41: return "mem_write16(" + addr_expr(i) + ", (u16)" + Rr(rt) + ");";
    case 42: return "op_swl(" + addr_expr(i) + ", " + Rr(rt) + ");";
    case 43: return "mem_write32(" + addr_expr(i) + ", " + Rr(rt) + ");";
    case 46: return "op_swr(" + addr_expr(i) + ", " + Rr(rt) + ");";
    case 50: return "gte_write_data(" + std::to_string(rt) + ", mem_read32(" + addr_expr(i) + "));";
    case 58: return "mem_write32(" + addr_expr(i) + ", gte_read_data(" + std::to_string(rt) + "));";
    }
    snprintf(b, sizeof b, "rt_invalid(c, 0x%08Xu);", pc);
    return b;
}

static std::string branch_cond(u32 i) {
    u32 rs = RS(i), rt = RT(i);
    switch (OP(i)) {
    case 4: return Rr(rs) + " == " + Rr(rt);
    case 5: return Rr(rs) + " != " + Rr(rt);
    case 6: return "(s32)" + Rr(rs) + " <= 0";
    case 7: return "(s32)" + Rr(rs) + " > 0";
    case 1: return (RT(i) & 1) ? "(s32)" + Rr(rs) + " >= 0" : "(s32)" + Rr(rs) + " < 0";
    }
    return "false";
}

struct Emitter {
    std::ostringstream o;
    int ii;
    const Func* f;

    std::string call_to(u32 t) {
        if (t == 0xA0 || t == 0xB0 || t == 0xC0) return "rt_bios(c, 0x" + H(t) + "u);";
        std::vector<int> r = resolve(ii, t);
        if (r.size() == 1 && (r[0] == ii || covering(t) == 1) && seeds[r[0]].count(t))
            return fname(r[0], t) + "(c);";
        return "rt_call(c, 0x" + H(t) + "u);";
    }

    void line(const std::string& s, int ind = 1) {
        if (s.empty()) return;
        for (int k = 0; k < ind; k++) o << "    ";
        o << s << "\n";
    }

    void comment(u32 pc, u32 i, int ind = 1) {
        for (int k = 0; k < ind; k++) o << "    ";
        o << "// " << H(pc) << ": " << disasm(pc, i) << "\n";
    }

    std::string delay_code(u32 pc) {
        const Image& im = imgs[ii];
        if (!im.valid(pc + 4)) return "rt_invalid(c, 0x" + H(pc + 4) + "u);";
        u32 d = im.w(pc + 4);
        Kind k = classify(d);
        if (k != K_NORMAL && k != K_SYSCALL && k != K_BREAK) return "rt_invalid(c, 0x" + H(pc + 4) + "u); /* control flow in delay slot */";
        return emit_simple(pc + 4, d);
    }

    void emit_func() {
        const Image& im = imgs[ii];
        std::set<u32> all(f->visited.begin(), f->visited.end());
        for (u32 d : f->dslots) all.insert(d);
        std::set<u32> labels = f->labels;
        // labels needed after delay slots reached as entries
        for (u32 a : all) if (f->dslots.count(a) && f->visited.count(a)) labels.insert(a + 4);
        std::vector<u32> order(all.begin(), all.end());

        bool need_ra = !f->dynjump.empty() || !f->jtab.empty();
        // Hand-written asm sometimes branches into its caller's epilogue ("lw ra, n(sp) ... jr ra"),
        // returning two levels at once. A function that restores ra without ever saving it is such
        // a helper: its jr ra flags a non-local return in c->escape, and every call site unwinds
        // until the level whose own return address matches.
        bool lw_ra = false, sw_ra = false;
        for (u32 a : all) { // includes delay slots ("bne ...; sw ra, n(sp)")
            u32 w = im.w(a) & 0xFFFF0000u;
            if (w == 0x8FBF0000u) lw_ra = true;
            if (w == 0xAFBF0000u) sw_ra = true;
        }
        const bool escapes = lw_ra && !sw_ra;
        const std::string unwind = "if (c->escape) { if (c->escape == ra_entry) c->escape = 0; return; }";
        o << "void " << fname(ii, f->entry) << "(CPU* c) {\n";
        o << "    const u32 ra_entry = c->r[31]; (void)ra_entry;\n";
        o << "    SAFEPOINT();\n";
        if (f->entry != order.front()) o << "    goto L_" << H(f->entry) << ";\n";

        u32 fall = f->entry == order.front() ? f->entry : 0; // address control falls into next
        bool block_start = true;
        for (size_t k = 0; k < order.size(); k++) {
            u32 a = order[k];
            bool dslot_only = f->dslots.count(a) && !f->visited.count(a);
            if (dslot_only) continue;
            bool is_label = labels.count(a) || a == f->entry;
            if (f->dslots.count(a)) {
                // this delay slot is also an entry point; skip it on the fallthrough path
                if (fall) { o << "    goto L_" << H(a + 4) << ";\n"; }
                fall = 0;
                is_label = true;
            }
            if (fall && fall != a) { o << "    goto L_" << H(fall) << ";\n"; labels.insert(fall); }
            if (is_label) { o << "L_" << H(a) << ":;\n"; block_start = true; }
            if (block_start) {
                // count instructions to the end of this block
                int n = 0;
                for (size_t q = k; q < order.size(); q++) {
                    u32 b = order[q];
                    if (q > k && (labels.count(b) || (b != order[q - 1] + 4))) break;
                    if (f->dslots.count(b) && !f->visited.count(b)) { n++; continue; }
                    n++;
                    Kind kk = classify(im.w(b));
                    if (kk != K_NORMAL && kk != K_SYSCALL && kk != K_BREAK) { n++; break; }
                }
                o << "    CYCLES(" << n << ");\n";
                block_start = false;
            }
            u32 i = im.w(a);
            Kind kind = classify(i);
            comment(a, i);
            fall = a + 4;
            switch (kind) {
            case K_INVALID:
                line("rt_invalid(c, 0x" + H(a) + "u); return;");
                fall = 0;
                break;
            case K_NORMAL: case K_SYSCALL: case K_BREAK: {
                // load delay hazard: next instruction reads the loaded register
                u32 nx = im.w(a + 4);
                bool next_inline = all.count(a + 4) && f->visited.count(a + 4) && !labels.count(a + 4) && !f->dslots.count(a + 4);
                u32 lr = (is_load(i) && RT(i)) ? RT(i) : 0;
                if (lr && next_inline && classify(nx) == K_NORMAL && (reads(nx) & (1u << lr)) &&
                    OP(nx) != 34 && OP(nx) != 38) {
                    g_hazards++;
                    std::string ld = emit_simple(a, i);
                    // rewrite "c->r[N] = expr;" into a temp
                    std::string pre = Rw(lr) + " = ";
                    std::string expr = ld.substr(pre.size(), ld.size() - pre.size() - 1);
                    line("{ const u32 ld = " + expr + "; /* load delay */");
                    comment(a + 4, nx, 2);
                    line(emit_simple(a + 4, nx), 2);
                    if (!(writes(nx) & (1u << lr))) line(Rw(lr) + " = ld;", 2);
                    line("}");
                    k++; // consumed next
                    fall = a + 8;
                    break;
                }
                line(emit_simple(a, i));
                break;
            }
            case K_BRANCH: case K_BRANCH_LINK: {
                u32 t = BT(a, i);
                bool always = (OP(i) == 4 && RS(i) == 0 && RT(i) == 0) || (OP(i) == 1 && RS(i) == 0 && (RT(i) & 1));
                comment(a + 4, im.w(a + 4), 2);
                if (kind == K_BRANCH_LINK) {
                    line("{ const bool cnd = " + branch_cond(i) + ";");
                    line("c->r[31] = 0x" + H(a + 8) + "u;", 2);
                    line(delay_code(a), 2);
                    line("if (cnd) { " + call_to(t) + " " + unwind + " }", 2);
                    line("}");
                    fall = a + 8;
                    break;
                }
                std::string jump;
                if (!im.valid(t)) jump = "{ rt_jump(c, 0x" + H(t) + "u, " + (need_ra ? "ra_entry" : "c->r[31]") + "); return; }";
                else if (t <= a) jump = "{ SAFEPOINT(); goto L_" + H(t) + "; }";
                else jump = "goto L_" + H(t) + ";";
                if (always) {
                    line(delay_code(a));
                    line(jump);
                    fall = 0;
                } else {
                    line("{ const bool cnd = " + branch_cond(i) + ";");
                    line(delay_code(a), 2);
                    line("if (cnd) " + jump, 2);
                    line("}");
                    fall = a + 8;
                }
                block_start = true;
                break;
            }
            case K_J: {
                u32 t = JT(a, i);
                comment(a + 4, im.w(a + 4), 2);
                line(delay_code(a));
                auto it = f->tail.find(a);
                if (it != f->tail.end()) { line(call_to(t)); line("return;"); }
                else if (t <= a) line("SAFEPOINT(); goto L_" + H(t) + ";");
                else line("goto L_" + H(t) + ";");
                fall = 0;
                break;
            }
            case K_JAL: {
                u32 t = JT(a, i);
                comment(a + 4, im.w(a + 4), 2);
                line("c->r[31] = 0x" + H(a + 8) + "u;");
                line(delay_code(a));
                line(call_to(t));
                line(unwind);
                fall = a + 8;
                block_start = true;
                break;
            }
            case K_JR: {
                comment(a + 4, im.w(a + 4), 2);
                u32 rs = RS(i);
                u32 prev = im.w(a - 4);
                if (rs == 31 && escapes) {
                    line("{ const u32 t = c->r[31];");
                    line(delay_code(a), 2);
                    line("if (t != ra_entry) c->escape = t;", 2);
                    line("return; }", 2);
                } else if (rs == 31) {
                    line(delay_code(a));
                    line("return;");
                } else if (OP(prev) == 9 && RS(prev) == 0 && RT(prev) == rs) {
                    u32 t = (u32)SIMM(prev);
                    line(delay_code(a));
                    line(call_to(t));
                    line("return;");
                } else if (f->jtab.count(a)) {
                    line("{ const u32 t = " + Rr(rs) + ";");
                    line(delay_code(a), 2);
                    line("switch (t) {", 2);
                    std::set<u32> uniq(f->jtab.at(a).begin(), f->jtab.at(a).end());
                    for (u32 t : uniq) line("case 0x" + H(t) + "u: goto L_" + H(t) + ";", 2);
                    line("default: rt_jump(c, t, ra_entry); return;", 2);
                    line("} }");
                } else {
                    line("{ const u32 t = " + Rr(rs) + ";");
                    line(delay_code(a), 2);
                    line("rt_jump(c, t, ra_entry); return; }", 2);
                }
                fall = 0;
                break;
            }
            case K_JALR: {
                comment(a + 4, im.w(a + 4), 2);
                line("{ const u32 t = " + Rr(RS(i)) + ";");
                if (RD(i)) line(Rw(RD(i)) + " = 0x" + H(a + 8) + "u;", 2);
                line(delay_code(a), 2);
                line("rt_call(c, t); }", 2);
                line(unwind);
                fall = a + 8;
                block_start = true;
                break;
            }
            }
            if (kind == K_BRANCH || kind == K_BRANCH_LINK || kind == K_J || kind == K_JAL || kind == K_JR || kind == K_JALR) {
                // delay slot consumed inline; skip it in order unless it's also an entry
                if (k + 1 < order.size() && order[k + 1] == a + 4 && !f->visited.count(a + 4)) k++;
            }
        }
        if (fall) {
            if (all.count(fall)) o << "    goto L_" << H(fall) << ";\n";
            else o << "    rt_jump(c, 0x" << H(fall) << "u, " << (need_ra ? "ra_entry" : "c->r[31]") << "); return;\n";
        }
        o << "}\n\n";
    }
};

// ---------------------------------------------------------------- main

static void add_seed(int ii, u32 a, std::vector<std::pair<int, u32>>& work) {
    if (!imgs[ii].valid(a)) return;
    if (seeds[ii].insert(a).second) work.push_back({ii, a});
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: recomp <game.cue> <outdir> [seeds.txt ...]\n"); return 1; }
    Disc disc;
    if (!disc.open(argv[1])) { fprintf(stderr, "cannot open disc %s\n", argv[1]); return 1; }
    std::string out = argv[2];

    // boot executable from SYSTEM.CNF
    std::vector<u8> cnf = disc.read("SYSTEM.CNF");
    std::string cs(cnf.begin(), cnf.end());
    size_t bp = cs.find("cdrom:");
    if (bp == std::string::npos) { fprintf(stderr, "bad SYSTEM.CNF\n"); return 1; }
    size_t bs = cs.find_first_not_of("\\", bp + 6);
    size_t be = cs.find_first_of(";\r\n", bs);
    std::string boot = cs.substr(bs, be - bs);
    std::vector<u8> exe = disc.read(boot.c_str());
    if (exe.size() < 0x800 || memcmp(exe.data(), "PS-X EXE", 8) != 0) { fprintf(stderr, "bad exe %s\n", boot.c_str()); return 1; }
    u32 pc0 = *(u32*)&exe[0x10];
    {
        Image im;
        im.name = boot; im.id = "EXE";
        im.base = *(u32*)&exe[0x18];
        im.data.assign(exe.begin() + 0x800, exe.end());
        im.end = im.base + (u32)im.data.size();
        imgs.push_back(im);
    }

    // overlays (Team Buddies OVERLAY.DAT: 0x30-byte entries, then a function table)
    std::vector<u8> ovl = disc.read("OVERLAY.DAT");
    std::vector<u32> ovl_funcs;
    for (size_t p = 0; p + 0x30 <= 0x180 && p + 0x30 <= ovl.size(); p += 0x30) {
        std::string nm((const char*)&ovl[p], strnlen((const char*)&ovl[p], 16));
        if (nm.empty()) break;
        size_t s = nm.find_first_not_of('\\');
        nm = nm.substr(s, nm.find(';') - s);
        u32 load = *(u32*)&ovl[p + 0x20], size = *(u32*)&ovl[p + 0x24];
        std::vector<u8> d = disc.read(nm.c_str());
        if (d.empty()) { printf("overlay %s not on disc, skipped\n", nm.c_str()); continue; }
        Image im;
        im.name = nm;
        im.id = nm.substr(0, nm.find('.'));
        im.base = load;
        im.data = d;
        im.end = load + std::min<u32>(size, (u32)d.size());
        imgs.push_back(im);
        printf("overlay %-9s %08X-%08X\n", nm.c_str(), im.base, im.end);
    }
    for (size_t p = 0x180; p + 4 <= ovl.size(); p += 4) {
        u32 v = *(u32*)&ovl[p];
        if (!v) break;
        ovl_funcs.push_back(v);
    }
    // the exe's tail is overwritten by the first overlay
    for (size_t j = 1; j < imgs.size(); j++)
        if (imgs[j].base > imgs[0].base && imgs[j].base < imgs[0].end) imgs[0].end = imgs[j].base;
    printf("exe %s %08X-%08X entry %08X\n", boot.c_str(), imgs[0].base, imgs[0].end, pc0);

    // debugging aid: recomp <cue> --dis IMAGE ADDR [COUNT]
    if (argc >= 5 && std::string(argv[2]) == "--dis") {
        u32 a = (u32)strtoul(argv[4], nullptr, 16);
        int n = argc > 5 ? atoi(argv[5]) : 64;
        for (auto& im : imgs) {
            if (im.id != argv[3]) continue;
            for (int k = 0; k < n; k++, a += 4) printf("%08X: %08X  %s\n", a, im.w(a), disasm(a, im.w(a)).c_str());
        }
        return 0;
    }

    seeds.resize(imgs.size());
    std::vector<std::pair<int, u32>> work;
    add_seed(0, pc0, work);
    for (u32 a : ovl_funcs)
        for (int j = 0; j < (int)imgs.size(); j++) if (imgs[j].valid(a) && plausible_func(j, a)) add_seed(j, a, work);

    // function pointers stored as data
    for (int ii = 0; ii < (int)imgs.size(); ii++) {
        const Image& im = imgs[ii];
        for (u32 a = im.base; a + 4 <= im.base + im.data.size(); a += 4) {
            u32 v = im.w(a);
            if (v & 3) continue;
            if (im.valid(v)) { if (plausible_func(ii, v)) add_seed(ii, v, work); continue; }
            for (int j = 0; j < (int)imgs.size(); j++) if (j != ii && imgs[j].valid(v) && plausible_func(j, v)) add_seed(j, v, work);
        }
    }
    // extra seeds discovered at runtime
    for (int ai = 3; ai < argc; ai++) {
        std::ifstream es(argv[ai]);
        std::string id, a;
        while (es >> id >> a) {
            u32 v = (u32)strtoul(a.c_str(), nullptr, 16);
            for (int j = 0; j < (int)imgs.size(); j++)
                if ((imgs[j].id == id || id == "*") && classify(imgs[j].w(v)) != K_INVALID) add_seed(j, v, work);
        }
    }
    size_t initial = 0;
    for (auto& s : seeds) initial += s.size();
    printf("initial seeds: %zu\n", initial);

    // discover functions until nothing new turns up
    while (!work.empty()) {
        auto [ii, a] = work.back();
        work.pop_back();
        Func f;
        analyze(ii, a, f);
        for (u32 t : f.calls)
            for (int j : resolve(ii, t)) add_seed(j, t, work);
        for (auto& kv : f.tail)
            for (int j : resolve(ii, kv.second)) if (j != ii || !imgs[ii].valid(kv.second)) add_seed(j, kv.second, work);
        for (u32 v : f.addr_consts) {
            if (v & 3) continue;
            if (imgs[ii].valid(v)) { if (plausible_func(ii, v)) add_seed(ii, v, work); continue; }
            for (int j = 0; j < (int)imgs.size(); j++) if (imgs[j].valid(v) && plausible_func(j, v)) add_seed(j, v, work);
        }
    }

    // final pass and emission
    std::string hdr = out + "/rec_funcs.h";
    std::ofstream fh(hdr);
    fh << "// generated by recomp - do not edit\n#pragma once\n#include \"rec.h\"\n\n";
    std::ostringstream table;
    table << "// generated by recomp - do not edit\n#include \"rec_funcs.h\"\n\n";
    std::ostringstream listing;
    int nfuncs = 0, njt = 0, ndyn = 0, ninv = 0;
    std::vector<std::string> files;

    struct Entry { u32 addr; int img; std::string fn; u32 s0, s1; };
    std::vector<Entry> entries;

    for (int ii = 0; ii < (int)imgs.size(); ii++) {
        int chunk = 0;
        std::ostringstream body;
        size_t lines = 0;
        auto flush = [&](bool force) {
            if (body.tellp() == 0 || (!force && lines < 60000)) return;
            std::string fnm = "rec_" + imgs[ii].id + "_" + std::to_string(chunk++) + ".cpp";
            std::ofstream fo(out + "/" + fnm);
            fo << "// generated by recomp - do not edit\n#include \"rec_funcs.h\"\n#pragma warning(disable: 4102 4065)\n\n" << body.str();
            files.push_back(fnm);
            body.str(""); body.clear();
            lines = 0;
        };
        for (u32 a : seeds[ii]) {
            Func f;
            analyze(ii, a, f);
            Emitter e;
            e.ii = ii;
            e.f = &f;
            e.emit_func();
            std::string s = e.o.str();
            body << s;
            lines += std::count(s.begin(), s.end(), '\n');
            fh << "void " << fname(ii, a) << "(CPU* c);\n";
            entries.push_back({a, ii, fname(ii, a), imgs[ii].w(a), imgs[ii].w(a + 4)});
            nfuncs++;
            njt += (int)f.jtab.size();
            ndyn += (int)f.dynjump.size();
            ninv += (int)f.invalid.size();
            u32 hi = *f.visited.rbegin();
            listing << imgs[ii].id << " " << H(a) << " " << H(hi + 4) << " insns=" << f.visited.size()
                    << (f.dynjump.empty() ? "" : " dynjump") << (f.invalid.empty() ? "" : " invalid") << "\n";
            flush(false);
        }
        flush(true);
    }

    // dispatch table
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.addr != b.addr ? a.addr < b.addr : a.img < b.img; });
    table << "const RecImage g_rec_images[] = {\n";
    for (int ii = 0; ii < (int)imgs.size(); ii++) {
        // checkpoints: words that tell this image apart from images loaded at the same place
        std::vector<u32> chk;
        bool shared = false;
        for (int j = 0; j < (int)imgs.size(); j++)
            if (j != ii && imgs[j].base < imgs[ii].end && imgs[ii].base < imgs[j].end) shared = true;
        if (shared) {
            for (u32 a : seeds[ii]) {
                bool distinct = true;
                for (int j = 0; j < (int)imgs.size(); j++) {
                    if (j == ii || !imgs[j].has(a)) continue;
                    if (imgs[j].w(a) == imgs[ii].w(a)) { distinct = false; break; }
                }
                if (distinct) chk.push_back(a);
            }
            // spread out
            std::vector<u32> pick;
            for (size_t k = 0; k < 8 && !chk.empty(); k++) pick.push_back(chk[k * chk.size() / 8]);
            pick.erase(std::unique(pick.begin(), pick.end()), pick.end());
            chk = pick;
        }
        table << "    { \"" << imgs[ii].name << "\", 0x" << H(imgs[ii].base) << "u, 0x" << H(imgs[ii].end) << "u, " << chk.size() << ", {";
        for (u32 a : chk) table << " {0x" << H(a) << "u, 0x" << H(imgs[ii].w(a)) << "u},";
        table << " } },\n";
    }
    table << "};\nconst int g_rec_image_count = " << imgs.size() << ";\n\n";
    table << "const RecFunc g_rec_funcs[] = {\n";
    for (auto& e : entries)
        table << "    { 0x" << H(e.addr) << "u, " << e.img << ", " << e.fn << ", 0x" << H(e.s0) << "u, 0x" << H(e.s1) << "u },\n";
    table << "};\nconst int g_rec_func_count = " << entries.size() << ";\n";
    std::ofstream(out + "/rec_table.cpp") << table.str();
    std::ofstream(out + "/functions.txt") << listing.str();
    {
        std::ofstream lst(out + "/sources.txt");
        for (auto& s : files) lst << s << "\n";
        lst << "rec_table.cpp\n";
    }
    printf("functions: %d  jump tables: %d  dynamic jumps: %d  invalid paths: %d  load hazards: %d\n",
           nfuncs, njt, ndyn, ninv, g_hazards);
    for (int ii = 0; ii < (int)imgs.size(); ii++) printf("  %-6s %zu functions\n", imgs[ii].id.c_str(), seeds[ii].size());
    return 0;
}
