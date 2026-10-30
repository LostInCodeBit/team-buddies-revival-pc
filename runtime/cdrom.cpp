// cdrom.cpp - CD-ROM controller reading straight from the user's CUE/BIN image,
// including XA-ADPCM streaming audio and CD-DA playback.
#include "psx.h"
#include <deque>
#include <algorithm>
#include <fstream>
#include <mutex>

// ---------------------------------------------------------------- disc image
struct TrackFile { FILE* f; u32 start_lba; u32 sectors; };
struct Track { int num; bool audio; u32 start; u32 length; }; // start = LBA of INDEX 01
static std::vector<TrackFile> s_files;
static std::vector<Track> s_tracks;
static u32 s_disc_sectors;

static u32 msf_to_lba(int m, int s, int f) { return (u32)((m * 60 + s) * 75 + f); }

bool cd_open(const std::string& cue_path) {
    std::string path = cue_path;
    std::string dir;
    size_t sl = path.find_last_of("/\\");
    if (sl != std::string::npos) dir = path.substr(0, sl + 1);
    std::ifstream cue(path);
    if (!cue) return false;
    std::string line;
    u32 lba = 0;
    int cur_track = 0;
    bool cur_audio = false;
    u32 file_start = 0;
    while (std::getline(cue, line)) {
        size_t p;
        if ((p = line.find("FILE \"")) != std::string::npos) {
            size_t q = line.find('"', p + 6);
            std::string name = line.substr(p + 6, q - p - 6);
            FILE* f = fopen((dir + name).c_str(), "rb");
            if (!f) { plog("cd: cannot open %s\n", (dir + name).c_str()); return false; }
            _fseeki64(f, 0, SEEK_END);
            u32 n = (u32)(_ftelli64(f) / 2352);
            file_start = lba;
            s_files.push_back({f, lba, n});
            lba += n;
        } else if ((p = line.find("TRACK ")) != std::string::npos) {
            cur_track = atoi(line.c_str() + p + 6);
            cur_audio = line.find("AUDIO") != std::string::npos;
        } else if ((p = line.find("INDEX 01 ")) != std::string::npos) {
            int m, s, f;
            if (sscanf(line.c_str() + p + 9, "%d:%d:%d", &m, &s, &f) == 3)
                s_tracks.push_back({cur_track, cur_audio, file_start + msf_to_lba(m, s, f), 0});
        }
    }
    s_disc_sectors = lba;
    for (size_t i = 0; i < s_tracks.size(); i++)
        s_tracks[i].length = (i + 1 < s_tracks.size() ? s_tracks[i + 1].start : s_disc_sectors) - s_tracks[i].start;
    plog("cd: %zu tracks, %u sectors\n", s_tracks.size(), s_disc_sectors);
    return !s_files.empty();
}

bool cd_read_raw_sector(u32 lba, u8* out) {
    for (auto& tf : s_files) {
        if (lba >= tf.start_lba && lba < tf.start_lba + tf.sectors) {
            _fseeki64(tf.f, (long long)(lba - tf.start_lba) * 2352, SEEK_SET);
            return fread(out, 1, 2352, tf.f) == 2352;
        }
    }
    memset(out, 0, 2352);
    return false;
}

bool cd_find_file(const char* name, u32& lba, u32& size) {
    u8 raw[2352];
    cd_read_raw_sector(16, raw);
    const u8* pvd = raw + 24;
    u32 rlba = *(const u32*)(pvd + 156 + 2), rsize = *(const u32*)(pvd + 156 + 10);
    size_t nl = strlen(name);
    for (u32 s = 0; s < (rsize + 2047) / 2048; s++) {
        cd_read_raw_sector(rlba + s, raw);
        const u8* d = raw + 24;
        for (u32 p = 0; p < 2048;) {
            u8 len = d[p];
            if (!len) break;
            u8 n = d[p + 32];
            const char* nm = (const char*)d + p + 33;
            if (n >= nl && _strnicmp(nm, name, nl) == 0 && (n == nl || nm[nl] == ';')) {
                lba = *(const u32*)(d + p + 2);
                size = *(const u32*)(d + p + 10);
                return true;
            }
            p += len;
        }
    }
    return false;
}

