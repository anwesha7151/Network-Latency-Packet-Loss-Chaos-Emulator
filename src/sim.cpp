#include "sim.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace chaos {

SimResult simulate(const Profile& p, uint64_t n, double pps, size_t size, uint64_t seed) {
    Impairment imp(p, seed);
    SimResult r;
    r.sent = n;
    struct Arr { double t; uint64_t id; };
    std::vector<Arr> arrivals;
    std::vector<double> delays, first_delay(n, -1.0);
    uint64_t run = 0, bursts = 0, burst_pkts = 0;
    double first_in = 0, last_out = 0;
    for (uint64_t i = 0; i < n; i++) {
        double t = (double)i * 1000.0 / pps;
        auto outs = imp.process(t, size);
        if (outs.empty()) {
            run++;
            continue;
        }
        if (run) { bursts++; burst_pkts += run; r.max_burst = std::max<size_t>(r.max_burst, run); run = 0; }
        for (auto& o : outs) {
            arrivals.push_back({o.t_ms, i});
            last_out = std::max(last_out, o.t_ms);
            if (!o.dup) { first_delay[i] = o.t_ms - t; delays.push_back(o.t_ms - t); }
        }
    }
    if (run) { bursts++; burst_pkts += run; r.max_burst = std::max<size_t>(r.max_burst, run); }
    const Counters& c = imp.counters();
    r.dropped = c.dropped; r.shaper_dropped = c.shaper_dropped; r.duplicates = c.dup; r.corrupt = c.corrupt;
    r.delivered = n - c.dropped;
    r.loss_pct = n ? 100.0 * (double)c.dropped / (double)n : 0;
    r.mean_burst = bursts ? (double)burst_pkts / (double)bursts : 0;
    r.delay = summarize(delays);
    // reordering = a packet arriving after a packet with a higher sequence number
    std::stable_sort(arrivals.begin(), arrivals.end(), [](const Arr& a, const Arr& b) { return a.t < b.t; });
    uint64_t maxid = 0; bool any = false;
    for (auto& a : arrivals) {
        if (any && a.id < maxid) r.reordered_arrivals++;
        if (!any || a.id > maxid) maxid = a.id;
        any = true;
    }
    double prev = -1, sum = 0; uint64_t k = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (first_delay[i] < 0) continue;
        if (prev >= 0) { sum += std::fabs(first_delay[i] - prev); k++; }
        prev = first_delay[i];
    }
    r.jitter_ms = k ? sum / (double)k : 0;
    if (last_out > first_in) r.goodput_kbps = (double)r.delivered * (double)size * 8.0 / (last_out - first_in);
    return r;
}

std::string simText(const Profile& p, const SimResult& r) {
    std::ostringstream o;
    o.setf(std::ios::fixed); o.precision(2);
    o << "Profile : " << describe(p) << "\n"
      << "Packets : sent=" << r.sent << " delivered=" << r.delivered << " dropped=" << r.dropped
      << " (shaper tail-drops=" << r.shaper_dropped << ") dup=" << r.duplicates << " corrupt=" << r.corrupt << "\n"
      << "Loss    : " << r.loss_pct << "%   mean burst=" << r.mean_burst << " pkts   longest burst=" << r.max_burst << "\n"
      << "Delay ms: min " << r.delay.min << "  avg " << r.delay.avg << "  p50 " << r.delay.p50
      << "  p95 " << r.delay.p95 << "  p99 " << r.delay.p99 << "  max " << r.delay.max << "\n"
      << "Jitter  : " << r.jitter_ms << " ms (mean |delta delay|)   reordered arrivals=" << r.reordered_arrivals << "\n"
      << "Goodput : " << r.goodput_kbps << " kbit/s\n";
    return o.str();
}

std::string simJson(const SimResult& r) {
    std::ostringstream o;
    o << "{\n"
      << "  \"sent\": " << r.sent << ",\n  \"delivered\": " << r.delivered << ",\n"
      << "  \"loss_pct\": " << r.loss_pct << ",\n  \"mean_burst\": " << r.mean_burst << ",\n"
      << "  \"max_burst\": " << r.max_burst << ",\n  \"delay_avg_ms\": " << r.delay.avg << ",\n"
      << "  \"delay_p95_ms\": " << r.delay.p95 << ",\n  \"jitter_ms\": " << r.jitter_ms << ",\n"
      << "  \"reordered\": " << r.reordered_arrivals << ",\n  \"duplicates\": " << r.duplicates << ",\n"
      << "  \"goodput_kbps\": " << r.goodput_kbps << "\n}\n";
    return o.str();
}

}  // namespace chaos
