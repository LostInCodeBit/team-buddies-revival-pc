// netplay.cpp - online play for two players: delay-based lockstep over UDP, peer to peer.
//
// The emulation is deterministic (same disc + same inputs = same RAM, see the status log's "ram"
// fingerprint), so both machines run the whole game and only exchange pad input. Every frame each
// side schedules its local pad state `delay` frames ahead, sends the recent ones (redundantly, so a
// lost packet costs nothing), and does not run frame N until it has the other player's input for N.
//
// Session: TeamBuddies.exe --host [port]   or   --join <address>[:port]   (optional --delay N)
//   1. client -> HELLO (protocol, build, disc fingerprint); host checks and answers WELCOME with the
//      settings that change emulation (CPU speed, widescreen hack). The client measures the ping.
//   2. host -> both memory cards (the client uses them in memory only, its own cards stay untouched)
//   3. client -> READY with the input delay; host -> START; both boot the game at frame 0.
//   Every 120 frames each side sends a RAM fingerprint; a mismatch ends the session ("desync").
//   The host asks the router for a UDP port forward (UPnP) in the background.
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <windows.h>
#include <natupnp.h>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <string>
#include <thread>
#include "psx.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

extern bool g_gpu_widescreen_hack; // gte.cpp
extern u64 g_dbg_frames;           // sched.cpp
void np_status_open();
void np_status_text(const char* t);
bool np_status_cancelled();
void np_status_close();

namespace {

const u32 MAGIC = 0x504E4254; // "TBNP"
const u16 PROTO = 1;
const int DEFAULT_PORT = 7845;
const int CARD_BYTES = 128 * 1024, CHUNK = 1024, CHUNKS = 2 * CARD_BYTES / CHUNK;
const int RING = 1024;          // input history per player (frames)
const int HASH_EVERY = 120;     // frames between RAM fingerprints
const DWORD PEER_TIMEOUT = 10000;

enum : u8 { P_HELLO = 1, P_WELCOME, P_REJECT, P_CARD, P_NEED, P_READY, P_START, P_INPUT, P_BYE };

#pragma pack(push, 1)
struct NetInput { u16 buttons; u8 lx, ly, rx, ry; };
struct Hdr { u32 magic; u8 type; };
struct Hello { Hdr h; u16 proto; u64 build, disc; u32 t; };
struct Welcome { Hdr h; u16 proto; u8 widescreen; double cpi; u32 t_echo; };
struct Reject { Hdr h; char reason[120]; };
struct CardChunk { Hdr h; u16 index; u8 data[CHUNK]; };
struct Need { Hdr h; u8 bits[CHUNKS / 8]; };
struct Ready { Hdr h; u8 delay; };
struct Start { Hdr h; u8 delay; };
struct Bye { Hdr h; char reason[96]; };
struct InputPkt {
    Hdr h;
    u8 slot, count;
    u32 first;              // frame of in[0]
    u32 hash_frame;         // latest RAM fingerprint (0 = none yet)
    u64 hash;
    NetInput in[16];
};
#pragma pack(pop)

SOCKET s_sock = INVALID_SOCKET;
WSAEVENT s_evt = WSA_INVALID_EVENT;   // signalled when a datagram arrives
sockaddr_in s_peer = {};
bool s_active = false;
int s_slot = 0, s_delay = 3;
u32 s_frame = 0;
NetInput s_in[2][RING];
u32 s_in_frame[2][RING];
u32 s_local_upto = 0;          // highest frame with local input scheduled
u64 s_my_hash[64]; u32 s_my_hash_frame[64];
u32 s_peer_hash_frame = 0; u64 s_peer_hash = 0;
u32 s_last_hash_frame = 0;
DWORD s_last_rx = 0;
double s_saved_cpi; bool s_saved_widescreen, s_saved_multitap;
std::atomic<bool> s_upnp_done{false};
std::string s_upnp_info;
int s_port = DEFAULT_PORT;

u64 fnv(const void* p, size_t n, u64 h = 1469598103934665603ull) {
    const u8* b = (const u8*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
u64 build_id() {
    u64 h = fnv(&PROTO, sizeof PROTO);
    for (int i = 0; i < g_rec_func_count; i += 7) h = fnv(&g_rec_funcs[i].addr, 4, h);
    return fnv(&g_rec_func_count, sizeof g_rec_func_count, h);
}
u64 ram_hash() {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < 0x200000; i += 8) { u64 w; memcpy(&w, g_ram + i, 8); h = (h ^ w) * 1099511628211ull; }
    return h;
}

template <class T> void init_hdr(T& p, u8 type) { memset(&p, 0, sizeof p); p.h.magic = MAGIC; p.h.type = type; }
// TB_NET_LOSS=percent drops outgoing datagrams (test harness: packet loss)
int loss_percent() { static int v = getenv("TB_NET_LOSS") ? atoi(getenv("TB_NET_LOSS")) : 0; return v; }
template <class T> void send_to(const T& p, const sockaddr_in& to) {
    static u32 rng = 12345;
    if (loss_percent() && (int)((rng = rng * 1103515245u + 12345u) >> 16) % 100 < loss_percent()) return;
    sendto(s_sock, (const char*)&p, sizeof p, 0, (const sockaddr*)&to, sizeof to);
}
template <class T> void send_peer(const T& p) { send_to(p, s_peer); }

// one datagram, non-blocking; returns its size (0 = none)
int recv_one(u8* buf, int cap, sockaddr_in& from) {
    int fl = sizeof from;
    int n = recvfrom(s_sock, (char*)buf, cap, 0, (sockaddr*)&from, &fl);
    if (n < (int)sizeof(Hdr)) return 0;
    Hdr h;
    memcpy(&h, buf, sizeof h);
    return h.magic == MAGIC ? n : 0;
}
bool same_addr(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }

bool open_socket(int bind_port) {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) return false;
    s_evt = WSACreateEvent();
    WSAEventSelect(s_sock, s_evt, FD_READ); // also makes the socket non-blocking
    // ignore ICMP "port unreachable" resets while the peer is not up yet
    BOOL off = FALSE;
    DWORD ret;
    WSAIoctl(s_sock, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)bind_port);
    a.sin_addr.s_addr = INADDR_ANY;
    return bind(s_sock, (sockaddr*)&a, sizeof a) == 0;
}