std::vector<u8> cd_read_file(const char* name) {
    u32 lba, size;
    std::vector<u8> out;
    if (!cd_find_file(name, lba, size)) return out;
    out.resize(size);
    u8 raw[2352];
    for (u32 o = 0; o < size; o += 2048) {
        cd_read_raw_sector(lba++, raw);
        memcpy(&out[o], raw + 24, std::min<u32>(2048, size - o));
    }
    return out;
}

// ---------------------------------------------------------------- controller state
static u8 bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static int unbcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

enum { ST_ERROR = 1, ST_MOTOR = 2, ST_SEEKERR = 4, ST_IDERR = 8, ST_SHELL = 16, ST_READ = 32, ST_SEEK = 64, ST_PLAY = 128 };

static u8 s_index;
static std::deque<u8> s_params, s_resp;
static u8 s_ienable = 0x1F, s_iflag = 0;
static u8 s_stat = ST_MOTOR;
static u8 s_mode = 0;
static u8 s_filter_file, s_filter_chan;
static u32 s_setloc = 0, s_pos = 0;
static bool s_setloc_pending = false;
static bool s_reading = false, s_playing = false;
static bool s_busy = false;
static bool s_muted = false, s_adpcm_mute = false;
static u8 s_vol_ll = 0x80, s_vol_lr = 0, s_vol_rl = 0, s_vol_rr = 0x80;
static u8 s_pvol_ll = 0x80, s_pvol_lr = 0, s_pvol_rl = 0, s_pvol_rr = 0x80;
static u8 s_last_header[8];
static int s_play_track = 0;

static u8 s_sector[2352];
static bool s_sector_ready = false;
static std::vector<u8> s_fifo;
static size_t s_fifo_pos = 0;

struct PendingInt { u8 type; std::vector<u8> resp; u64 due; std::vector<u8> sector; }; // due = earliest cpu time; INT1 carries its sector
static std::deque<PendingInt> s_queue;

// ---------------------------------------------------------------- audio (XA + CD-DA), 44.1 kHz stereo ring
static std::mutex s_audio_mtx;
static std::deque<s16> s_audio; // interleaved L/R

static void audio_push(s16 l, s16 r) {
    s_audio.push_back(l);
    s_audio.push_back(r);
    if (s_audio.size() > 44100 * 2) { s_audio.pop_front(); s_audio.pop_front(); }
}

void cd_get_audio(s16* out, int frames) {
    std::lock_guard<std::mutex> lk(s_audio_mtx);
    for (int i = 0; i < frames; i++) {
        s16 l = 0, r = 0;
        if (s_audio.size() >= 2) { l = s_audio.front(); s_audio.pop_front(); r = s_audio.front(); s_audio.pop_front(); }
        if (s_muted) { l = r = 0; }
        int ol = (l * s_pvol_ll + r * s_pvol_rl) >> 7;
        int orr = (l * s_pvol_lr + r * s_pvol_rr) >> 7;
        out[i * 2] = (s16)std::clamp(ol, -32768, 32767);
        out[i * 2 + 1] = (s16)std::clamp(orr, -32768, 32767);
    }
}

// XA-ADPCM decoder
static s32 s_xa_old[2][2];
static double s_xa_phase = 0;
static s16 s_xa_prev[2];

int g_dbg_xa_peak, g_dbg_cdvol;
static void xa_output(const s16* l, const s16* r, int n, int rate) {
    g_dbg_cdvol = s_pvol_ll | s_pvol_rr << 8 | (s_muted ? 0x10000 : 0) | (s_adpcm_mute ? 0x20000 : 0);
    // resample to 44100 Hz with linear interpolation
    double step = (double)rate / 44100.0;
    std::lock_guard<std::mutex> lk(s_audio_mtx);
    for (int i = 0; i < n;) {
        double t = s_xa_phase;
        s16 a0 = i == 0 ? s_xa_prev[0] : l[i - 1], a1 = l[i];
        s16 b0 = i == 0 ? s_xa_prev[1] : r[i - 1], b1 = r[i];
        audio_push((s16)(a0 + (a1 - a0) * t), (s16)(b0 + (b1 - b0) * t));
        s_xa_phase += step;
        while (s_xa_phase >= 1.0) { s_xa_phase -= 1.0; i++; }
        (void)t;
    }
    if (n) { s_xa_prev[0] = l[n - 1]; s_xa_prev[1] = r[n - 1]; }
    for (int i = 0; i < n; i++) g_dbg_xa_peak = std::max({g_dbg_xa_peak, std::abs((int)l[i]), std::abs((int)r[i])});
}

