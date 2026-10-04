#pragma once
#include "impairment.h"
#include "stats.h"
#include <string>

namespace chaos {

struct SimResult {
    uint64_t sent = 0, delivered = 0, dropped = 0, shaper_dropped = 0, duplicates = 0, corrupt = 0, reordered_arrivals = 0;
    double loss_pct = 0, mean_burst = 0, jitter_ms = 0, goodput_kbps = 0;
    size_t max_burst = 0;
    Summary delay;
};

// Offline statistical simulation: no sockets, no root. Used to verify a profile
// before applying it, and by the unit tests.
SimResult simulate(const Profile& p, uint64_t n, double pps, size_t size, uint64_t seed);
std::string simText(const Profile& p, const SimResult& r);
std::string simJson(const SimResult& r);

}  // namespace chaos
