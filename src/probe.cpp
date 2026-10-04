#include "probe.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>

namespace chaos {
namespace {
volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
const uint32_t kMagic = 0xC4A05E11;
uint8_t fillByte(uint32_t seq, size_t i) { return (uint8_t)((seq * 31 + i) & 0xff); }
}  // namespace

int runEcho(const std::string& bind_ip, uint16_t port) {
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (fd < 0 || inet_pton(AF_INET, bind_ip.c_str(), &a.sin_addr) != 1 || bind(fd, (sockaddr*)&a, sizeof a) != 0) {
        std::cerr << "error: cannot bind echo server: " << strerror(errno) << "\n";
        return 1;
    }
    std::cerr << "echo server on " << bind_ip << ":" << port << "\n";
    std::vector<uint8_t> buf(65536);
    while (!g_stop) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        sockaddr_in src{};
        socklen_t sl = sizeof src;
        ssize_t n = recvfrom(fd, buf.data(), buf.size(), 0, (sockaddr*)&src, &sl);
        if (n > 0) sendto(fd, buf.data(), (size_t)n, 0, (sockaddr*)&src, sl);
    }
    close(fd);
    return 0;
}

bool runProbe(const ProbeOptions& o, ProbeResult& r, std::string& err) {
    size_t size = std::max<size_t>(o.size, 16);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(o.host.c_str(), std::to_string(o.port).c_str(), &hints, &res) != 0 || !res) { err = "cannot resolve " + o.host; return false; }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0) { err = std::string("socket: ") + strerror(errno); freeaddrinfo(res); return false; }
    freeaddrinfo(res);

    int n = o.count;
    std::vector<double> sent_at(n, -1), rtts;
    std::vector<char> seen(n, 0);
    std::vector<uint8_t> pkt(size), in(65536);
    double start = nowMs(), last_send = start, prev_rtt = -1, jitter = 0;
    int sent = 0;
    long max_seen = -1;
    r = ProbeResult{};
    while (true) {
        double now = nowMs();
        if (sent < n && now >= start + sent * o.interval_ms) {
            uint32_t seq = (uint32_t)sent, magic = kMagic;
            uint64_t ts = (uint64_t)(now * 1000);
            memcpy(pkt.data(), &magic, 4);
            memcpy(pkt.data() + 4, &seq, 4);
            memcpy(pkt.data() + 8, &ts, 8);
            for (size_t i = 16; i < size; i++) pkt[i] = fillByte(seq, i);
            send(fd, pkt.data(), size, 0);
            sent_at[sent] = now;
            last_send = now;
            sent++;
        }
        double wait;
        if (sent < n) wait = start + sent * o.interval_ms - now;
        else {
            wait = last_send + o.wait_ms - now;
            if (wait <= 0 || r.received >= n) break;
        }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, wait <= 0 ? 0 : (int)std::min(20.0, std::ceil(wait))) > 0) {
            for (int i = 0; i < 256; i++) {
                ssize_t len = recv(fd, in.data(), in.size(), MSG_DONTWAIT);
                if (len < 16) break;
                double rx = nowMs();
                uint32_t magic, seq;
                memcpy(&magic, in.data(), 4);
                memcpy(&seq, in.data() + 4, 4);
                if (magic != kMagic || (int)seq >= n || sent_at[seq] < 0) continue;  // mangled header: unattributable
                if (seen[seq]) { r.dups++; continue; }
                seen[seq] = 1;
                r.received++;
                double rtt = rx - sent_at[seq];
                rtts.push_back(rtt);
                if ((long)seq < max_seen) r.reordered++;
                max_seen = std::max<long>(max_seen, seq);
                for (size_t k = 16; k < (size_t)len; k++) if (in[k] != fillByte(seq, k)) { r.corrupt++; break; }
                if (prev_rtt >= 0) jitter += (std::fabs(rtt - prev_rtt) - jitter) / 16.0;  // RFC 3550 estimator
                prev_rtt = rtt;
                size_t b = (size_t)((sent_at[seq] - start) / 1000.0);
                if (r.timeline.size() <= b) r.timeline.resize(b + 1);
                r.timeline[b].recv++;
                r.timeline[b].rtt_sum += rtt;
            }
        }
    }
    close(fd);
    for (int i = 0; i < sent; i++) {
        size_t b = (size_t)((sent_at[i] - start) / 1000.0);
        if (r.timeline.size() <= b) r.timeline.resize(b + 1);
        r.timeline[b].sent++;
    }
    r.sent = sent;
    r.lost = sent - r.received;
    r.loss_pct = sent ? 100.0 * r.lost / sent : 0;
    r.jitter_ms = jitter;
    r.rtt = summarize(rtts);
    return true;
}

std::string probeText(const ProbeOptions& o, const ProbeResult& r, bool timeline) {
    std::ostringstream s;
    s.setf(std::ios::fixed); s.precision(1);
    s << "Probe " << o.host << ":" << o.port << "  sent=" << r.sent << " recv=" << r.received << " loss=" << r.loss_pct
      << "%  dup=" << r.dups << " reordered=" << r.reordered << " corrupt=" << r.corrupt << "\n"
      << "RTT ms: min " << r.rtt.min << "  avg " << r.rtt.avg << "  p50 " << r.rtt.p50 << "  p95 " << r.rtt.p95
      << "  p99 " << r.rtt.p99 << "  max " << r.rtt.max << "   jitter(RFC3550) " << r.jitter_ms << "\n";
    if (timeline) {
        s << "Timeline (per second of send time):\n";
        for (size_t i = 0; i < r.timeline.size(); i++) {
            const Bucket& b = r.timeline[i];
            double loss = b.sent ? 100.0 * (b.sent - b.recv) / b.sent : 0;
            s << "  t=" << i << "s  sent=" << b.sent << "  loss=" << loss << "%  avg RTT=" << (b.recv ? b.rtt_sum / b.recv : 0) << " ms\n";
        }
    }
    return s.str();
}

std::string probeJson(const ProbeResult& r) {
    std::ostringstream s;
    s << "{\n  \"sent\": " << r.sent << ",\n  \"received\": " << r.received << ",\n  \"loss_pct\": " << r.loss_pct
      << ",\n  \"dups\": " << r.dups << ",\n  \"reordered\": " << r.reordered << ",\n  \"corrupt\": " << r.corrupt
      << ",\n  \"rtt_avg_ms\": " << r.rtt.avg << ",\n  \"rtt_p95_ms\": " << r.rtt.p95
      << ",\n  \"jitter_ms\": " << r.jitter_ms << "\n}\n";
    return s.str();
}

}  // namespace chaos
