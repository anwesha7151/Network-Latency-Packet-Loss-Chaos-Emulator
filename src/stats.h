#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace chaos {

struct Summary { double min = 0, avg = 0, p50 = 0, p95 = 0, p99 = 0, max = 0; size_t n = 0; };

inline Summary summarize(std::vector<double> v) {
    Summary s;
    s.n = v.size();
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto pct = [&](double p) {
        size_t i = (size_t)std::ceil(p / 100.0 * v.size());
        return v[(i == 0 ? 1 : i) - 1];
    };
    s.min = v.front(); s.max = v.back(); s.avg = sum / v.size();
    s.p50 = pct(50); s.p95 = pct(95); s.p99 = pct(99);
    return s;
}

inline double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace chaos
