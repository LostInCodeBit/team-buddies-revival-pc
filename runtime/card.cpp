// card.cpp - memory cards. The game sees two standard 128 KB cards; on disk every save is its own file:
//   savedata\SLUS-00869\<PS1 file name>.bin          card 1: the save's raw 8 KB blocks
//   savedata\SLUS-00869\card2\<PS1 file name>.bin    card 2
//   ...\backup\<PS1 file name>.bin                   the previous version of a save the game overwrote or deleted
// The card image is assembled from those files at start. Game writes go to the image; once the game has
// stopped writing for half a second (and at exit) the changed saves are written back (temp file + rename).
// An old memcards\cardN.mcd is split into files once and kept as cardN.mcd.migrated.
#include "psx.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include "files.h"

static const char* const kGameCode = "SLUS-00869";
static const int FRAME = 128, BLOCK = 8192, CARD_SIZE = 128 * 1024, SYNC_DELAY = 30; // vblanks

struct Slot {
    std::vector<u8> img;
    std::string dir;              // this card's save folder; empty = memory only (online client)
    std::string legacy;           // an .mcd that could not be split: written through as before
    std::set<std::string> files;  // saves known to be on disk (only these may be moved to backup)
    int dirty = 0;                // vblanks since the last game write, 0 = nothing pending
};
static Slot s_slot[2];

static void checksum(u8* f) {
    u8 x = 0;
    for (int i = 0; i < 127; i++) x ^= f[i];
    f[127] = x;
}

static void format(std::vector<u8>& c) {
    c.assign(CARD_SIZE, 0);
    c[0] = 'M'; c[1] = 'C';
    c[127] = 0x0E;
    for (int n = 1; n < 16; n++) {
        u8* f = &c[n * FRAME];
        f[0] = 0xA0;
        f[8] = f[9] = 0xFF;
        checksum(f);
    }
    for (int n = 16; n < 36; n++) {
        u8* f = &c[n * FRAME];
        f[0] = f[1] = f[2] = f[3] = 0xFF;
        f[8] = f[9] = 0xFF;
        checksum(f);
    }
    // write test frame copies the header
    memcpy(&c[63 * FRAME], &c[0], FRAME);
}

// ---------------------------------------------------------------- saves <-> card image
struct Save { std::string name; std::vector<u8> data; };

static u16 rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }

// the saves in a card image's directory; names whose block chain is damaged go to `bad` (left alone on disk)
static bool parse(const std::vector<u8>& img, std::vector<Save>& out, std::set<std::string>& bad) {
    if (img.size() != CARD_SIZE || img[0] != 'M' || img[1] != 'C') return false;
    for (int i = 1; i < 16; i++) {
        const u8* f = &img[i * FRAME];
        if (f[0] != 0x51) continue; // first block of a save
        std::string name((const char*)f + 0x0A, strnlen((const char*)f + 0x0A, 20));
        if (name.empty()) continue;
        std::vector<int> blocks;
        int b = i;
        bool ok = true;
        u32 seen = 0;
        for (;;) {
            if (b < 1 || b > 15 || (seen & (1u << b))) { ok = false; break; }
            seen |= 1u << b;
            blocks.push_back(b);
            u16 next = rd16(&img[b * FRAME + 8]);
            if (next == 0xFFFF) break;
            b = next + 1;
        }
        if (!ok) { bad.insert(name); continue; }
        Save s;
        s.name = name;
        for (int k : blocks) s.data.insert(s.data.end(), img.begin() + k * BLOCK, img.begin() + (k + 1) * BLOCK);
        out.push_back(std::move(s));
    }
    return true;
}

// lays saves out on a freshly formatted card in the given order; returns how many fitted
static size_t build(std::vector<u8>& img, const std::vector<Save>& saves) {
    format(img);
    int nextb = 1;
    size_t placed = 0;
    for (const Save& s : saves) {
        int n = (int)(s.data.size() / BLOCK);
        if (n < 1 || nextb + n > 16) continue;
        for (int k = 0; k < n; k++) {
            int b = nextb + k;
            u8* f = &img[b * FRAME];
            memset(f, 0, FRAME);
            f[0] = k == 0 ? 0x51 : k == n - 1 ? 0x53 : 0x52; // first, middle, last block
            if (k == 0) {
                u32 size = (u32)s.data.size();
                memcpy(f + 4, &size, 4);
                memcpy(f + 0x0A, s.name.data(), std::min<size_t>(s.name.size(), 20));
            }
            u16 next = k == n - 1 ? 0xFFFF : (u16)b; // the next block, numbered from 0
            f[8] = (u8)next;
            f[9] = (u8)(next >> 8);
            checksum(f);
            memcpy(&img[b * BLOCK], &s.data[k * BLOCK], BLOCK);
        }
        nextb += n;
        placed++;
    }
    return placed;
}