static void xa_decode(const u8* raw) {
    const u8* sub = raw + 16;
    u8 ci = sub[3];
    bool stereo = (ci & 3) == 1;
    int rate = ((ci >> 2) & 3) == 1 ? 18900 : 37800;
    bool eight = ((ci >> 4) & 3) == 1;
    static const int K0[4] = {0, 60, 115, 98}, K1[4] = {0, 0, -52, -55};
    std::vector<s16> L, R;
    L.reserve(4032);
    R.reserve(4032);
    const u8* data = raw + 24;
    for (int g = 0; g < 18; g++) {
        const u8* grp = data + g * 128;
        int units = eight ? 4 : 8;
        for (int u = 0; u < units; u++) {
            u8 hdr = grp[4 + u];
            int shift = hdr & 0x0F;
            if (shift > 12) shift = 9;
            int filt = (hdr >> 4) & 3;
            int ch = stereo ? (u & 1) : 0;
            s32& o1 = s_xa_old[ch][0];
            s32& o2 = s_xa_old[ch][1];
            s16 out[28];
            for (int j = 0; j < 28; j++) {
                const u8* wd = grp + 16 + j * 4;
                s32 smp;
                if (eight) smp = (s16)(wd[u] << 8);
                else smp = (s16)(((wd[u / 2] >> ((u & 1) * 4)) & 0xF) << 12);
                smp >>= shift;
                smp += (o1 * K0[filt] + o2 * K1[filt] + 32) >> 6;
                smp = std::clamp(smp, -32768, 32767);
                o2 = o1;
                o1 = smp;
                out[j] = (s16)smp;
            }
            if (stereo) {
                if (u & 1) R.insert(R.end(), out, out + 28);
                else L.insert(L.end(), out, out + 28);
            } else {
                L.insert(L.end(), out, out + 28);
            }
        }
    }
    if (!stereo) R = L;
    size_t n = std::min(L.size(), R.size());
    if (!s_adpcm_mute) xa_output(L.data(), R.data(), (int)n, rate);
}

// ---------------------------------------------------------------- interrupts
static void update_irq() {
    if (s_iflag & s_ienable & 0x1F) irq_raise(IRQ_CDROM);
}

static void deliver_next(int) {
    if (g_settings.log_level >= 3) plog("cd deliver: iflag=%02X ien=%02X queue=%zu at %llu (sr=%08X depth=%d)\n", s_iflag, s_ienable, s_queue.size(), cpu_now(), g_cpu.cop0[12], g_exception_depth);
    if (s_iflag & 7) return; // previous interrupt not acknowledged yet
    if (s_queue.empty()) return;
    PendingInt p = s_queue.front();
    s_queue.pop_front();
    s_resp.assign(p.resp.begin(), p.resp.end());
    if (p.type == 1 && !p.sector.empty()) { memcpy(s_sector, p.sector.data(), 2352); memcpy(s_last_header, s_sector + 12, 8); s_sector_ready = true; }
    s_iflag = (s_iflag & ~7) | p.type;
    if (p.type == 3) s_busy = false;
    update_irq();
}

static void schedule_delivery() {
    if (s_queue.empty() || (s_iflag & 7) || sched_active(EV_CDROM_CMD)) return;
    u64 now = cpu_now(), due = s_queue.front().due;
    sched_set(EV_CDROM_CMD, due > now + 500 ? due - now : 500, deliver_next);
}

static void push_int(u8 type, std::vector<u8> resp, u64 delay) {
    u64 due = cpu_now() + delay;
    if (!s_queue.empty() && s_queue.back().due > due) due = s_queue.back().due;
    s_queue.push_back({type, std::move(resp), due});
    schedule_delivery();
}

static u8 stat() {
    u8 s = s_stat & ~(ST_READ | ST_PLAY | ST_SEEK);
    if (s_reading) s |= ST_READ;
    if (s_playing) s |= ST_PLAY;
    return s;
}

static u64 sector_period() { return (s_mode & 0x80) ? CPU_HZ / 150 : CPU_HZ / 75; }

// ---------------------------------------------------------------- sector reading
static void ev_read(int);

static int track_of(u32 lba) {
    int t = 1;
    for (auto& tr : s_tracks) if (lba >= tr.start - (tr.num > 1 ? 150 : 0)) t = tr.num;
    return t;
}

