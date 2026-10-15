/* =============================================================================
 * ccmap_web.h - the phone view of CC Map: a tiny HTTP server on the Force's own network.
 *
 *   GET /                     the page (ccmap_page.h): Launch Control drawing + other controllers
 *   GET /state                JSON of the current map, polled by the page about 4 times a second
 *   GET /label?slot=N&name=X  give slot N (1-based) the name X ("" clears it)
 *
 * One thread, one request at a time, poll() with a short timeout so the plugin can close it. No
 * files are served and nothing outside the map can be changed; port 8000 (CCMAP_WEB_PORT
 * overrides, offline test). Plain POSIX sockets, no dependencies.
 * ========================================================================== */
#ifndef CCMAP_WEB_H
#define CCMAP_WEB_H
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace ccweb {

struct Server {
    std::function<std::string()> state;                         /* JSON, caller locks what it needs */
    std::function<void(int, const std::string &)> set_label;    /* slot index 0-based */
    const char *page = "";
    int port = 8000;
    int fd = -1;
    std::atomic<bool> running{false};
    std::atomic<long> requests{0};
    std::thread thr;
    std::mutex addr_mu;
    std::string addr_s = "-";                                   /* "192.168.1.20:8000" */
    std::string address() { std::lock_guard<std::mutex> lk(addr_mu); return addr_s; }

    bool start(int p) {
        port = p;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((uint16_t)port);
        if (bind(fd, (sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) { close(fd); fd = -1; return false; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        refresh_addr();
        running = true;
        thr = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        if (running.exchange(false) && thr.joinable()) thr.join();
        if (fd >= 0) close(fd);
        fd = -1;
    }
    /* the address to type into the phone: the first IPv4 that isn't loopback (wlan0 on the Force) */
    void refresh_addr() {
        ifaddrs *ifs = nullptr;
        char ip[INET_ADDRSTRLEN] = "";
        if (getifaddrs(&ifs) == 0) {
            for (ifaddrs *i = ifs; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
                inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
                if (!std::strncmp(i->ifa_name, "wl", 2)) break;   /* prefer the WLAN */
            }
            freeifaddrs(ifs);
        }
        char b[64];
        if (ip[0]) std::snprintf(b, sizeof b, "%s:%d", ip, port);
        else std::snprintf(b, sizeof b, "NO NETWORK (:%d)", port);
        std::lock_guard<std::mutex> lk(addr_mu);
        addr_s = b;
    }

    static std::string url_arg(const std::string &q, const char *key) {
        const std::string k = std::string(key) + "=";
        size_t p = 0;
        while ((p = q.find(k, p)) != std::string::npos) {
            if (p == 0 || q[p - 1] == '&' || q[p - 1] == '?') {
                size_t e = q.find('&', p);
                std::string v = q.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
                std::string o;
                for (size_t i = 0; i < v.size(); i++) {
                    if (v[i] == '%' && i + 2 < v.size()) { o += (char)std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
                    else if (v[i] == '+') o += ' ';
                    else o += v[i];
                }
                return o;
            }
            p += k.size();
        }
        return "";
    }
    static void send_all(int c, const std::string &s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = send(c, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return;
            off += (size_t)n;
        }
    }
    static void reply(int c, const char *status, const char *type, const std::string &body) {
        char h[256];
        std::snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                      "Connection: close\r\n\r\n", status, type, body.size());
        send_all(c, h);
        send_all(c, body);
    }
    void handle(int c) {
        char buf[2048];
        int got = 0;
        pollfd pf{c, POLLIN, 0};
        while (got < (int)sizeof buf - 1 && poll(&pf, 1, 300) > 0) {
            ssize_t n = recv(c, buf + got, sizeof buf - 1 - got, 0);
            if (n <= 0) break;
            got += (int)n;
            buf[got] = 0;
            if (std::strstr(buf, "\r\n\r\n")) break;
        }
        buf[got] = 0;
        if (std::strncmp(buf, "GET ", 4)) { reply(c, "405 Method Not Allowed", "text/plain", "GET only\n"); return; }
        const char *sp = std::strchr(buf + 4, ' ');
        std::string path(buf + 4, sp ? (size_t)(sp - buf - 4) : std::strlen(buf + 4));
        const size_t qm = path.find('?');
        const std::string route = path.substr(0, qm), query = qm == std::string::npos ? "" : path.substr(qm + 1);
        requests++;
        if (route == "/" || route == "/index.html") reply(c, "200 OK", "text/html; charset=utf-8", page);
        else if (route == "/state") reply(c, "200 OK", "application/json", state ? state() : "{}");
        else if (route == "/label") {
            const int slot = std::atoi(url_arg(query, "slot").c_str());
            if (slot < 1 || slot > 128 || !set_label) { reply(c, "400 Bad Request", "text/plain", "slot 1..128\n"); return; }
            set_label(slot - 1, url_arg(query, "name"));
            reply(c, "200 OK", "application/json", "{\"ok\":true}");
        } else reply(c, "404 Not Found", "text/plain", "not found\n");
    }
    void loop() {
        int tick = 0;
        while (running) {
            pollfd pf{fd, POLLIN, 0};
            if (poll(&pf, 1, 200) > 0) {
                int c = accept(fd, nullptr, nullptr);
                if (c >= 0) { handle(c); close(c); }
            }
            if (++tick % 50 == 0) refresh_addr();   /* about every 10 s: the WLAN may come up later */
        }
    }
};

/* JSON string escaping */
static inline std::string js(const char *s) {
    std::string o = "\"";
    char b[8];
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 32) { std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

}   // namespace ccweb
#endif
