#pragma once
#include "stats.h"
#include <cstdint>
#include <string>
#include <vector>

namespace chaos {

struct ProbeOptions {
    std::string host;
    uint16_t port = 0;
    int count = 100;
    double interval_ms = 20;
    size_t size = 64;
    double wait_ms = 1000;   // how long to wait for stragglers after the last send
};
struct Bucket { int sent = 0, recv = 0; double rtt_sum = 0; };
struct ProbeResult {
    int sent = 0, received = 0, lost = 0, dups = 0, reordered = 0, corrupt = 0;
    double loss_pct = 0, jitter_ms = 0;
    Summary rtt;
    std::vector<Bucket> timeline;   // one bucket per second of send time
};

// UDP echo server and measuring client: together they verify what an impairment really did.
int runEcho(const std::string& bind, uint16_t port);
bool runProbe(const ProbeOptions& o, ProbeResult& r, std::string& err);
std::string probeText(const ProbeOptions& o, const ProbeResult& r, bool timeline);
std::string probeJson(const ProbeResult& r);

}  // namespace chaos