// waits a little while keeping the window alive; false = the user closed the window
// sleeps until a datagram or a window message arrives (or ms pass); false = the user closed the window
bool s_window_closed = false;
bool idle(DWORD ms) {
    if (!platform_pump_messages()) { s_window_closed = true; return false; }
    if (np_status_cancelled()) return false;
    MsgWaitForMultipleObjects(1, &s_evt, FALSE, ms, QS_ALLINPUT);
    WSAResetEvent(s_evt); // recv_one drains the socket; FD_READ re-signals while data is left
    return platform_pump_messages();
}

std::string local_ip() {
    // the interface Windows would route to the internet (connect() on UDP sends nothing)
    SOCKET t = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    std::string ip = "0.0.0.0";
    if (connect(t, (sockaddr*)&a, sizeof a) == 0) {
        sockaddr_in me = {};
        int l = sizeof me;
        getsockname(t, (sockaddr*)&me, &l);
        char b[32];
        inet_ntop(AF_INET, &me.sin_addr, b, sizeof b);
        ip = b;
    }
    closesocket(t);
    return ip;
}

// router port forward through UPnP (best effort, background thread)
void upnp_map(int port, bool add) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUPnPNAT* nat = nullptr;
    IStaticPortMappingCollection* coll = nullptr;
    std::string info = "no UPnP, forward the UDP port yourself for internet play";
    if (SUCCEEDED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), (void**)&nat)) &&
        SUCCEEDED(nat->get_StaticPortMappingCollection(&coll)) && coll) {
        BSTR proto = SysAllocString(L"UDP");
        if (add) {
            std::string ip = local_ip();
            std::wstring wip(ip.begin(), ip.end());
            BSTR bip = SysAllocString(wip.c_str()), desc = SysAllocString(L"Team Buddies online");
            IStaticPortMapping* m = nullptr;
            if (SUCCEEDED(coll->Add(port, proto, port, bip, VARIANT_TRUE, desc, &m)) && m) {
                BSTR ext = nullptr;
                info = "port forwarded";
                if (SUCCEEDED(m->get_ExternalIPAddress(&ext)) && ext) {
                    char b[64];
                    WideCharToMultiByte(CP_UTF8, 0, ext, -1, b, sizeof b, nullptr, nullptr);
                    info = std::string("public address ") + b + ":" + std::to_string(port);
                    SysFreeString(ext);
                }
                m->Release();
            } else {
                info = "UPnP refused the port forward";
            }
            SysFreeString(bip);
            SysFreeString(desc);
        } else {
            coll->Remove(port, proto);
        }
        SysFreeString(proto);
        coll->Release();
    }
    if (nat) nat->Release();
    CoUninitialize();
    if (add) { s_upnp_info = info; s_upnp_done = true; plog("netplay: %s\n", info.c_str()); }
}

