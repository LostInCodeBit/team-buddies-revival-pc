// discord.cpp - Discord Rich Presence: the Discord app on this PC shows "Playing Team Buddies", where the
// player is (menus, battle...) and for how long. Discord's local IPC: a named pipe on Windows, a Unix socket
// elsewhere, carrying frames of {u32 opcode, u32 length, JSON}. A worker thread owns the connection, so the
// game never waits on Discord; while Discord is closed it retries with a growing delay. Off with
// discord_presence = false. Test harness: TB_DISCORD_PIPE names a fake Discord (runs with TB_INI or TB_HIDDEN
// otherwise leave the real one alone).
#include "psx.h"
#include "platform_common.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// the Discord application whose name shows as "Playing ..." (discord.com/developers/applications)
static const char k_app_id[] = "1566311231785009192"; // "Team Buddies"

namespace {
using Clock = std::chrono::steady_clock;
using std::chrono::seconds;
enum : u32 { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

// shared by the game thread and the worker; never freed, so a worker still running at exit touches nothing gone
struct Shared {
    std::mutex m;
    std::string activity; // the activity's JSON object
    unsigned version = 0; // bumped with every new activity
    std::string log;      // lines for the game thread to log (plog is not thread-safe)
    std::string app_id;
    unsigned long pid = 0;
};
Shared* s_sh;

void worker_log(const std::string& line) {
    std::lock_guard<std::mutex> l(s_sh->m);
    if (s_sh->log.size() < 2048) s_sh->log += "discord: " + line + "\n";
}

#ifdef _WIN32
struct Ipc {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool denied = false; // Discord is there but refuses us: it runs as administrator (its pipe then only lets us read)
    bool open() {
        const char* test = getenv("TB_DISCORD_PIPE");
        denied = false;
        for (int i = 0; i < 10; i++) { // discord-ipc-0..9: one per Discord running (stable, PTB, Canary)
            std::string name = test ? std::string(test) : "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
            h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) denied = true;
            if (h != INVALID_HANDLE_VALUE || test) break;
        }
        return h != INVALID_HANDLE_VALUE;
    }
    void close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    bool write(const void* p, size_t n) {
        DWORD w = 0;
        return WriteFile(h, p, (DWORD)n, &w, nullptr) && w == n;
    }
    int readable() { // -1 = gone, 0 = nothing yet, 1 = data waiting
        DWORD a = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &a, nullptr)) return -1;
        return a > 0;
    }
    bool read(void* p, size_t n) {
        char* c = (char*)p;
        while (n) {
            DWORD got = 0;
            if (!ReadFile(h, c, (DWORD)n, &got, nullptr) && GetLastError() != ERROR_MORE_DATA) return false;
            if (!got) return false;
            c += got;
            n -= got;
        }
        return true;
    }
};
#else
struct Ipc {
    int fd = -1;
    bool denied = false; // a socket that refuses us (permission denied)
    bool open() {
        denied = false;
        std::vector<std::string> paths;
        if (const char* test = getenv("TB_DISCORD_PIPE")) paths.push_back(test);
        else {
            std::vector<std::string> dirs;
            for (const char* e : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
                if (const char* v = getenv(e))
                    if (*v) dirs.push_back(v);
            dirs.push_back("/tmp");
            // the plain install, then the Flatpak and Snap packages and Vesktop's Flatpak
            for (const std::string& d : dirs)
                for (const char* sub : {"", "/app/com.discordapp.Discord", "/snap.discord", "/.flatpak/dev.vencord.Vesktop/xdg-run"})
                    for (int i = 0; i < 10; i++) paths.push_back(d + sub + "/discord-ipc-" + std::to_string(i));
        }
        for (const std::string& p : paths) {
            sockaddr_un a = {};
            a.sun_family = AF_UNIX;
            if (p.size() >= sizeof a.sun_path) continue;
            memcpy(a.sun_path, p.c_str(), p.size() + 1);
            int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (s < 0) return false;
            if (connect(s, (sockaddr*)&a, sizeof a) == 0) {
                fd = s;
                return true;
            }
            if (errno == EACCES) denied = true;
            ::close(s);
        }
        return false;
    }
    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    bool write(const void* p, size_t n) {
        const char* c = (const char*)p;
        while (n) {
            ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return false;
            c += w;
            n -= (size_t)w;
        }
        return true;
    }
    int readable() {
        pollfd q = {fd, POLLIN, 0};
        int r = poll(&q, 1, 0);
        if (r < 0) return errno == EINTR ? 0 : -1;
        if (q.revents & POLLIN) return 1;
        return q.revents & (POLLERR | POLLHUP | POLLNVAL) ? -1 : 0;
    }
    bool read(void* p, size_t n) {
        char* c = (char*)p;
        while (n) {
            ssize_t r = recv(fd, c, n, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            c += r;
            n -= (size_t)r;
        }
        return true;
    }
};
#endif

bool send_frame(Ipc& c, u32 op, const std::string& json) {
    std::string b(8 + json.size(), '\0');
    u32 len = (u32)json.size();
    memcpy(&b[0], &op, 4);
    memcpy(&b[4], &len, 4);
    memcpy(&b[8], json.data(), json.size());
    return c.write(b.data(), b.size());
}

bool recv_frame(Ipc& c, u32& op, std::string& json) {
    u32 hd[2];
    if (!c.read(hd, sizeof hd) || hd[1] > 64 * 1024) return false; // Discord's replies are a few KB
    op = hd[0];
    json.assign(hd[1], '\0');
    return hd[1] == 0 || c.read(&json[0], hd[1]);
}

// the first value of "key" in a JSON text: a string's contents or a bare token ("" if absent)
std::string json_field(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p = j.find_first_not_of(" \t\r\n:", p + k.size());
    if (p == std::string::npos) return "";
    if (j[p] == '"') {
        size_t e = j.find('"', p + 1);
        return e == std::string::npos ? "" : j.substr(p + 1, e - p - 1);
    }
    return j.substr(p, j.find_first_of(",}] \t\r\n", p) - p);
}

std::string esc(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\';
        o += (unsigned char)ch < 0x20 ? ' ' : ch;
    }
    return o;
}

// handshake, then wait up to 5 s for READY; refused = Discord turned the application down (do not keep asking)
bool handshake(Ipc& c, std::string& why, bool& refused) {
    if (!send_frame(c, OP_HANDSHAKE, "{\"v\":1,\"client_id\":\"" + esc(s_sh->app_id) + "\"}")) { why = "the connection broke"; return false; }
    for (auto end = Clock::now() + seconds(5); Clock::now() < end;) {
        int r = c.readable();
        if (r < 0) { why = "the connection broke"; return false; }
        if (r == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        u32 op;
        std::string j;
        if (!recv_frame(c, op, j)) { why = "the connection broke"; return false; }
        if (op == OP_FRAME && json_field(j, "evt") == "READY") return true;
        if (op == OP_CLOSE) {
            why = "refused: " + json_field(j, "message");
            refused = true;
            return false;
        }
    }
    why = "no answer";
    return false;
}

void worker() {
    Ipc c;
    bool up = false;
    unsigned sent = 0;
    long nonce = 0;
    int delay = 2; // seconds before trying again while Discord is closed: doubles up to a minute
    bool told_denied = false;
    auto retry_at = Clock::now(), send_at = Clock::now();
    for (;; std::this_thread::sleep_for(std::chrono::milliseconds(250))) {
        auto now = Clock::now();
        if (!up) {
            if (now < retry_at) continue;
            if (!c.open()) {
                if (c.denied && !told_denied) {
#ifdef _WIN32
                    worker_log("Discord runs as administrator and refuses other programs; start it normally to show your status");
#else
                    worker_log("Discord's socket refuses this program (permission denied)");
#endif
                    told_denied = true;
                }
                retry_at = now + seconds(delay);
                delay = std::min(delay * 2, 60);
                continue;
            }
            std::string why;
            bool refused = false;
            if (!handshake(c, why, refused)) {
                c.close();
                worker_log(why);
                retry_at = Clock::now() + seconds(refused ? 600 : delay);
                delay = std::min(delay * 2, 60);
                continue;
            }
            up = true;
            delay = 2;
            sent = 0;
            worker_log("connected");
        }
        // Discord's replies and pings
        bool lost = false;
        for (int r; !lost && (r = c.readable()) != 0;) {
            u32 op;
            std::string j;
            if (r < 0 || !recv_frame(c, op, j)) lost = true;
            else if (op == OP_PING) lost = !send_frame(c, OP_PONG, j);
            else if (op == OP_CLOSE) lost = true;
            else if (op == OP_FRAME && json_field(j, "evt") == "ERROR") worker_log("error: " + json_field(j, "message"));
        }
        if (!lost && now >= send_at) {
            std::string act;
            unsigned ver;
            {
                std::lock_guard<std::mutex> l(s_sh->m);
                act = s_sh->activity;
                ver = s_sh->version;
            }
            if (ver != sent) {
                std::string msg = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(s_sh->pid) + ",\"activity\":" + act +
                                  "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
                if (!send_frame(c, OP_FRAME, msg)) lost = true;
                sent = ver;
                send_at = now + seconds(4); // Discord takes at most 5 updates per 20 s
            }
        }
        if (lost) {
            c.close();
            up = false;
            retry_at = now + seconds(5);
            worker_log("disconnected");
        }
    }
}

time_t s_start;

// where the player is, from the overlay loaded at the shared address: MNU the menus (team and battle setup
// included), ROT story mode's map and briefings, GAME every battle, MPLR the movie player. nullptr while one
// loads (the last place stays)
const char* where(const std::string& ov) {
    if (ov == "GAME") return "In battle";
    if (ov == "ROT") return "Choosing a mission";
    if (ov == "MNU") return "In the menus";
    if (ov == "MPLR") return "Watching a movie";
    return nullptr;
}
} // namespace

void discord_start() {
    const char* test = getenv("TB_DISCORD_PIPE");
    if (!g_settings.discord || (!test && (getenv("TB_INI") || hidden_run()))) return;
    const char* id = k_app_id[0] ? k_app_id : test ? "0" : nullptr;
    if (!id) {
        plog("discord: this build has no Discord application id, presence off\n");
        return;
    }
    s_sh = new Shared;
    s_sh->app_id = id;
#ifdef _WIN32
    s_sh->pid = GetCurrentProcessId();
#else
    s_sh->pid = (unsigned long)getpid();
#endif
    s_start = time(nullptr);
    std::thread(worker).detach();
}

// game thread, every vblank: twice a second, a place that held for a second goes to Discord
void discord_tick() {
    static unsigned n;
    if (n++ % 30) return;
    const std::string ov = rec_loaded_overlays();
    static std::string last_ov = "-";
    if (ov != last_ov && g_settings.log_level >= 2) plog("overlay: %s (vblank %u)\n", ov.empty() ? "none" : ov.c_str(), n);
    last_ov = ov;
    if (!s_sh) return;
    std::string log;
    {
        std::lock_guard<std::mutex> l(s_sh->m);
        log.swap(s_sh->log);
    }
    if (!log.empty()) plog("%s", log.c_str());
    const char* place = where(ov);
    if (!place) return;
    std::string act = std::string("{\"details\":\"") + place + "\"";
    if (netplay_active()) act += ",\"state\":\"Playing online\"";
    act += ",\"timestamps\":{\"start\":" + std::to_string((long long)s_start) + "}}";
    static std::string candidate, published;
    static int held;
    if (act != candidate) {
        candidate = act;
        held = 0;
        return;
    }
    if (++held < 2 || act == published) return;
    published = act;
    std::lock_guard<std::mutex> l(s_sh->m);
    s_sh->activity = act;
    s_sh->version++;
}
