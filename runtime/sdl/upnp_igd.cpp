// upnp_igd.cpp - router port forward for online play outside Windows (netplay.cpp's upnp_map; Windows has its
// UPnP NAT API): finds the internet gateway with SSDP, then asks it over SOAP (UPnP IGD: WANIPConnection or
// WANPPPConnection). Best effort and bounded: every network step has a timeout and every reply a size cap.
// TB_UPNP_LOCATION=http://... skips the discovery (test harness: a stand-in gateway).
#ifndef _WIN32
#include "../psx.h"
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

const int STEP_MS = 3000;             // per request and for the discovery
const size_t MAX_REPLY = 256 * 1024;

struct Url { std::string host; int port = 80; std::string path = "/"; };

long long now_ms() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool parse_url(const std::string& u, Url& out) {
    if (lower(u.substr(0, 7)) != "http://") return false;
    const size_t slash = u.find('/', 7);
    std::string hostport = u.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
    out.path = slash == std::string::npos ? "/" : u.substr(slash);
    out.port = 80;
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.port = atoi(hostport.c_str() + colon + 1);
        hostport.resize(colon);
    }
    out.host = hostport;
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

// a header's value in an HTTP or SSDP message (any case)
std::string header(const std::string& msg, const std::string& name) {
    const std::string l = lower(msg), key = lower(name) + ":";
    for (size_t p = 0; p < l.size();) {
        size_t e = l.find('\n', p);
        if (e == std::string::npos) e = l.size();
        if (l.compare(p, key.size(), key) == 0) return trim(msg.substr(p + key.size(), e - p - key.size()));
        p = e + 1;
    }
    return "";
}

// the text of the first <name> element
std::string element(const std::string& xml, const std::string& name) {
    const size_t a = xml.find("<" + name + ">");
    if (a == std::string::npos) return "";
    const size_t s = a + name.size() + 2, e = xml.find("</" + name + ">", s);
    return e == std::string::npos ? "" : trim(xml.substr(s, e - s));
}

std::string unchunk(const std::string& b) {
    std::string out;
    for (size_t p = 0; p < b.size();) {
        const size_t e = b.find("\r\n", p);
        if (e == std::string::npos) break;
        const unsigned long n = strtoul(b.c_str() + p, nullptr, 16);
        p = e + 2;
        if (n == 0 || n > b.size() - p) break;
        out.append(b, p, n);
        p += n + 2;
    }
    return out;
}

// one HTTP exchange (Connection: close): the status code (0 = no proper answer) and the body
int http(const Url& u, const std::string& req, std::string& body) {
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) return 0;
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { freeaddrinfo(res); return 0; }
    fcntl(s, F_SETFD, FD_CLOEXEC);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    const long long deadline = now_ms() + STEP_MS;
    auto wait = [&](short ev) {
        pollfd p = {s, ev, 0};
        const long long left = deadline - now_ms();
        return left > 0 && poll(&p, 1, (int)left) > 0;
    };
    bool ok = connect(s, res->ai_addr, res->ai_addrlen) == 0 || (errno == EINPROGRESS && wait(POLLOUT));
    freeaddrinfo(res);
    if (ok) {
        int err = 0;
        socklen_t l = sizeof err;
        ok = getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l) == 0 && err == 0;
    }
    for (size_t sent = 0; ok && sent < req.size();) {
        const ssize_t n = send(s, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n > 0) sent += (size_t)n;
        else if (!(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait(POLLOUT))) ok = false;
    }
    std::string resp;
    while (ok && resp.size() < MAX_REPLY) {
        char buf[4096];
        const ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n > 0) resp.append(buf, (size_t)n);
        else if (n == 0 || !((errno == EAGAIN || errno == EWOULDBLOCK) && wait(POLLIN))) break;
    }
    close(s);
    const size_t sp = resp.find(' '), end = resp.find("\r\n\r\n");
    if (resp.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos || end == std::string::npos) return 0;
    body = resp.substr(end + 4);
    if (lower(header(resp.substr(0, end), "Transfer-Encoding")).find("chunked") != std::string::npos) body = unchunk(body);
    return atoi(resp.c_str() + sp + 1);
}