void set_title(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    platform_set_title(b);
    const char* prefix = "Team Buddies - ";
    np_status_text(strncmp(b, prefix, strlen(prefix)) == 0 ? b + strlen(prefix) : b);
}

void fail(const char* msg) {
    plog("netplay: %s\n", msg);
    MessageBoxA(nullptr, msg, "Team Buddies online", MB_OK | MB_ICONWARNING);
}

int delay_for_rtt(DWORD rtt) {
    int d = (int)((rtt / 2.0) / 16.68 + 0.999) + 1; // one-way latency in frames + one frame of jitter margin
    return d < 2 ? 2 : d > 12 ? 12 : d;
}

void apply_session_settings(double cpi, bool widescreen) {
    s_saved_cpi = g_settings.cpu_cpi;
    s_saved_widescreen = g_settings.widescreen;
    s_saved_multitap = g_settings.multitap;
    g_settings.cpu_cpi = cpi;
    g_settings.widescreen = widescreen;
    g_gpu_widescreen_hack = widescreen;
    g_settings.multitap = false; // two players, one per port
}

// ---------------------------------------------------------------- host
bool host_session(int port, int forced_delay, u64 disc) {
    if (!open_socket(port)) { fail("Cannot open the UDP port for hosting (already in use?)."); return false; }
    std::thread([port] { upnp_map(port, true); }).detach();
    std::string lan = local_ip();
    u8 buf[2048];
    sockaddr_in from;
    // 1. wait for a player
    bool got = false;
    for (DWORD t0 = GetTickCount(); !got;) {
        set_title("Team Buddies - Hosting on %s:%d. Router: %s. Waiting for player 2...", lan.c_str(), port,
                  s_upnp_done ? s_upnp_info.c_str() : "checking");
        while (int n = recv_one(buf, sizeof buf, from)) {
            if (buf[4] != P_HELLO || n < (int)sizeof(Hello)) continue;
            Hello hi;
            memcpy(&hi, buf, sizeof hi);
            const char* why = hi.proto != PROTO ? "different online protocol version"
                            : hi.build != build_id() ? "different game build (update both copies)"
                            : hi.disc != disc ? "different disc image" : nullptr;
            if (why) {
                Reject r;
                init_hdr(r, P_REJECT);
                snprintf(r.reason, sizeof r.reason, "The host refused: %s.", why);
                send_to(r, from);
                plog("netplay: refused a player: %s\n", why);
                continue;
            }
            s_peer = from;
            Welcome w;
            init_hdr(w, P_WELCOME);
            w.proto = PROTO;
            w.cpi = g_settings.cpu_cpi;
            w.widescreen = g_settings.widescreen;
            w.t_echo = hi.t;
            send_peer(w);
            got = true;
        }
        if (!got && !idle(10)) return false;
        (void)t0;
    }
    char peer[32];
    inet_ntop(AF_INET, &s_peer.sin_addr, peer, sizeof peer);
    plog("netplay: player 2 connected from %s\n", peer);
    // 2. memory cards, 3. ready/start
    const std::vector<u8>* cards[2] = {&card_image(0), &card_image(1)};
    int delay = 0;
    DWORD last = GetTickCount();
    while (!delay) {
        set_title("Team Buddies - Player 2 joined from %s. Sending memory cards...", peer);
        while (int n = recv_one(buf, sizeof buf, from)) {
            if (!same_addr(from, s_peer)) continue;
            if (buf[4] == P_HELLO) { // our WELCOME was lost
                Hello hi;
                memcpy(&hi, buf, sizeof hi);
                Welcome w;
                init_hdr(w, P_WELCOME);
                w.proto = PROTO;
                w.cpi = g_settings.cpu_cpi;
                w.widescreen = g_settings.widescreen;
                w.t_echo = hi.t;
                send_peer(w);
            } else if (buf[4] == P_NEED && n >= (int)sizeof(Need)) {
                Need nd;
                memcpy(&nd, buf, sizeof nd);
                int sent = 0;
                for (int i = 0; i < CHUNKS && sent < 48; i++) {
                    if (!(nd.bits[i / 8] & (1 << (i % 8)))) continue;
                    CardChunk c;
                    init_hdr(c, P_CARD);
                    c.index = (u16)i;
                    const std::vector<u8>& img = *cards[i / (CARD_BYTES / CHUNK)];
                    size_t off = (size_t)(i % (CARD_BYTES / CHUNK)) * CHUNK;
                    if (img.size() >= off + CHUNK) memcpy(c.data, &img[off], CHUNK);
                    send_peer(c);
                    sent++;
                }
            } else if (buf[4] == P_READY && n >= (int)sizeof(Ready)) {
                Ready r;
                memcpy(&r, buf, sizeof r);
                delay = forced_delay > 0 ? forced_delay : r.delay;
            }
            last = GetTickCount();
        }
        if (GetTickCount() - last > PEER_TIMEOUT) { fail("Player 2 stopped responding while joining."); return false; }
        if (!delay && !idle(2)) return false;
    }
    s_delay = delay;
    s_slot = 0;
    apply_session_settings(g_settings.cpu_cpi, g_settings.widescreen); // host keeps its own, minus the multitap
    Start st;
    init_hdr(st, P_START);
    st.delay = (u8)delay;
    send_peer(st); // repeated from netplay_sync until the client's first input arrives
    return true;
}