// ---------------------------------------------------------------- files
// PS1 file names are ASCII; characters Windows does not allow in names are written as %XX
static std::string encode(const std::string& n) {
    std::string o;
    for (size_t i = 0; i < n.size(); i++) {
        unsigned char c = (unsigned char)n[i];
        bool tail = i + 1 == n.size() && (c == '.' || c == ' ');
        if (c < 0x20 || c >= 0x7F || strchr("<>:\"/\\|?*%", c) || tail) {
            char h[4];
            snprintf(h, sizeof h, "%%%02X", c);
            o += h;
        } else o += (char)c;
    }
    return o;
}

static std::string decode(const std::string& n) {
    std::string o;
    for (size_t i = 0; i < n.size(); i++) {
        if (n[i] == '%' && i + 2 < n.size() && isxdigit((unsigned char)n[i + 1]) && isxdigit((unsigned char)n[i + 2])) {
            o += (char)strtol(n.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += n[i];
    }
    return o;
}

static bool read_file(const std::string& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static bool write_atomic(const std::string& p, const std::vector<u8>& data) {
    std::string tmp = p + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write((const char*)data.data(), data.size());
        f.flush();
        if (!f) { f.close(); file_delete(tmp); return false; }
    }
    return file_move(tmp, p, MOVE_DURABLE);
}

static void to_backup(const std::string& dir, const std::string& file) {
    make_dirs(dir + PATH_SEP "backup");
    file_move(dir + PATH_SEP + file, dir + PATH_SEP "backup" PATH_SEP + file, MOVE_ANY_VOLUME);
}

// card image -> files: changed saves rewritten, saves the game deleted moved to backup
static void sync(int s) {
    Slot& sl = s_slot[s];
    sl.dirty = 0;
    if (sl.dir.empty()) return;
    std::vector<Save> saves;
    std::set<std::string> bad;
    if (!parse(sl.img, saves, bad)) { log_once("card-unreadable", "card %d: directory unreadable, saves not written\n", s + 1); return; }
    make_dirs(sl.dir);
    std::set<std::string> now;
    for (const Save& v : saves) {
        std::string file = encode(v.name) + ".bin", path = sl.dir + PATH_SEP + file;
        now.insert(v.name);
        std::vector<u8> old;
        bool had = read_file(path, old);
        if (had && old == v.data) continue;
        if (had) {
            // keep the previous version: new data goes to a temp file first, then the swap
            std::string tmp = path + ".new";
            if (!write_atomic(tmp, v.data)) { plog("card %d: cannot write %s\n", s + 1, path.c_str()); continue; }
            to_backup(sl.dir, file);
            file_move(tmp, path, MOVE_DURABLE);
        } else if (!write_atomic(path, v.data)) {
            plog("card %d: cannot write %s\n", s + 1, path.c_str());
            continue;
        }
        plog("card %d: saved %s (%d blocks)\n", s + 1, v.name.c_str(), (int)(v.data.size() / BLOCK));
    }
    for (const std::string& n : sl.files)
        if (!now.count(n) && !bad.count(n)) {
            to_backup(sl.dir, encode(n) + ".bin");
            plog("card %d: %s deleted by the game (moved to backup)\n", s + 1, n.c_str());
        }
    now.insert(bad.begin(), bad.end());
    sl.files = now;
}

// files -> card image (sorted by name, so the layout is the same every run)
static void load(int s) {
    Slot& sl = s_slot[s];
    std::vector<Save> saves;
    for (const std::string& file : list_files(sl.dir, ".bin")) {
        std::string name = decode(file.substr(0, file.size() - 4));
        Save v;
        v.name = name;
        if (name.empty() || name.size() > 20 || !read_file(sl.dir + PATH_SEP + file, v.data)) continue;
        if (v.data.empty() || v.data.size() % BLOCK || v.data.size() > 15u * BLOCK) {
            plog("card %d: %s is not a save (size %u), ignored\n", s + 1, file.c_str(), (unsigned)v.data.size());
            continue;
        }
        saves.push_back(std::move(v));
    }
    std::sort(saves.begin(), saves.end(), [](const Save& a, const Save& b) { return a.name < b.name; });
    size_t placed = build(sl.img, saves);
    sl.files.clear();
    // only saves that made it onto the card can be treated as deleted later
    std::vector<Save> on;
    std::set<std::string> bad;
    parse(sl.img, on, bad);
    for (const Save& v : on) sl.files.insert(v.name);
    if (placed < saves.size()) plog("card %d: %d saves did not fit on the 15-block card and stay on disk only\n", s + 1, (int)(saves.size() - placed));
}

// old cardN.mcd -> one file per save (this game's into the card folder, other games' into savedata\<code>)
static bool migrate(int s, const std::string& root, const std::string& mcd) {
    std::vector<u8> img;
    if (!read_file(mcd, img) || img.size() != CARD_SIZE) return false;
    std::vector<Save> saves;
    std::set<std::string> bad;
    if (!parse(img, saves, bad)) return false;
    for (const Save& v : saves) {
        std::string code = v.name.size() >= 12 ? v.name.substr(2, 10) : "unknown";
        std::string dir = root + PATH_SEP + encode(code) + (s ? PATH_SEP "card2" : "");
        make_dirs(dir);
        std::string path = dir + PATH_SEP + encode(v.name) + ".bin";
        if (path_exists(path)) continue; // never overwrite a save already split out
        if (!write_atomic(path, v.data)) return false;
    }
    file_move(mcd, mcd + ".migrated");
    plog("card %d: %s split into %d save files under %s\n", s + 1, mcd.c_str(), (int)saves.size(), root.c_str());
    return true;
}

void card_init(const std::string& root, const std::string& legacy_dir) {
    for (int s = 0; s < 2; s++) {
        Slot& sl = s_slot[s];
        sl = Slot();
        sl.dir = root + PATH_SEP + kGameCode + (s ? PATH_SEP "card2" : "");
        std::string mcd = legacy_dir + PATH_SEP "card" + std::to_string(s + 1) + ".mcd";
        if (path_exists(mcd) && !migrate(s, root, mcd)) {
            // unreadable old card: keep using it exactly as before rather than lose anything
            if (read_file(mcd, sl.img) && sl.img.size() == CARD_SIZE) {
                sl.legacy = mcd;
                sl.dir.clear();
                plog("card %d: %s could not be split into save files, using it as is\n", s + 1, mcd.c_str());
                continue;
            }
        }
        load(s);
    }
}

void card_vblank() {
    for (int s = 0; s < 2; s++)
        if (s_slot[s].dirty && ++s_slot[s].dirty > SYNC_DELAY) sync(s);
}

void card_flush() {
    for (int s = 0; s < 2; s++)
        if (s_slot[s].dirty) sync(s);
}

bool card_present(int slot) { return slot >= 0 && slot < 2 && !s_slot[slot].img.empty(); }

bool card_read_frame(int slot, int frame, u8* out) {
    if (!card_present(slot) || frame < 0 || frame >= 1024) return false;
    memcpy(out, &s_slot[slot].img[frame * FRAME], FRAME);
    return true;
}

bool card_write_frame(int slot, int frame, const u8* in) {
    if (!card_present(slot) || frame < 0 || frame >= 1024) return false;
    Slot& sl = s_slot[slot];
    memcpy(&sl.img[frame * FRAME], in, FRAME);
    if (!sl.legacy.empty()) { // write through, as the old .mcd handling did
        if (FILE* f = fopen(sl.legacy.c_str(), "r+b")) {
            fseek(f, frame * FRAME, SEEK_SET);
            fwrite(in, 1, FRAME, f);
            fclose(f);
        }
    } else if (!sl.dir.empty()) {
        sl.dirty = 1; // (re)start the quiet period
    }
    return true;
}

// online play: the host's cards are copied to the client and used there in memory only
const std::vector<u8>& card_image(int slot) { return s_slot[slot].img; }
void card_replace(int slot, const u8* data) {
    Slot& sl = s_slot[slot];
    sl.img.assign(data, data + CARD_SIZE);
    sl.dir.clear(); // never written back over the client's own saves
    sl.legacy.clear();
    sl.dirty = 0;
}
