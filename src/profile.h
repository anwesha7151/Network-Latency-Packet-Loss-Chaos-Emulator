#pragma once
// A Profile describes one network "weather condition". It is shared by every backend
// (offline simulator, userspace UDP proxy, kernel netem) so behaviour stays consistent.
#include <string>
#include <vector>

namespace chaos {

enum class Dist { Uniform, Normal };

struct Profile {
    double delay_ms = 0, jitter_ms = 0;
    Dist dist = Dist::Uniform;
    double loss_pct = 0;                 // independent (Bernoulli) loss
    // Gilbert-Elliott burst-loss model; active when ge_p > 0.
    // ge_p: P(good->bad) %, ge_r: P(bad->good) %, ge_bad/ge_good: loss % in each state.
    double ge_p = 0, ge_r = 0, ge_bad = 100, ge_good = 0;
    double dup_pct = 0, corrupt_pct = 0, reorder_pct = 0;
    double rate_kbps = 0;                // token-bucket shaper, 0 = unlimited
    double burst_bytes = 3000;           // token bucket depth (userspace backends)
    double queue_ms = 500;               // max shaper queueing before tail-drop (userspace)
    bool any() const;
};

bool applyKV(Profile& p, const std::string& key, const std::string& val, std::string& err);
bool validate(const Profile& p, std::string& err);
bool presetByName(const std::string& name, Profile& out);
std::vector<std::string> presetNames();
std::string describe(const Profile& p);

struct Step { double at_s; Profile profile; };
// Scenario file: lines like "at 30 loss=15 delay=300" or "at 0 lte". Words without '=' are presets.
bool parseScenario(const std::string& text, std::vector<Step>& out, std::string& err);

}  // namespace chaos