// ---------------------------------------------------------------- client
bool join_session(const std::string& target, int forced_delay, u64 disc) {
    std::string hostname = target;
    int port = DEFAULT_PORT;
    size_t c = target.rfind(':');
    if (c != std::string::npos) { hostname = target.substr(0, c); port = atoi(target.c_str() + c + 1); }
    if (!open_socket(0)) { fail("Cannot open a UDP socket."); return false; }
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(hostname.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        fail(("Cannot find host \"" + hostname + "\".").c_str());
        return false;
    }
    memcpy(&s_peer, res->ai_addr, sizeof s_peer);
    freeaddrinfo(res);
    u8 buf[2048];
    sockaddr_in from;
    // 1. hello until welcome; keep the best ping of a few rounds
    DWORD best = 0xFFFFFFFF, t0 = GetTickCount(), last_hello = 0;
    int welcomes = 0;
    double cpi = 2.0;
    bool widescreen = false;
    while (welcomes < 5) {
        set_title("Team Buddies - Connecting to %s:%d...", hostname.c_str(), port);
        DWORD now = GetTickCount();
        if (now - last_hello > 150) {
            Hello hi;
            init_hdr(hi, P_HELLO);
            hi.proto = PROTO;
            hi.build = build_id();
            hi.disc = disc;
            hi.t = now;
            send_peer(hi);
            last_hello = now;
        }
        while (int n = recv_one(buf, sizeof buf, from)) {
            if (!same_addr(from, s_peer)) continue;
            if (buf[4] == P_REJECT && n >= (int)sizeof(Reject)) {
                Reject r;
                memcpy(&r, buf, sizeof r);
                r.reason[sizeof r.reason - 1] = 0;
                fail(r.reason);
                return false;
            }
            if (buf[4] == P_WELCOME && n >= (int)sizeof(Welcome)) {
                Welcome w;
                memcpy(&w, buf, sizeof w);
                DWORD rtt = GetTickCount() - w.t_echo;
                if (rtt < best) best = rtt;
                cpi = w.cpi;
                widescreen = w.widescreen != 0;
                welcomes++;
                last_hello = 0; // next ping right away
            }
        }
        if (GetTickCount() - t0 > PEER_TIMEOUT && !welcomes) {
            fail("No answer from the host. Check the address, that the host is waiting, and the host's port forward / firewall.");
            return false;
        }
        if (!idle(2)) return false;
    }
    int delay = forced_delay > 0 ? forced_delay : delay_for_rtt(best);
    plog("netplay: connected, ping %lu ms, input delay %d frames\n", best, delay);
    // 2. memory cards
    static std::vector<u8> img(2 * CARD_BYTES);
    bool have[CHUNKS] = {};
    int missing = CHUNKS;
    DWORD last_need = 0, last_rx = GetTickCount();
    while (missing) {
        set_title("Team Buddies - Receiving memory cards (%d%%)...", (CHUNKS - missing) * 100 / CHUNKS);
        while (int n = recv_one(buf, sizeof buf, from)) {
            if (!same_addr(from, s_peer)) continue;
            last_rx = GetTickCount();
            if (buf[4] != P_CARD || n < (int)sizeof(CardChunk)) continue;
            CardChunk ch;
            memcpy(&ch, buf, sizeof ch);
            if (ch.index < CHUNKS && !have[ch.index]) {
                memcpy(&img[(size_t)ch.index * CHUNK], ch.data, CHUNK);
                have[ch.index] = true;
                missing--;
            }
        }
        DWORD now = GetTickCount();
        if (missing && now - last_need > 60) {
            Need nd;
            init_hdr(nd, P_NEED);
            for (int i = 0; i < CHUNKS; i++) if (!have[i]) nd.bits[i / 8] |= (u8)(1 << (i % 8));
            send_peer(nd);
            last_need = now;
        }
        if (now - last_rx > PEER_TIMEOUT) { fail("The host stopped responding while sending the memory cards."); return false; }
        if (missing && !idle(1)) return false;
    }
    card_replace(0, &img[0]);
    card_replace(1, &img[CARD_BYTES]);
    // 3. ready until start
    DWORD last_ready = 0;
    last_rx = GetTickCount();
    for (;;) {
        set_title("Team Buddies - Waiting for the host to start...");
        DWORD now = GetTickCount();
        if (now - last_ready > 100) {
            Ready r;
            init_hdr(r, P_READY);
            r.delay = (u8)delay;
            send_peer(r);
            last_ready = now;
        }
        bool started = false;
        while (int n = recv_one(buf, sizeof buf, from)) {
            if (!same_addr(from, s_peer)) continue;
            last_rx = now;
            if (buf[4] == P_START && n >= (int)sizeof(Start)) { Start st; memcpy(&st, buf, sizeof st); delay = st.delay; started = true; }
            if (buf[4] == P_INPUT) started = true; // START lost, but the host is already running
        }
        if (started) break;
        if (now - last_rx > PEER_TIMEOUT) { fail("The host stopped responding."); return false; }
        if (!idle(2)) return false;
    }
    apply_session_settings(cpi, widescreen);
    s_delay = delay;
    s_slot = 1;
    return true;
}

