#include "profile.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace chaos {

bool Profile::any() const {
    return delay_ms > 0 || jitter_ms > 0 || loss_pct > 0 || ge_p > 0 || dup_pct > 0 ||
           corrupt_pct > 0 || reorder_pct > 0 || rate_kbps > 0;
}

static bool parseNum(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (*end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool applyKV(Profile& p, const std::string& k, const std::string& v, std::string& err) {
    if (k == "dist") {
        if (v == "uniform") p.dist = Dist::Uniform;
        else if (v == "normal") p.dist = Dist::Normal;
        else { err = "dist must be uniform|normal"; return false; }
        return true;
    }
    double x;
    if (!parseNum(v, x)) { err = "value for '" + k + "' is not a number: '" + v + "'"; return false; }
    struct F { const char* key; double* dst; double lo, hi; };
    F fields[] = {
        {"delay", &p.delay_ms, 0, 60000}, {"jitter", &p.jitter_ms, 0, 60000},
        {"loss", &p.loss_pct, 0, 100},    {"ge_p", &p.ge_p, 0, 100},
        {"ge_r", &p.ge_r, 0, 100},        {"ge_bad", &p.ge_bad, 0, 100},
        {"ge_good", &p.ge_good, 0, 100},  {"dup", &p.dup_pct, 0, 100},
        {"corrupt", &p.corrupt_pct, 0, 100}, {"reorder", &p.reorder_pct, 0, 100},
        {"rate", &p.rate_kbps, 0, 1e7},   {"burst", &p.burst_bytes, 1, 1e9},
        {"queue", &p.queue_ms, 0, 600000}};
    for (auto& f : fields) {
        if (k != f.key) continue;
        if (x < f.lo || x > f.hi) {
            std::ostringstream o;
            o << "'" << k << "' must be between " << f.lo << " and " << f.hi;
            err = o.str();
            return false;
        }
        *f.dst = x;
        return true;
    }
    err = "unknown parameter '" + k + "'";
    return false;
}

bool validate(const Profile& p, std::string& err) {
    if (p.jitter_ms > 0 && p.delay_ms <= 0) { err = "jitter requires delay > 0"; return false; }
    if (p.ge_p > 0 && p.ge_r <= 0) { err = "ge_p > 0 requires ge_r > 0 (otherwise the link never recovers)"; return false; }
    return true;
}

static const struct { const char* name; const char* spec; } kPresets[] = {
    {"none", ""},
    {"lte", "delay=45 jitter=15 dist=normal loss=0.2"},
    {"3g", "delay=150 jitter=40 dist=normal loss=1 rate=1500"},
    {"edge-2g", "delay=400 jitter=100 dist=normal loss=3 rate=150 queue=800"},
    {"satellite", "delay=600 jitter=30 dist=normal loss=1 rate=5000"},
    {"lossy-wifi", "delay=8 jitter=6 dist=normal ge_p=3 ge_r=30 ge_bad=60 reorder=1"},
    {"congested", "delay=80 jitter=60 dist=normal loss=3 rate=2000 queue=300 dup=0.1"},
    {"blackhole", "loss=100"},
};

bool presetByName(const std::string& name, Profile& out) {
    for (auto& pr : kPresets) {
        if (name != pr.name) continue;
        Profile p;
        std::istringstream in(pr.spec);
        std::string tok, err;
        while (in >> tok) {
            auto eq = tok.find('=');
            applyKV(p, tok.substr(0, eq), tok.substr(eq + 1), err);
        }
        out = p;
        return true;
    }
    return false;
}

std::vector<std::string> presetNames() {
    std::vector<std::string> v;
    for (auto& pr : kPresets) v.push_back(pr.name);
    return v;
}

std::string describe(const Profile& p) {
    std::ostringstream o;
    if (!p.any()) return "pass-through";
    if (p.delay_ms > 0) o << "delay=" << p.delay_ms << "ms";
    if (p.jitter_ms > 0) o << " jitter=" << p.jitter_ms << "ms(" << (p.dist == Dist::Normal ? "normal" : "uniform") << ")";
    if (p.loss_pct > 0) o << " loss=" << p.loss_pct << "%";
    if (p.ge_p > 0) o << " burst-loss(p=" << p.ge_p << "% r=" << p.ge_r << "% bad=" << p.ge_bad << "% good=" << p.ge_good << "%)";
    if (p.dup_pct > 0) o << " dup=" << p.dup_pct << "%";
    if (p.corrupt_pct > 0) o << " corrupt=" << p.corrupt_pct << "%";
    if (p.reorder_pct > 0) o << " reorder=" << p.reorder_pct << "%";
    if (p.rate_kbps > 0) o << " rate=" << p.rate_kbps << "kbit/s";
    std::string s = o.str();
    return s.empty() || s[0] != ' ' ? s : s.substr(1);
}

bool parseScenario(const std::string& text, std::vector<Step>& out, std::string& err) {
    std::istringstream in(text);
    std::string line;
    int ln = 0;
    Profile cur;
    double last = -1;
    out.clear();
    while (std::getline(in, line)) {
        ln++;
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::istringstream ls(line);
        std::string kw;
        if (!(ls >> kw)) continue;
        auto fail = [&](const std::string& m) { err = "line " + std::to_string(ln) + ": " + m; return false; };
        if (kw != "at") return fail("expected 'at <seconds> ...'");
        std::string ts;
        double t;
        if (!(ls >> ts) || !parseNum(ts, t) || t < 0) return fail("bad time");
        if (t < last) return fail("times must be non-decreasing");
        std::string tok;
        while (ls >> tok) {
            auto eq = tok.find('=');
            if (eq == std::string::npos) {
                if (!presetByName(tok, cur)) return fail("unknown preset '" + tok + "'");
            } else {
                std::string e;
                if (!applyKV(cur, tok.substr(0, eq), tok.substr(eq + 1), e)) return fail(e);
            }
        }
        std::string e;
        if (!validate(cur, e)) return fail(e);
        out.push_back({t, cur});
        last = t;
    }
    if (out.empty()) { err = "scenario has no steps"; return false; }
    return true;
}

}  // namespace chaos
