#include "proxy.h"
#include "impairment.h"
#include "stats.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <queue>
#include <unordered_map>

namespace chaos {
namespace {

volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Event {
    double due;
    uint64_t seq;
    int fd;
    int owner;          // upstream fd of the client this packet belongs to
    bool to_client;
    sockaddr_in dest;
    std::vector<uint8_t> data;
};
struct Later {
    bool operator()(const Event& a, const Event& b) const { return a.due != b.due ? a.due > b.due : a.seq > b.seq; }
};
struct Client { sockaddr_in addr; int up_fd; double last_seen; uint64_t pending; };

std::string keyOf(const sockaddr_in& a) {
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, b, sizeof b);
    return std::string(b) + ":" + std::to_string(ntohs(a.sin_port));
}

void printCounters(const char* name, const Counters& c) {
    std::cerr << "  " << name << ": in=" << c.in << " out=" << c.out << " dropped=" << c.dropped
              << " (shaper=" << c.shaper_dropped << ") dup=" << c.dup << " corrupt=" << c.corrupt
              << " reordered=" << c.reordered << "\n";
}

}  // namespace

int runProxy(const ProxyOptions& o) {
    struct sigaction sa {};
    sa.sa_handler = onSignal;                  // no SA_RESTART: poll() must wake on Ctrl+C
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int lfd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in la{};
    la.sin_family = AF_INET;
    la.sin_port = htons(o.listen_port);
    if (lfd < 0 || inet_pton(AF_INET, o.bind.c_str(), &la.sin_addr) != 1 ||
        bind(lfd, (sockaddr*)&la, sizeof la) != 0) {
        std::cerr << "error: cannot bind " << o.bind << ":" << o.listen_port << " (" << strerror(errno) << ")\n";
        return 1;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(o.target_host.c_str(), std::to_string(o.target_port).c_str(), &hints, &res) != 0 || !res) {
        std::cerr << "error: cannot resolve target " << o.target_host << "\n";
        return 1;
    }
    sockaddr_in target = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);

    Impairment up(o.up, o.seed), down(o.down, o.seed + 1);
    std::priority_queue<Event, std::vector<Event>, Later> q;
    std::unordered_map<std::string, Client> clients;
    std::unordered_map<int, std::string> fd_owner;
    std::vector<uint8_t> buf(65536);
    uint64_t seq = 0, overflow = 0, expired = 0, evicted = 0, refused = 0, sock_errors = 0;
    size_t step = 0;
    const double t0 = nowMs();
    double next_stats = o.stats_every_s > 0 ? o.stats_every_s * 1000 : -1;
    double next_sweep = 1000;

    auto clientOf = [&](int up_fd) -> Client* {
        auto ow = fd_owner.find(up_fd);
        if (ow == fd_owner.end()) return nullptr;
        auto cl = clients.find(ow->second);
        return cl == clients.end() ? nullptr : &cl->second;
    };
    auto openUpstream = [&]() -> int {
        int ufd = socket(AF_INET, SOCK_DGRAM, 0);
        if (ufd < 0) return -1;
        if (connect(ufd, (sockaddr*)&target, sizeof target) != 0) { int e = errno; close(ufd); errno = e; return -1; }
        return ufd;
    };
    // Free the least-recently-used client that has nothing queued (used when out of file descriptors).
    auto evictOldest = [&]() -> bool {
        auto victim = clients.end();
        for (auto it = clients.begin(); it != clients.end(); ++it)
            if (it->second.pending == 0 && (victim == clients.end() || it->second.last_seen < victim->second.last_seen)) victim = it;
        if (victim == clients.end()) return false;
        fd_owner.erase(victim->second.up_fd);
        close(victim->second.up_fd);
        clients.erase(victim);
        return true;
    };

    std::cerr << "proxy " << o.bind << ":" << o.listen_port << " -> " << o.target_host << ":" << o.target_port << "\n"
              << "  up  : " << describe(o.up) << "\n  down: " << describe(o.down) << "\n";

    auto schedule = [&](Impairment& imp, double now, int fd, int owner, bool to_client, const sockaddr_in& dest, const uint8_t* d, size_t len) {
        for (const Out& out : imp.process(now, len)) {
            if (q.size() >= 200000) { overflow++; continue; }  // bound memory under floods
            Event e{out.t_ms, seq++, fd, owner, to_client, dest, std::vector<uint8_t>(d, d + len)};
            if (out.corrupt && len > 0) e.data[imp.randBelow(len)] ^= (uint8_t)(1u << imp.randBelow(8));
            q.push(std::move(e));
            if (Client* cl = clientOf(owner)) cl->pending++;
        }
    };

    while (!g_stop) {
        double now = nowMs() - t0;
        if (o.duration_s > 0 && now >= o.duration_s * 1000) break;
        while (step < o.scenario.size() && now >= o.scenario[step].at_s * 1000) {
            up.setProfile(o.scenario[step].profile);
            down.setProfile(o.scenario[step].profile);
            std::cerr << "[t=" << (int)(now / 1000) << "s] scenario -> " << describe(o.scenario[step].profile) << "\n";
            step++;
        }
        if (next_stats >= 0 && now >= next_stats) {
            std::cerr << "[t=" << (int)(now / 1000) << "s] stats\n";
            printCounters("up  ", up.counters());
            printCounters("down", down.counters());
            next_stats += o.stats_every_s * 1000;
        }

        if (o.idle_s > 0 && now >= next_sweep) {   // release upstream sockets of idle clients
            next_sweep = now + 1000;
            for (auto it = clients.begin(); it != clients.end();) {
                if (it->second.pending == 0 && now - it->second.last_seen > o.idle_s * 1000) {
                    fd_owner.erase(it->second.up_fd);
                    close(it->second.up_fd);
                    it = clients.erase(it);
                    expired++;
                } else ++it;
            }
        }

        std::vector<pollfd> pfds{{lfd, POLLIN, 0}};
        for (auto& kv : clients) pfds.push_back({kv.second.up_fd, POLLIN, 0});
        int timeout = 100;
        if (!q.empty()) {
            double w = q.top().due - now;
            timeout = w <= 0 ? 0 : (int)std::min(100.0, std::ceil(w));
        }
        int n = poll(pfds.data(), pfds.size(), timeout);
        now = nowMs() - t0;
        if (n > 0) {
            for (auto& p : pfds) {
                if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    // e.g. ICMP "port unreachable" because the target is down. The error stays pending (and poll()
                    // keeps returning immediately = 100% CPU) until it is read, so consume it.
                    int soerr = 0;
                    socklen_t esl = sizeof soerr;
                    getsockopt(p.fd, SOL_SOCKET, SO_ERROR, &soerr, &esl);
                    if (p.fd != lfd) sock_errors++;
                }
                if (!(p.revents & POLLIN)) continue;
                for (int i = 0; i < 64; i++) {  // drain a bounded burst per wake-up
                    if (p.fd == lfd) {
                        sockaddr_in src{};
                        socklen_t sl = sizeof src;
                        ssize_t len = recvfrom(lfd, buf.data(), buf.size(), MSG_DONTWAIT, (sockaddr*)&src, &sl);
                        if (len < 0) break;
                        std::string key = keyOf(src);
                        auto it = clients.find(key);
                        if (it == clients.end()) {
                            int ufd = openUpstream();
                            if (ufd < 0 && (errno == EMFILE || errno == ENFILE) && evictOldest()) { evicted++; ufd = openUpstream(); }
                            if (ufd < 0) {
                                if (++refused == 1) std::cerr << "warning: cannot open upstream socket (" << strerror(errno) << "); dropping packets from new clients\n";
                                continue;
                            }
                            it = clients.emplace(key, Client{src, ufd, now, 0}).first;
                            fd_owner[ufd] = key;
                        }
                        it->second.last_seen = now;
                        schedule(up, now, it->second.up_fd, it->second.up_fd, false, target, buf.data(), (size_t)len);
                    } else {
                        Client* cl = clientOf(p.fd);
                        if (!cl) break;   // client was evicted during this wake-up
                        ssize_t len = recv(p.fd, buf.data(), buf.size(), MSG_DONTWAIT);
                        if (len < 0) break;
                        cl->last_seen = now;
                        schedule(down, now, lfd, p.fd, true, cl->addr, buf.data(), (size_t)len);
                    }
                }
            }
        }
        now = nowMs() - t0;
        while (!q.empty() && q.top().due <= now) {
            Event e = q.top();
            q.pop();
            ssize_t sent = e.to_client ? sendto(e.fd, e.data.data(), e.data.size(), 0, (sockaddr*)&e.dest, sizeof e.dest)
                                       : send(e.fd, e.data.data(), e.data.size(), 0);
            if (sent < 0) sock_errors++;
            if (Client* cl = clientOf(e.owner)) if (cl->pending) cl->pending--;
        }
    }
    std::cerr << "\nproxy stopped. totals:\n";
    printCounters("up  ", up.counters());
    printCounters("down", down.counters());
    if (overflow) std::cerr << "  queue overflow drops: " << overflow << "\n";
    if (sock_errors) std::cerr << "  socket errors (e.g. target unreachable): " << sock_errors << "\n";
    if (expired) std::cerr << "  idle clients released: " << expired << "\n";
    if (evicted) std::cerr << "  clients evicted (out of file descriptors): " << evicted << "\n";
    if (refused) std::cerr << "  packets from new clients refused (no socket available): " << refused << "\n";
    for (auto& kv : clients) close(kv.second.up_fd);
    close(lfd);
    return 0;
}

}  // namespace chaos
