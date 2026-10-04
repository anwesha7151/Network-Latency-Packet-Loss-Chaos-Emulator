#include "netem.h"
#include "probe.h"
#include "proxy.h"
#include "sim.h"
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace chaos;

static const char* kUsage = R"(chaos - network latency / packet-loss chaos emulator

USAGE
  chaos profiles                              list built-in profiles
  chaos sim    --profile lte [--packets N] [--pps R] [--size B] [--seed S] [--json]
  chaos proxy  --listen PORT --target HOST:PORT [--profile P] [--set k=v]...
               [--down-profile P] [--down-set k=v]... [--scenario FILE]
               [--duration SEC] [--stats SEC] [--idle SEC] [--bind IP] [--seed S]
  chaos echo   --port PORT [--bind IP]
  chaos probe  --target HOST:PORT [--count N] [--interval MS] [--size B] [--timeline] [--json]
  chaos netem apply    --dev IF --profile P [--set k=v]... [--ttl SEC] [--persist] [--dry-run] [--force]
  chaos netem scenario --dev IF --file FILE [--dry-run] [--force]
  chaos netem show|reset --dev IF [--dry-run]

PARAMETERS (--set key=value)
  delay jitter(ms) dist(uniform|normal)  loss dup corrupt reorder(%)  rate(kbit/s)
  ge_p ge_r ge_bad ge_good (%, Gilbert-Elliott burst loss)  burst(bytes) queue(ms)
)";

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::vector<std::string>> opt;
    std::set<std::string> flags;
    bool has(const std::string& k) const { return opt.count(k) > 0; }
    std::string get(const std::string& k, const std::string& d = "") const { auto i = opt.find(k); return i == opt.end() ? d : i->second.back(); }
    static std::string fmt(double v) { char b[32]; snprintf(b, sizeof b, "%.15g", v); return b; }
    // Validated numeric option: must be a finite number inside [lo, hi].
    double real(const std::string& k, double d, double lo, double hi) const {
        if (!has(k)) return d;
        const std::string s = get(k);   // keep the string alive while strtod's end pointer is used
        char* e = nullptr;
        double v = strtod(s.c_str(), &e);
        if (s.empty() || *e || !std::isfinite(v)) throw std::runtime_error("--" + k + " expects a number, got '" + s + "'");
        if (v < lo || v > hi) throw std::runtime_error("--" + k + " must be between " + fmt(lo) + " and " + fmt(hi) + ", got '" + s + "'");
        return v;
    }
    // Validated whole-number option (no silent negative->huge unsigned conversion).
    uint64_t integer(const std::string& k, uint64_t d, double lo, double hi) const {
        if (!has(k)) return d;
        double v = real(k, 0, lo, hi);
        if (v != std::floor(v)) throw std::runtime_error("--" + k + " must be a whole number, got '" + get(k) + "'");
        return (uint64_t)v;
    }
};

static Args parseArgs(int argc, char** argv, int from) {
    static const std::set<std::string> kFlags{"json", "dry-run", "persist", "force", "timeline", "help"};
    Args a;
    for (int i = from; i < argc; i++) {
        std::string s = argv[i];
        if (s.rfind("--", 0) != 0) { a.pos.push_back(s); continue; }
        std::string name = s.substr(2), val;
        auto eq = name.find('=');
        bool inline_val = eq != std::string::npos;
        if (inline_val) { val = name.substr(eq + 1); name = name.substr(0, eq); }
        if (kFlags.count(name)) {
            if (inline_val) throw std::runtime_error("--" + name + " is a flag and does not take a value");
            a.flags.insert(name);
            continue;
        }
        if (!inline_val) {
            if (i + 1 >= argc) throw std::runtime_error("--" + name + " needs a value");
            val = argv[++i];
        }
        a.opt[name].push_back(val);
    }
    return a;
}

