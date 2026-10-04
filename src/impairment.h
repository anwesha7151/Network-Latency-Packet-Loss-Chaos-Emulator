#pragma once
// The impairment engine: a pure, deterministic (seeded) function from
// "packet arrives at time t" to "zero or more packets leave at times t1, t2...".
// It has no sockets and no clock, so the same code drives the offline simulator,
// the userspace proxy and the unit tests.
#include "profile.h"
#include <cstdint>
#include <random>
#include <vector>

namespace chaos {

struct Out { double t_ms; bool corrupt; bool dup; };

struct Counters {
    uint64_t in = 0, out = 0, dropped = 0, shaper_dropped = 0, dup = 0, corrupt = 0, reordered = 0;
};

class Impairment {
public:
    Impairment(const Profile& p, uint64_t seed);
    void setProfile(const Profile& p);
    const Profile& profile() const { return p_; }
    std::vector<Out> process(double t_ms, size_t size);  // empty result => packet dropped
    const Counters& counters() const { return c_; }
    uint64_t randBelow(uint64_t n);

private:
    bool coin(double pct);
    bool lossDecision();
    double sampleDelay();
    Profile p_;
    std::mt19937_64 rng_;
    bool bad_ = false;               // Gilbert-Elliott state
    double tokens_ = 0, last_t_ = 0; // token bucket
    Counters c_;
};

}  // namespace chaos