static void ev_read(int) {
    if (!s_reading && !s_playing) return;
    sched_set(EV_CDROM_READ, sector_period(), ev_read);
    u8 raw[2352];
    cd_read_raw_sector(s_pos, raw);
    u32 lba = s_pos++;
    if (g_settings.log_level >= 3) plog("cd read lba %u%s\n", lba, s_playing ? " (audio)" : "");
    if (s_playing) {
        // CD-DA: 588 stereo frames per sector
        {
            std::lock_guard<std::mutex> lk(s_audio_mtx);
            const s16* smp = (const s16*)raw;
            for (int i = 0; i < 588; i++) audio_push(smp[i * 2], smp[i * 2 + 1]);
        }
        if ((s_mode & 0x04) && (lba % 10) == 0) {
            u32 abs = lba + 150;
            int tn = track_of(lba);
            u32 rel = lba - s_tracks[std::max(0, tn - 1)].start;
            bool use_abs = (lba / 10) & 1;
            u32 v = use_abs ? abs : rel;
            u8 mm = bcd(v / 75 / 60), ss = bcd((v / 75) % 60), ff = bcd(v % 75);
            push_int(1, {stat(), bcd(tn), 1, mm, (u8)(use_abs ? ss : ss | 0x80), ff, 0, 0}, 1000);
        }
        int tn = track_of(lba);
        const Track& tr = s_tracks[std::max(0, tn - 1)];
        if ((s_mode & 0x02) && s_pos >= tr.start + tr.length) {
            s_playing = false;
            push_int(4, {stat()}, 1000);
        }
        return;
    }
    memcpy(s_last_header, raw + 12, 8);
    const u8* sub = raw + 16;
    bool form2 = (sub[2] & 0x20) != 0, audio = (sub[2] & 0x04) != 0, rt = (sub[2] & 0x40) != 0;
    (void)rt;
    if ((s_mode & 0x40) && form2 && audio) {
        if (!(s_mode & 0x08) || (sub[0] == s_filter_file && sub[1] == s_filter_chan)) xa_decode(raw);
        return;
    }
    if ((s_mode & 0x08) && form2 && audio) return;
    // the drive buffers sectors until the host takes them; each INT1 hands over the next one
    int buffered = 0;
    for (auto& q : s_queue) if (q.type == 1) buffered++;
    if (buffered >= 8) {
        for (auto it = s_queue.begin(); it != s_queue.end(); ++it)
            if (it->type == 1) { s_queue.erase(it); break; } // overrun: the oldest sector is lost
    }
    u64 due = cpu_now() + 100;
    if (!s_queue.empty() && s_queue.back().due > due) due = s_queue.back().due;
    s_queue.push_back({1, {stat()}, due, std::vector<u8>(raw, raw + 2352)});
    schedule_delivery();
}

static void start_reading() {
    s_reading = true;
    s_playing = false;
    u64 delay = sector_period();
    if (s_setloc_pending) { s_pos = s_setloc; s_setloc_pending = false; delay += 30000; }
    sched_set(EV_CDROM_READ, delay, ev_read);
}