// ---------------------------------------------------------------- per frame
void store(int slot, u32 frame, const NetInput& in) {
    s_in[slot][frame % RING] = in;
    s_in_frame[slot][frame % RING] = frame;
}
bool have_input(int slot, u32 frame) { return s_in_frame[slot][frame % RING] == frame; }

void send_inputs() {
    InputPkt p;
    init_hdr(p, P_INPUT);
    p.slot = (u8)s_slot;
    u32 last = s_local_upto;
    u32 first = last >= 15 ? last - 15 : 0;
    p.first = first;
    p.count = (u8)(last - first + 1);
    for (u32 f = first; f <= last; f++) p.in[f - first] = s_in[s_slot][f % RING];
    p.hash_frame = s_last_hash_frame;
    p.hash = s_last_hash_frame ? s_my_hash[(s_last_hash_frame / HASH_EVERY) % 64] : 0;
    send_peer(p);
}

// why = shown here; peer_why = what the other player is told (nullptr: they already know)
void end_session(const char* why, const char* peer_why = "the other player left") {
    plog("netplay: session ended: %s\n", why);
    if (peer_why) {
        Bye bye;
        init_hdr(bye, P_BYE);
        snprintf(bye.reason, sizeof bye.reason, "%s", peer_why);
        for (int i = 0; i < 3; i++) send_to(bye, s_peer);
    }
    s_active = false;
    set_title("Team Buddies - Offline (%s)", why);
    std::string msg = std::string("Online session ended: ") + why + ".\nThe game continues offline.";
    std::thread([msg] { MessageBoxA(nullptr, msg.c_str(), "Team Buddies online", MB_OK | MB_ICONINFORMATION); }).detach(); // the game keeps running
}

