// tbmatch - matchmaking and relay server for Team Buddies online play.
//
// One UDP port. Games queue for a quick match or a private room; the server pairs two compatible
// games (same build and disc), tells each the other's public and LAN address, and hands out a
// session id plus a per-side token. The games then talk directly (peer to peer); only when their
// NATs refuse that do they bind to the relay, which forwards packets between exactly the two
// endpoints that proved the session token. Everything is in memory: nothing to persist or back up.
//
// Robustness: every table is bounded and expires idle entries, control messages are rate limited
// per IP, relay traffic per session; no reply is larger than the request that caused it (no
// amplification). Logs carry counts only, never addresses or tokens.
//
// Build: Linux  g++ -std=c++17 -O2 -static -o tbmatch tbmatch.cpp
//        Win    cl /std:c++17 /O2 /EHsc tbmatch.cpp ws2_32.lib bcrypt.lib
// Run:   tbmatch [--port 7846]        Health: tbmatch --healthcheck [--port 7846]
#include "tbmatch_proto.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
typedef int socklen_t;
static void sock_close(SOCKET s) { closesocket(s); }
static bool random_bytes(void* p, size_t n) { return BCryptGenRandom(nullptr, (PUCHAR)p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0; }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
const SOCKET INVALID_SOCKET = -1;
static void sock_close(SOCKET s) { close(s); }
static bool random_bytes(void* p, size_t n) {
    uint8_t* b = (uint8_t*)p;
    while (n) {
        ssize_t r = getrandom(b, n, 0);
        if (r <= 0) return false;
        b += r;
        n -= (size_t)r;
    }
    return true;
}
#endif

using namespace tbm;
typedef std::chrono::steady_clock Clock;

// ---------------------------------------------------------------- limits
const size_t MAX_TICKETS = 20000, MAX_ROOMS = 10000, MAX_SESSIONS = 10000, MAX_LIMITED_IPS = 200000;
const double TICKET_IDLE_S = 15, MATCHED_KEEP_S = 30, ROOM_TTL_S = 600, SESSION_IDLE_S = 60;
const double CTRL_RATE = 20, CTRL_BURST = 40;          // control packets per second per IP
const double RELAY_PPS = 400, RELAY_BPS = 256 * 1024;  // per session side

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static void logf(const char* fmt, ...) {
    char ts[32];
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("%s %s\n", ts, msg);
    fflush(stdout);
}

static double now_s() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

struct Endpoint {
    uint32_t ip = 0;   // network order
    uint16_t port = 0; // network order
    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
};
static Endpoint ep_of(const sockaddr_in& a) { return {a.sin_addr.s_addr, a.sin_port}; }
static sockaddr_in sa_of(const Endpoint& e) {
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = e.ip;
    a.sin_port = e.port;
    return a;
}

struct Bucket {
    double tokens, last;
    bool take(double rate, double burst, double t, double cost = 1) {
        tokens = std::min(burst, tokens + (t - last) * rate);
        last = t;
        if (tokens < cost) return false;
        tokens -= cost;
        return true;
    }
};

struct Ticket {
    uint64_t id = 0, build = 0, disc = 0;
    Endpoint pub;
    Addr4 lan = {};
    uint8_t mode = 0;
    std::string room;
    double created = 0, last = 0;
    bool matched = false;
    double matched_at = 0;
    Match match = {};
};

struct Session {
    uint8_t token[2][16];
    Endpoint bound[2];
    bool has[2] = {false, false};
    double last = 0;
    Bucket pps[2], bps[2];
};

static SOCKET g_sock = INVALID_SOCKET;
static std::unordered_map<uint64_t, Ticket> g_tickets;
static std::vector<uint64_t> g_quick;                       // waiting quick-match tickets, oldest first
static std::unordered_map<std::string, uint64_t> g_rooms;   // code -> creator ticket
static std::unordered_map<std::string, Session> g_sessions; // 16-byte id -> session
static std::unordered_map<uint32_t, Bucket> g_limit;
static uint64_t g_stat_matches = 0, g_stat_relayed = 0, g_stat_dropped = 0;

template <class T> static void init(T& p, uint8_t type) { memset(&p, 0, sizeof p); p.h.magic = MAGIC; p.h.version = VERSION; p.h.type = type; }
template <class T> static void send_to(const T& p, const Endpoint& e) {
    sockaddr_in a = sa_of(e);
    sendto(g_sock, (const char*)&p, sizeof p, 0, (const sockaddr*)&a, sizeof a);
}

static void send_status(const Ticket& t, uint8_t state) {
    Status s;
    init(s, STATUS);
    s.ticket = t.id;
    s.state = state;
    if (state == ST_ROOM_OPEN) memcpy(s.room, t.room.c_str(), std::min<size_t>(t.room.size(), 5));
    s.seen.ip = t.pub.ip;
    s.seen.port = t.pub.port;
    s.waiting = (uint16_t)std::min<size_t>(g_quick.size(), 65535);
    send_to(s, t.pub);
}

static std::string new_room_code() {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no 0/O, 1/I
    for (int attempt = 0; attempt < 32; attempt++) {
        uint8_t r[5];
        random_bytes(r, sizeof r);
        std::string c;
        for (uint8_t b : r) c += alphabet[b % 32];
        if (!g_rooms.count(c)) return c;
    }
    return "";
}

static void remove_from_quick(uint64_t id) {
    for (size_t i = 0; i < g_quick.size(); i++) if (g_quick[i] == id) { g_quick.erase(g_quick.begin() + i); return; }
}

// pair two tickets: a = host (player 1), b = client (player 2)
static void pair(Ticket& a, Ticket& b, double t) {
    if (g_sessions.size() >= MAX_SESSIONS) { send_status(a, ST_BUSY); send_status(b, ST_BUSY); return; }
    uint8_t sid[16];
    Session s;
    if (!random_bytes(sid, 16) || !random_bytes(s.token, sizeof s.token)) return;
    s.last = t;
    for (int k = 0; k < 2; k++) { s.pps[k] = {RELAY_PPS, t}; s.bps[k] = {RELAY_BPS, t}; }
    g_sessions[std::string((const char*)sid, 16)] = s;
    Ticket* side[2] = {&a, &b};
    for (int k = 0; k < 2; k++) {
        Ticket& me = *side[k];
        Ticket& other = *side[1 - k];
        Match m;
        init(m, MATCH);
        m.ticket = me.id;
        m.role = (uint8_t)k;
        m.peer_public.ip = other.pub.ip;
        m.peer_public.port = other.pub.port;
        m.peer_lan = other.lan;
        memcpy(m.session, sid, 16);
        memcpy(m.token, s.token[k], 16);
        me.matched = true;
        me.matched_at = t;
        me.match = m;
        send_to(m, me.pub);
        remove_from_quick(me.id);
        if (!me.room.empty()) g_rooms.erase(me.room);
    }
    g_stat_matches++;
}

static void try_quick_match(double t) {
    for (size_t i = 0; i < g_quick.size(); i++) {
        Ticket& a = g_tickets[g_quick[i]];
        for (size_t j = i + 1; j < g_quick.size(); j++) {
            Ticket& b = g_tickets[g_quick[j]];
            if (a.build != b.build || a.disc != b.disc || a.pub == b.pub) continue;
            pair(a, b, t);
            return; // one pair per packet keeps the loop short; the next keepalive pairs the rest
        }
    }
}

static void on_queue(const Queue& q, const Endpoint& from, double t) {
    auto it = g_tickets.find(q.ticket);
    if (it != g_tickets.end()) {
        Ticket& tk = it->second;
        if (!(tk.pub == from)) return; // tickets are bound to the address that created them
        tk.last = t;
        if (tk.matched) { send_to(tk.match, tk.pub); return; } // MATCH lost: repeat it
        send_status(tk, tk.mode == MODE_CREATE_ROOM ? ST_ROOM_OPEN : ST_SEARCHING);
        if (tk.mode == MODE_QUICK) try_quick_match(t);
        return;
    }
    if (g_tickets.size() >= MAX_TICKETS || q.ticket == 0) {
        Ticket tmp;
        tmp.id = q.ticket;
        tmp.pub = from;
        send_status(tmp, ST_BUSY);
        return;
    }
    Ticket tk;
    tk.id = q.ticket;
    tk.build = q.build;
    tk.disc = q.disc;
    tk.pub = from;
    tk.lan = q.lan;
    tk.mode = q.mode;
    tk.created = tk.last = t;
    if (q.mode == MODE_QUICK) {
        g_tickets[tk.id] = tk;
        g_quick.push_back(tk.id);
        send_status(tk, ST_SEARCHING);
        try_quick_match(t);
    } else if (q.mode == MODE_CREATE_ROOM) {
        if (g_rooms.size() >= MAX_ROOMS) { send_status(tk, ST_BUSY); return; }
        tk.room = new_room_code();
        if (tk.room.empty()) { send_status(tk, ST_BUSY); return; }
        g_rooms[tk.room] = tk.id;
        g_tickets[tk.id] = tk;
        send_status(tk, ST_ROOM_OPEN);
    } else if (q.mode == MODE_JOIN_ROOM) {
        char code[6] = {};
        memcpy(code, q.room, 5);
        for (char& c : code) c = (char)toupper((unsigned char)c);
        auto r = g_rooms.find(code);
        if (r == g_rooms.end() || !g_tickets.count(r->second)) { send_status(tk, ST_NO_SUCH_ROOM); return; }
        Ticket& host = g_tickets[r->second];
        if (host.build != tk.build || host.disc != tk.disc) { send_status(tk, ST_INCOMPATIBLE); return; }
        g_tickets[tk.id] = tk;
        pair(host, g_tickets[tk.id], t);
    }
}

static void on_cancel(const Cancel& c, const Endpoint& from) {
    auto it = g_tickets.find(c.ticket);
    if (it == g_tickets.end() || !(it->second.pub == from)) return;
    remove_from_quick(c.ticket);
    if (!it->second.room.empty()) g_rooms.erase(it->second.room);
    g_tickets.erase(it);
}

static void on_relay_bind(const RelayBind& b, const Endpoint& from, double t) {
    auto it = g_sessions.find(std::string((const char*)b.session, 16));
    if (it == g_sessions.end() || b.role > 1) return;
    Session& s = it->second;
    if (memcmp(s.token[b.role], b.token, 16) != 0) { g_stat_dropped++; return; }
    s.bound[b.role] = from;
    s.has[b.role] = true;
    s.last = t;
    RelayOk ok;
    init(ok, RELAY_OK);
    memcpy(ok.session, b.session, 16);
    send_to(ok, from);
}

static void on_relay_data(const uint8_t* buf, int n, const Endpoint& from, double t) {
    const RelayData* d = (const RelayData*)buf;
    if (d->role > 1) return;
    auto it = g_sessions.find(std::string((const char*)d->session, 16));
    if (it == g_sessions.end()) return;
    Session& s = it->second;
    int me = d->role, other = 1 - me;
    if (!s.has[me] || !(s.bound[me] == from) || !s.has[other]) { g_stat_dropped++; return; }
    if (!s.pps[me].take(RELAY_PPS, RELAY_PPS, t) || !s.bps[me].take(RELAY_BPS, RELAY_BPS, t, n)) { g_stat_dropped++; return; }
    s.last = t;
    sockaddr_in a = sa_of(s.bound[other]);
    sendto(g_sock, (const char*)buf, n, 0, (const sockaddr*)&a, sizeof a);
    g_stat_relayed += (uint64_t)n;
}

static void housekeeping(double t) {
    for (auto it = g_tickets.begin(); it != g_tickets.end();) {
        const Ticket& tk = it->second;
        bool dead = tk.matched ? t - tk.matched_at > MATCHED_KEEP_S
                               : t - tk.last > TICKET_IDLE_S || (tk.mode == MODE_CREATE_ROOM && t - tk.created > ROOM_TTL_S);
        if (dead) {
            remove_from_quick(tk.id);
            if (!tk.room.empty() && !tk.matched) g_rooms.erase(tk.room);
            it = g_tickets.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_sessions.begin(); it != g_sessions.end();) it = t - it->second.last > SESSION_IDLE_S ? g_sessions.erase(it) : std::next(it);
    if (g_limit.size() > MAX_LIMITED_IPS) g_limit.clear();
}

static bool open_socket(int port) {
#ifdef _WIN32
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = INADDR_ANY;
    return bind(g_sock, (sockaddr*)&a, sizeof a) == 0;
}

static bool wait_readable(int ms) {
#ifdef _WIN32
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(g_sock, &rs);
    timeval tv = {ms / 1000, (ms % 1000) * 1000};
    return select(0, &rs, nullptr, nullptr, &tv) > 0;
#else
    pollfd p = {g_sock, POLLIN, 0};
    return poll(&p, 1, ms) > 0;
#endif
}

static int healthcheck(int port) {
#ifdef _WIN32
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Ping p;
    init(p, PING);
    p.t = 0x48454C4F;
    Endpoint e = {htonl(INADDR_LOOPBACK), htons((uint16_t)port)};
    send_to(p, e);
    if (!wait_readable(1500)) return 1;
    uint8_t buf[64];
    int n = recv(g_sock, (char*)buf, sizeof buf, 0);
    const Ping* r = (const Ping*)buf;
    return n >= (int)sizeof(Ping) && r->h.magic == MAGIC && r->h.type == PONG && r->t == p.t ? 0 : 1;
}

int main(int argc, char** argv) {
    int port = getenv("TBMATCH_PORT") ? atoi(getenv("TBMATCH_PORT")) : DEFAULT_PORT;
    bool health = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--healthcheck")) health = true;
    }
    if (port < 1 || port > 65535) { fprintf(stderr, "bad port\n"); return 2; }
    if (health) return healthcheck(port);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (!open_socket(port)) { logf("cannot bind UDP port %d", port); return 1; }
    uint8_t probe;
    if (!random_bytes(&probe, 1)) { logf("no secure random source"); return 1; }
    logf("tbmatch protocol %d listening on UDP %d", VERSION, port);

    double next_house = now_s() + 1, next_stats = now_s() + 60;
    uint8_t buf[MAX_PACKET + 1];
    while (!g_stop) {
        if (wait_readable(200)) {
            for (int burst = 0; burst < 256; burst++) {
                sockaddr_in fa;
                socklen_t fl = sizeof fa;
#ifdef _WIN32
                u_long avail = 0;
                ioctlsocket(g_sock, FIONREAD, &avail);
                if (!avail && burst) break;
#endif
                int n = recvfrom(g_sock, (char*)buf, sizeof buf, 0, (sockaddr*)&fa, &fl);
                if (n < (int)sizeof(Header)) break;
                double t = now_s();
                const Header* h = (const Header*)buf;
                if (h->magic != MAGIC || n > MAX_PACKET) continue;
                Endpoint from = ep_of(fa);
                if (h->type == RELAY_DATA) {
                    if (n >= (int)sizeof(RelayData)) on_relay_data(buf, n, from, t);
                } else {
                    Bucket& b = g_limit.emplace(from.ip, Bucket{CTRL_BURST, t}).first->second;
                    if (!b.take(CTRL_RATE, CTRL_BURST, t)) { g_stat_dropped++; continue; }
                    if (h->version != VERSION) {
                        if (h->type == QUEUE && n >= (int)sizeof(Queue)) {
                            Ticket tmp;
                            tmp.id = ((const Queue*)buf)->ticket;
                            tmp.pub = from;
                            send_status(tmp, ST_BAD_VERSION);
                        }
                        continue;
                    }
                    switch (h->type) {
                    case QUEUE: if (n >= (int)sizeof(Queue)) on_queue(*(const Queue*)buf, from, t); break;
                    case CANCEL: if (n >= (int)sizeof(Cancel)) on_cancel(*(const Cancel*)buf, from); break;
                    case MATCH_ACK: break; // matched tickets simply expire
                    case RELAY_BIND: if (n >= (int)sizeof(RelayBind)) on_relay_bind(*(const RelayBind*)buf, from, t); break;
                    case PING:
                        if (n >= (int)sizeof(Ping)) {
                            Ping p = *(const Ping*)buf;
                            p.h.type = PONG;
                            send_to(p, from);
                        }
                        break;
                    default: break;
                    }
                }
#ifndef _WIN32
                if (!wait_readable(0)) break;
#endif
            }
        }
        double t = now_s();
        if (t >= next_house) { housekeeping(t); next_house = t + 1; }
        if (t >= next_stats) {
            logf("searching %zu, rooms %zu, sessions %zu, matches %llu, relayed %.1f MB, dropped %llu", g_quick.size(), g_rooms.size(),
                 g_sessions.size(), (unsigned long long)g_stat_matches, g_stat_relayed / 1048576.0, (unsigned long long)g_stat_dropped);
            next_stats = t + 60;
        }
    }
    logf("shutting down");
    sock_close(g_sock);
    return 0;
}