static Profile buildProfile(const Args& a, const std::string& prefix, Profile base = Profile()) {
    Profile p = base;
    if (a.has(prefix + "profile") && !presetByName(a.get(prefix + "profile"), p))
        throw std::runtime_error("unknown profile '" + a.get(prefix + "profile") + "' (see: chaos profiles)");
    auto it = a.opt.find(prefix + "set");
    if (it != a.opt.end()) {
        for (auto& kv : it->second) {
            auto eq = kv.find('=');
            std::string err;
            if (eq == std::string::npos || !applyKV(p, kv.substr(0, eq), kv.substr(eq + 1), err))
                throw std::runtime_error(eq == std::string::npos ? "--set expects key=value" : err);
        }
    }
    std::string err;
    if (!validate(p, err)) throw std::runtime_error(err);
    return p;
}

static void splitHostPort(const std::string& s, std::string& host, uint16_t& port) {
    auto c = s.rfind(':');
    if (c == std::string::npos) throw std::runtime_error("expected HOST:PORT, got '" + s + "'");
    host = s.substr(0, c);
    const std::string ps = s.substr(c + 1);
    char* e = nullptr;
    long v = strtol(ps.c_str(), &e, 10);
    if (host.empty() || ps.empty() || *e || v < 1 || v > 65535) throw std::runtime_error("bad HOST:PORT '" + s + "'");
    port = (uint16_t)v;
}

static uint16_t portOf(const Args& a, const std::string& k) {
    if (!a.has(k)) throw std::runtime_error("--" + k + " is required");
    return (uint16_t)a.integer(k, 0, 1, 65535);
}

static std::vector<Step> loadScenario(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open scenario file " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<Step> steps;
    std::string err;
    if (!parseScenario(ss.str(), steps, err)) throw std::runtime_error(path + ": " + err);
    return steps;
}


// Every command's accepted options. Anything else is an error: silently ignoring a typo like
// "--profle 3g" would run a chaos test with NO impairment while the user believes it is active.
static const std::map<std::string, std::set<std::string>> kOptions = {
    {"profiles", {}},
    {"sim", {"profile", "set", "packets", "pps", "size", "seed"}},
    {"proxy", {"listen", "target", "profile", "set", "down-profile", "down-set", "scenario", "duration", "stats", "idle", "bind", "seed"}},
    {"echo", {"port", "bind"}},
    {"probe", {"target", "count", "interval", "size", "wait"}},
    {"netem", {"dev", "profile", "set", "ttl", "file"}},
};
static const std::map<std::string, std::set<std::string>> kCmdFlags = {
    {"profiles", {}}, {"sim", {"json"}}, {"proxy", {}}, {"echo", {}},
    {"probe", {"json", "timeline"}}, {"netem", {"dry-run", "persist", "force"}},
};

static size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); j++)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

static void checkArgs(const std::string& cmd, const Args& a) {
    const auto& opts = kOptions.at(cmd);
    const auto& flags = kCmdFlags.at(cmd);
    auto unknown = [&](const std::string& name) {
        std::string best;
        size_t bd = 3;
        for (const auto* set : {&opts, &flags})
            for (auto& cand : *set) { size_t d = editDistance(name, cand); if (d < bd) { bd = d; best = cand; } }
        throw std::runtime_error("unknown option '--" + name + "' for '" + cmd + "'" +
                                 (best.empty() ? "" : " (did you mean '--" + best + "'?)") + "  [see: chaos help]");
    };
    for (auto& kv : a.opt) if (!opts.count(kv.first)) unknown(kv.first);
    for (auto& f : a.flags) if (f != "help" && !flags.count(f)) unknown(f);
    size_t allowed_pos = cmd == "netem" ? 1 : 0;
    if (a.pos.size() > allowed_pos) throw std::runtime_error("unexpected argument '" + a.pos[allowed_pos] + "' for '" + cmd + "'");
}