void check_hashes() {
    if (!s_peer_hash_frame) return;
    u32 f = s_peer_hash_frame;
    int k = (f / HASH_EVERY) % 64;
    if (s_my_hash_frame[k] != f) return; // not there yet (or too old)
    if (s_my_hash[k] != s_peer_hash) {
        char b[128];
        snprintf(b, sizeof b, "desync at frame %u (the two games diverged)", f);
        end_session(b, b);
    }
    s_peer_hash_frame = 0;
}

void receive() {
    u8 buf[2048];
    sockaddr_in from;
    while (int n = recv_one(buf, sizeof buf, from)) {
        if (!same_addr(from, s_peer)) continue;
        s_last_rx = GetTickCount();
        if (buf[4] == P_BYE) {
            Bye bye;
            memset(&bye, 0, sizeof bye);
            memcpy(&bye, buf, n < (int)sizeof bye ? n : sizeof bye);
            bye.reason[sizeof bye.reason - 1] = 0;
            end_session(bye.reason[0] ? bye.reason : "the other player left", nullptr);
            return;
        }
        if (buf[4] == P_READY && s_slot == 0) { // START was lost
            Start st;
            init_hdr(st, P_START);
            st.delay = (u8)s_delay;
            send_peer(st);
            continue;
        }
        if (buf[4] != P_INPUT || n < (int)sizeof(InputPkt)) continue;
        InputPkt p;
        memcpy(&p, buf, sizeof p);
        int other = 1 - s_slot;
        if (p.slot != other || p.count > 16) continue;
        for (int i = 0; i < p.count; i++) {
            u32 f = p.first + i;
            if (f + RING / 2 > s_frame && !have_input(other, f)) store(other, f, p.in[i]);
        }
        if (p.hash_frame && p.hash_frame > s_peer_hash_frame) { s_peer_hash_frame = p.hash_frame; s_peer_hash = p.hash; }
    }
}

} // namespace

// ---------------------------------------------------------------- interface
bool netplay_active() { return s_active; }
int netplay_local_slot() { return s_active ? s_slot : 0; }

bool netplay_start(int argc, char** argv, const std::vector<u8>& exe) {
    int mode = 0, port = DEFAULT_PORT, delay = 0;
    std::string target;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--host") { mode = 1; if (i + 1 < argc && isdigit((u8)argv[i + 1][0])) port = atoi(argv[++i]); }
        else if (a == "--join" && i + 1 < argc) { mode = 2; target = argv[++i]; }
        else if (a == "--delay" && i + 1 < argc) delay = atoi(argv[++i]);
    }
    if (!mode) return true;
    u64 disc = fnv(exe.data(), exe.size());
    memset(s_in_frame, 0xFF, sizeof s_in_frame);
    np_status_open();
    bool ok = mode == 1 ? host_session(port, delay, disc) : join_session(target, delay, disc);
    np_status_close();
    if (!ok) {
        if (s_sock != INVALID_SOCKET) closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        if (mode == 1) std::thread([port] { upnp_map(port, false); }).detach();
        platform_set_title("Team Buddies");
        return !s_window_closed; // cancelled or failed: play offline; window closed: quit
    }
    s_port = mode == 1 ? port : 0;
    s_active = true;
    s_frame = 0;
    s_local_upto = 0;
    s_last_rx = GetTickCount();
    // the first `delay` frames have no input from anyone yet: neutral pads
    NetInput neutral = {0, 0x80, 0x80, 0x80, 0x80};
    for (u32 f = 0; f < (u32)s_delay; f++) { store(0, f, neutral); store(1, f, neutral); }
    s_local_upto = s_delay ? s_delay - 1 : 0;
    plog("netplay: started as player %d, input delay %d frames\n", s_slot + 1, s_delay);
    return true;
}

