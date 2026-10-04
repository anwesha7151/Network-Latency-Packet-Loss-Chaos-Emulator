#include "impairment.h"
#include <algorithm>

namespace chaos {

Impairment::Impairment(const Profile& p, uint64_t seed) : p_(p), rng_(seed) { tokens_ = p.burst_bytes; }

void Impairment::setProfile(const Profile& p) { p_ = p; tokens_ = p.burst_bytes; bad_ = false; }

uint64_t Impairment::randBelow(uint64_t n) { return n ? rng_() % n : 0; }

bool Impairment::coin(double pct) {
    if (pct <= 0) return false;
    if (pct >= 100) return true;
    return std::uniform_real_distribution<double>(0.0, 100.0)(rng_) < pct;
}

// Two loss models can be combined: bursty Gilbert-Elliott chain, then independent loss.
bool Impairment::lossDecision() {
    bool lost = false;
    if (p_.ge_p > 0) {
        if (bad_) { if (coin(p_.ge_r)) bad_ = false; }
        else if (coin(p_.ge_p)) bad_ = true;
        lost = coin(bad_ ? p_.ge_bad : p_.ge_good);
    }
    if (!lost && p_.loss_pct > 0) lost = coin(p_.loss_pct);
    return lost;
}

double Impairment::sampleDelay() {
    double d = p_.delay_ms;
    if (p_.jitter_ms > 0) {
        if (p_.dist == Dist::Normal) d = std::normal_distribution<double>(p_.delay_ms, p_.jitter_ms)(rng_);
        else d = p_.delay_ms + std::uniform_real_distribution<double>(-p_.jitter_ms, p_.jitter_ms)(rng_);
    }
    return std::max(0.0, d);
}

std::vector<Out> Impairment::process(double t, size_t size) {
    c_.in++;
    if (lossDecision()) { c_.dropped++; return {}; }

    // Token-bucket shaper using "debt": tokens may go negative; the debt tells us how
    // long the packet must wait. If the wait exceeds the queue limit it is tail-dropped.
    double wait = 0;
    if (p_.rate_kbps > 0) {
        double rate = p_.rate_kbps / 8.0;  // bytes per millisecond
        tokens_ = std::min(p_.burst_bytes, tokens_ + (t - last_t_) * rate);
        last_t_ = t;
        tokens_ -= (double)size;
        if (tokens_ < 0) {
            wait = -tokens_ / rate;
            if (wait > p_.queue_ms) {
                tokens_ += (double)size;
                c_.shaper_dropped++;
                c_.dropped++;
                return {};
            }
        }
    }

    // Reordering (like netem): a fraction of packets skip the delay, overtaking delayed ones.
    bool skip = p_.reorder_pct > 0 && p_.delay_ms > 0 && coin(p_.reorder_pct);
    if (skip) c_.reordered++;
    std::vector<Out> outs;
    outs.push_back({t + wait + (skip ? 0.0 : sampleDelay()), coin(p_.corrupt_pct), false});
    if (coin(p_.dup_pct)) {
        outs.push_back({t + wait + sampleDelay(), coin(p_.corrupt_pct), true});
        c_.dup++;
    }
    for (auto& o : outs) if (o.corrupt) c_.corrupt++;
    c_.out += outs.size();
    return outs;
}

}  // namespace chaos