// SSDP: where the gateway's description is
std::string discover() {
    if (const char* e = getenv("TB_UPNP_LOCATION")) return e;
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "";
    fcntl(s, F_SETFD, FD_CLOEXEC);
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                           "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
        const std::string m = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
        sendto(s, m.data(), m.size(), 0, (const sockaddr*)&to, sizeof to);
    }
    std::string loc;
    for (const long long deadline = now_ms() + STEP_MS; loc.empty();) {
        const long long left = deadline - now_ms();
        pollfd p = {s, POLLIN, 0};
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
        char buf[2048];
        const ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) continue;
        const std::string r(buf, (size_t)n), st = lower(header(r, "ST"));
        if (st.find("internetgatewaydevice") != std::string::npos || st.find("wanipconnection") != std::string::npos ||
            st.find("wanpppconnection") != std::string::npos)
            loc = header(r, "LOCATION");
    }
    close(s);
    return loc;
}

// in the gateway's description: the control URL and type of its WANIPConnection (else WANPPPConnection) service
bool find_control(const std::string& xml, const Url& location, Url& control, std::string& type) {
    for (const char* kind : {"WANIPConnection:", "WANPPPConnection:"})
        for (size_t pos = 0; (pos = xml.find("<service>", pos)) != std::string::npos;) {
            const size_t end = xml.find("</service>", pos);
            if (end == std::string::npos) break;
            const std::string svc = xml.substr(pos, end - pos), t = element(svc, "serviceType"), ctl = element(svc, "controlURL");
            pos = end;
            if (t.find(kind) == std::string::npos || ctl.empty()) continue;
            if (lower(ctl.substr(0, 7)) == "http://") {
                if (!parse_url(ctl, control)) continue;
            } else { // relative: to URLBase when the description has one, else to where it came from
                const std::string base = element(xml, "URLBase");
                if (base.empty() || !parse_url(base, control)) control = location;
                control.path = ctl[0] == '/' ? ctl : "/" + ctl;
            }
            type = t;
            return true;
        }
    return false;
}

int soap(const Url& c, const std::string& type, const std::string& action, const std::string& args, std::string& reply) {
    const std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + type +
                             "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    const std::string req = "POST " + c.path + " HTTP/1.1\r\nHost: " + c.host + ":" + std::to_string(c.port) +
                            "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + type + "#" + action +
                            "\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    return http(c, req, reply);
}

std::mutex s_lock;     // the forward and its removal (another thread) one at a time
bool s_found = false;  // the gateway answered the last discovery
Url s_control;
std::string s_type;

} // namespace

// add: forward UDP `port` to local_ip; the text for the host's status ("public address ...", or why not).
// !add: remove the forward made before.
std::string upnp_igd_forward(int port, bool add, const std::string& local_ip) {
    std::lock_guard<std::mutex> guard(s_lock);
    const std::string p = std::to_string(port);
    std::string reply;
    if (!add) {
        if (s_found)
            soap(s_control, s_type, "DeletePortMapping",
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + p + "</NewExternalPort><NewProtocol>UDP</NewProtocol>", reply);
        return "";
    }
    s_found = false;
    const std::string loc = discover();
    Url u;
    std::string desc;
    if (!loc.empty() && parse_url(loc, u) &&
        http(u, "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) + "\r\nConnection: close\r\n\r\n", desc) == 200)
        s_found = find_control(desc, u, s_control, s_type);
    if (!s_found) return "no UPnP, forward the UDP port yourself for internet play";
    auto map = [&](const char* lease) {
        return soap(s_control, s_type, "AddPortMapping",
                    "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + p + "</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                    "<NewInternalPort>" + p + "</NewInternalPort><NewInternalClient>" + local_ip + "</NewInternalClient>"
                    "<NewEnabled>1</NewEnabled><NewPortMappingDescription>Team Buddies online</NewPortMappingDescription>"
                    "<NewLeaseDuration>" + lease + "</NewLeaseDuration>", reply) == 200;
    };
    // without an end, like Windows' forward; gateways that take only limited leases (IGD 2) get a day
    if (!map("0") && !map("86400")) return "UPnP refused the port forward";
    if (soap(s_control, s_type, "GetExternalIPAddress", "", reply) == 200) {
        const std::string ip = element(reply, "NewExternalIPAddress");
        bool plain = !ip.empty() && ip.size() < 46;
        for (char c : ip) plain = plain && (isxdigit((unsigned char)c) || c == '.' || c == ':');
        if (plain) return "public address " + ip + ":" + p;
    }
    return "port forwarded";
}
#endif