void netplay_sync() {
    if (!s_active) return;
    // 1. schedule this machine's pad for frame + delay
    const PadState& lp = g_pads[0];
    NetInput li = {lp.buttons, lp.lx, lp.ly, lp.rx, lp.ry};
    u32 target = s_frame + s_delay;
    store(s_slot, target, li);
    s_local_upto = target;
    // 2. RAM fingerprint at fixed frames (state at this frame boundary is the same on both machines)
    if (s_frame && s_frame % HASH_EVERY == 0) {
        int k = (s_frame / HASH_EVERY) % 64;
        s_my_hash[k] = ram_hash();
        s_my_hash_frame[k] = s_frame;
        s_last_hash_frame = s_frame;
    }
    send_inputs();
    // 3. wait for the other player's input for this frame
    int other = 1 - s_slot;
    DWORD wait_start = GetTickCount(), last_send = wait_start;
    bool warned = false;
    for (;;) {
        receive();
        if (!s_active) break;
        check_hashes();
        if (!s_active) break;
        if (have_input(other, s_frame)) break;
        DWORD now = GetTickCount();
        if (now - last_send > 30) { send_inputs(); last_send = now; }
        if (now - wait_start > 1000 && !warned) { set_title("Team Buddies - Waiting for the other player..."); warned = true; }
        if (now - s_last_rx > PEER_TIMEOUT) { end_session("connection lost"); break; }
        if (!idle(5)) return; // closing: the quit path ends the session
    }
    if (warned && s_active) set_title("Team Buddies - Online");
    // 4. both players' pads for this frame
    if (s_active) {
        for (int p = 0; p < 4; p++) {
            g_pads[p].buttons = 0;
            g_pads[p].connected = p < 2;
            g_pads[p].lx = g_pads[p].ly = g_pads[p].rx = g_pads[p].ry = 0x80;
        }
        for (int p = 0; p < 2; p++) {
            const NetInput& in = s_in[p][s_frame % RING];
            g_pads[p].buttons = in.buttons;
            g_pads[p].lx = in.lx; g_pads[p].ly = in.ly; g_pads[p].rx = in.rx; g_pads[p].ry = in.ry;
        }
    }
    if (s_frame % 600 == 0 && g_settings.log_level >= 2) plog("netplay: frame %u (vblank %llu)\n", s_frame, g_dbg_frames);
    // TB_NET_DESYNC=frame corrupts one RAM byte on the client (test harness: desync detection)
    static u32 corrupt_at = getenv("TB_NET_DESYNC") ? (u32)atoi(getenv("TB_NET_DESYNC")) : 0;
    if (corrupt_at && s_slot == 1 && s_frame == corrupt_at) g_ram[0x180] ^= 0x5A; // unused kernel area: harmless, but the fingerprint must differ
    s_frame++;
    static DWORD title_at = 0;
    if (s_active && GetTickCount() - title_at > 2000) {
        title_at = GetTickCount();
        set_title("Team Buddies - Online as player %d (input delay %d frames)", s_slot + 1, s_delay);
    }
}

void netplay_shutdown() {
    if (s_sock == INVALID_SOCKET) return;
    if (s_active) {
        Hdr bye = {MAGIC, P_BYE};
        for (int i = 0; i < 3; i++) send_to(bye, s_peer);
        s_active = false;
    }
    // the session overrode settings that change emulation: do not let them reach the ini
    g_settings.cpu_cpi = s_saved_cpi;
    g_settings.widescreen = s_saved_widescreen;
    g_settings.multitap = s_saved_multitap;
    if (s_port) upnp_map(s_port, false);
    closesocket(s_sock);
    s_sock = INVALID_SOCKET;
}