// ---------------------------------------------------------------- commands
static void command(u8 cmd) {
    std::vector<u8> p(s_params.begin(), s_params.end());
    s_params.clear();
    const u64 ACK = 20000;
    s_busy = true;
    if (g_settings.log_level >= 2) {
        char pb[64] = "";
        for (size_t k = 0; k < p.size() && k < 8; k++) snprintf(pb + strlen(pb), 8, " %02X", p[k]);
        plog("cd cmd %02X%s (pos %u mode %02X)\n", cmd, pb, s_pos, s_mode);
    }
    switch (cmd) {
    case 0x01: push_int(3, {stat()}, ACK); break;
    case 0x02:
        if (p.size() >= 3) {
            s_setloc = msf_to_lba(unbcd(p[0]), unbcd(p[1]), unbcd(p[2]));
            s_setloc = s_setloc >= 150 ? s_setloc - 150 : 0;
            s_setloc_pending = true;
        }
        push_int(3, {stat()}, ACK);
        break;
    case 0x03: { // Play
        int tn = p.empty() ? 0 : unbcd(p[0]);
        if (tn > 0 && tn <= (int)s_tracks.size()) { s_pos = s_tracks[tn - 1].start; s_setloc_pending = false; }
        else if (s_setloc_pending) { s_pos = s_setloc; s_setloc_pending = false; }
        s_play_track = track_of(s_pos);
        s_reading = false;
        s_playing = true;
        push_int(3, {stat()}, ACK);
        sched_set(EV_CDROM_READ, sector_period(), ev_read);
        break;
    }
    case 0x04: case 0x05: push_int(3, {stat()}, ACK); break;
    case 0x06: case 0x1B: // ReadN / ReadS
        start_reading();
        push_int(3, {stat()}, ACK);
        break;
    case 0x07: s_stat |= ST_MOTOR; push_int(3, {stat()}, ACK); push_int(2, {stat()}, 300000); break;
    case 0x08:
        s_reading = s_playing = false;
        sched_cancel(EV_CDROM_READ);
        push_int(3, {stat()}, ACK);
        s_stat &= ~ST_MOTOR;
        push_int(2, {stat()}, 300000);
        break;
    case 0x09: { // Pause
        bool was = s_reading || s_playing;
        push_int(3, {stat()}, ACK);
        s_reading = s_playing = false;
        sched_cancel(EV_CDROM_READ);
        for (auto it = s_queue.begin(); it != s_queue.end();) { if (it->type == 1) it = s_queue.erase(it); else ++it; }
        push_int(2, {stat()}, was ? 200000 : 9000);
        break;
    }
    case 0x0A: // Init
        s_mode = 0;
        s_reading = s_playing = false;
        sched_cancel(EV_CDROM_READ);
        s_stat = ST_MOTOR;
        push_int(3, {stat()}, 60000);
        push_int(2, {stat()}, 120000);
        break;
    case 0x0B: s_muted = true; push_int(3, {stat()}, ACK); break;
    case 0x0C: s_muted = false; push_int(3, {stat()}, ACK); break;
    case 0x0D:
        if (p.size() >= 2) { s_filter_file = p[0]; s_filter_chan = p[1]; }
        push_int(3, {stat()}, ACK);
        break;
    case 0x0E: if (!p.empty()) s_mode = p[0]; push_int(3, {stat()}, ACK); break;
    case 0x0F: push_int(3, {stat(), s_mode, 0, s_filter_file, s_filter_chan}, ACK); break;
    case 0x10: push_int(3, std::vector<u8>(s_last_header, s_last_header + 8), ACK); break;
    case 0x11: {
        u32 lba = s_pos;
        int tn = track_of(lba);
        const Track& tr = s_tracks[std::max(0, tn - 1)];
        u32 rel = lba >= tr.start ? lba - tr.start : tr.start - lba;
        u32 abs = lba + 150;
        push_int(3, {bcd(tn), 1, bcd(rel / 75 / 60), bcd((rel / 75) % 60), bcd(rel % 75), bcd(abs / 75 / 60), bcd((abs / 75) % 60), bcd(abs % 75)}, ACK);
        break;
    }
    case 0x12: push_int(3, {stat()}, ACK); push_int(2, {stat()}, 100000); break;
    case 0x13: push_int(3, {stat(), 1, bcd((int)s_tracks.size())}, ACK); break;
    case 0x14: {
        int tn = p.empty() ? 0 : unbcd(p[0]);
        u32 lba = tn == 0 ? s_disc_sectors : (tn <= (int)s_tracks.size() ? s_tracks[tn - 1].start : 0);
        lba += 150;
        push_int(3, {stat(), bcd(lba / 75 / 60), bcd((lba / 75) % 60)}, ACK);
        break;
    }
    case 0x15: case 0x16: // SeekL / SeekP
        s_reading = s_playing = false;
        sched_cancel(EV_CDROM_READ);
        if (s_setloc_pending) { s_pos = s_setloc; s_setloc_pending = false; }
        push_int(3, {stat()}, ACK);
        push_int(2, {stat()}, 120000);
        break;
    case 0x19: {
        u8 sub = p.empty() ? 0 : p[0];
        if (sub == 0x20) push_int(3, {0x94, 0x09, 0x19, 0xC0}, ACK);
        else push_int(3, {stat()}, ACK);
        break;
    }
    case 0x1A: // GetID: licensed NTSC-U data disc
        push_int(3, {stat()}, ACK);
        push_int(2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'}, 33000);
        break;
    case 0x1C: push_int(3, {stat()}, ACK); break;
    case 0x1E: push_int(3, {stat()}, ACK); push_int(2, {stat()}, 500000); break;
    default:
        log_once(("cd" + std::to_string(cmd)).c_str(), "cd: unknown command %02X\n", cmd);
        push_int(5, {(u8)(stat() | ST_ERROR), 0x40}, ACK);
        break;
    }
}

void cd_init() {
    s_index = 0;
    s_params.clear();
    s_resp.clear();
    s_queue.clear();
    s_iflag = 0;
    s_ienable = 0x1F; // the BIOS leaves CD interrupts enabled after boot
    s_stat = ST_MOTOR;
}

static u8 cd_read_impl(u32 off);
u8 cd_read(u32 off) {
    u8 v = cd_read_impl(off);
    if (g_settings.log_level >= 4) plog("cd r%u.%u = %02X\n", off, s_index, v);
    return v;
}
static u8 cd_read_impl(u32 off) {
    switch (off) {
    case 0: {
        u8 v = s_index;
        if (!s_xa_old[0][0] && false) v |= 4;
        if (s_params.empty()) v |= 8;
        if (s_params.size() < 16) v |= 16;
        if (!s_resp.empty()) v |= 32;
        if (s_fifo_pos < s_fifo.size()) v |= 64;
        if (s_busy) v |= 128;
        return v;
    }
    case 1: {
        if (s_resp.empty()) return 0;
        u8 v = s_resp.front();
        s_resp.pop_front();
        return v;
    }
    case 2: {
        if (s_fifo_pos < s_fifo.size()) return s_fifo[s_fifo_pos++];
        return 0;
    }
    case 3:
        if (s_index & 1) return s_iflag | 0xE0;
        return s_ienable | 0xE0;
    }
    return 0;
}

void cd_write(u32 off, u8 v) {
    if (g_settings.log_level >= 3) plog("cd w%u.%u = %02X\n", off, s_index, v);
    switch (off) {
    case 0: s_index = v & 3; return;
    case 1:
        switch (s_index) {
        case 0: command(v); break;
        case 3: s_vol_rr = v; break;
        default: break;
        }
        return;
    case 2:
        switch (s_index) {
        case 0: if (s_params.size() < 16) s_params.push_back(v); break;
        case 1: s_ienable = v & 0x1F; update_irq(); break;
        case 2: s_vol_ll = v; break;
        case 3: s_vol_rl = v; break;
        }
        return;
    case 3:
        switch (s_index) {
        case 0:
            if (v & 0x80) {
                // a second request while the fifo still holds data is ignored
                if (s_sector_ready && s_fifo_pos >= s_fifo.size()) {
                    size_t sz = (s_mode & 0x20) ? 2340 : 2048;
                    const u8* src = (s_mode & 0x20) ? s_sector + 12 : s_sector + 24;
                    s_fifo.assign(src, src + sz);
                    s_fifo_pos = 0;
                }
            } else {
                s_fifo.clear();
                s_fifo_pos = 0;
            }
            break;
        case 1:
            if (g_settings.log_level >= 3 && (s_iflag & 7)) plog("cd ack int%d at %llu (sr=%08X depth=%d)\n", s_iflag & 7, cpu_now(), g_cpu.cop0[12], g_exception_depth);
            s_iflag &= ~(v & 0x1F);
            if (v & 0x40) s_params.clear();
            if (!(s_iflag & 7)) schedule_delivery(); // next queued interrupt follows the acknowledge
            break;
        case 2: s_vol_lr = v; break;
        case 3:
            s_adpcm_mute = v & 1;
            if (v & 0x20) { s_pvol_ll = s_vol_ll; s_pvol_lr = s_vol_lr; s_pvol_rl = s_vol_rl; s_pvol_rr = s_vol_rr; }
            break;
        }
        return;
    }
}

u32 cd_fifo_remaining() { return s_fifo_pos < s_fifo.size() ? (u32)(s_fifo.size() - s_fifo_pos) : 0; }

u32 cd_dma_read() {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        u8 b = s_fifo_pos < s_fifo.size() ? s_fifo[s_fifo_pos++] : 0;
        v |= (u32)b << (i * 8);
    }
    return v;
}

void cd_dma_read_block(u8* dst, u32 bytes) {
    for (u32 i = 0; i < bytes; i++) dst[i] = s_fifo_pos < s_fifo.size() ? s_fifo[s_fifo_pos++] : 0;
}