static int run(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h" || std::string(argv[1]) == "help") { std::cout << kUsage; return argc < 2 ? 2 : 0; }
    std::string cmd = argv[1];
    if (!kOptions.count(cmd)) throw std::runtime_error("unknown command '" + cmd + "' (try: chaos help)");
    Args a = parseArgs(argc, argv, 2);
    if (a.flags.count("help")) { std::cout << kUsage; return 0; }
    checkArgs(cmd, a);

    if (cmd == "profiles") {
        for (auto& n : presetNames()) { Profile p; presetByName(n, p); std::cout << "  " << n << std::string(12 - std::min<size_t>(n.size(), 11), ' ') << describe(p) << "\n"; }
        return 0;
    }
    if (cmd == "sim") {
        Profile p = buildProfile(a, "");
        SimResult r = simulate(p, a.integer("packets", 100000, 1, 20000000), a.real("pps", 1000, 0.001, 1e9),
                               (size_t)a.integer("size", 1000, 1, 65535), a.integer("seed", 1, 0, 9007199254740991.0));
        std::cout << (a.flags.count("json") ? simJson(r) : simText(p, r));
        return 0;
    }
    if (cmd == "proxy") {
        ProxyOptions o;
        o.bind = a.get("bind", "127.0.0.1");
        o.listen_port = portOf(a, "listen");
        if (!a.has("target")) throw std::runtime_error("--target HOST:PORT is required");
        splitHostPort(a.get("target"), o.target_host, o.target_port);
        o.up = buildProfile(a, "");
        o.down = (a.has("down-profile") || a.has("down-set")) ? buildProfile(a, "down-", o.up) : o.up;
        if (a.has("scenario")) o.scenario = loadScenario(a.get("scenario"));
        o.seed = a.integer("seed", 1, 0, 9007199254740991.0);
        o.duration_s = a.real("duration", 0, 0, 1e9);
        o.stats_every_s = a.real("stats", 0, 0, 1e9);
        o.idle_s = a.real("idle", 60, 0, 1e9);
        return runProxy(o);
    }
    if (cmd == "echo") return runEcho(a.get("bind", "127.0.0.1"), portOf(a, "port"));
    if (cmd == "probe") {
        ProbeOptions o;
        if (!a.has("target")) throw std::runtime_error("--target HOST:PORT is required");
        splitHostPort(a.get("target"), o.host, o.port);
        o.count = (int)a.integer("count", 100, 1, 1000000);
        o.interval_ms = a.real("interval", 20, 0.001, 3.6e6);
        o.size = (size_t)a.integer("size", 64, 16, 65507);   // first 16 bytes carry the probe header; 65507 = max UDP payload
        o.wait_ms = a.real("wait", 1000, 0, 3.6e6);
        ProbeResult r;
        std::string err;
        if (!runProbe(o, r, err)) throw std::runtime_error(err);
        std::cout << (a.flags.count("json") ? probeJson(r) : probeText(o, r, a.flags.count("timeline") > 0));
        return 0;
    }
    if (cmd == "netem") {
        if (a.pos.empty()) throw std::runtime_error("netem needs a subcommand: apply|scenario|show|reset");
        NetemOptions o;
        o.dev = a.get("dev");
        o.dry_run = a.flags.count("dry-run") > 0;
        o.force = a.flags.count("force") > 0;
        if (o.dev.empty()) throw std::runtime_error("--dev IF is required");
        const std::string& sub = a.pos[0];
        if (sub == "apply") return netemApplyHold(o, buildProfile(a, ""), a.real("ttl", 60, 0, 31536000), a.flags.count("persist") > 0);
        if (sub == "scenario") {
            if (!a.has("file")) throw std::runtime_error("netem scenario needs --file FILE");
            return netemRunScenario(o, loadScenario(a.get("file")));
        }
        if (sub == "show") return netemShow(o) ? 0 : 1;
        if (sub == "reset") return netemReset(o) ? 0 : 1;
        throw std::runtime_error("unknown netem subcommand '" + sub + "'");
    }
    throw std::runtime_error("unknown command '" + cmd + "' (try: chaos help)");
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
