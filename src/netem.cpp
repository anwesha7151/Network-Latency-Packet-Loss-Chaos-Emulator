#include "netem.h"
#include "stats.h"
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

namespace chaos {
namespace {
volatile sig_atomic_t g_sig = 0;
void onSignal(int) { g_sig = 1; }
void installHandlers() {
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);   // terminal closed
}
std::string num(double v) { char b[32]; snprintf(b, sizeof b, "%g", v); return b; }

// Run argv without a shell; stream its output to ours. Returns exit status (or -1).
int runArgv(const std::vector<std::string>& argv, bool dry) {
    std::string line;
    for (auto& a : argv) line += (line.empty() ? "" : " ") + a;
    std::cout << (dry ? "[dry-run] " : "[exec] ") << line << "\n" << std::flush;
    if (dry) return 0;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> v;
        for (auto& a : argv) v.push_back(const_cast<char*>(a.c_str()));
        v.push_back(nullptr);
        execvp(v[0], v.data());
        int err = errno;
        fprintf(stderr, "error: cannot run '%s': %s%s\n", v[0], strerror(err),
                err == ENOENT ? " (is iproute2 installed?  sudo apt install iproute2)" : "");
        _exit(127);
    }
    int st = 0;
    pid_t w;
    do { w = waitpid(pid, &st, 0); } while (w < 0 && errno == EINTR);   // Ctrl+C must not abandon a running tc
    if (w < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

bool ifaceExists(const std::string& n) {
    struct stat st;
    return stat(("/sys/class/net/" + n).c_str(), &st) == 0;
}

// Refuse to impair the interface that carries our own SSH session (lock-out protection).
bool carriesSshSession(const std::string& dev) {
    const char* env = getenv("SSH_CONNECTION");
    if (!env) return false;
    std::istringstream in(env);
    std::string cip, cport, sip;
    in >> cip >> cport >> sip;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return false;
    bool hit = false;
    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || dev != p->ifa_name) continue;
        char b[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((sockaddr_in*)p->ifa_addr)->sin_addr, b, sizeof b);
        if (sip == b) hit = true;
    }
    freeifaddrs(ifa);
    return hit;
}

bool preflight(const NetemOptions& o) {
    if (!validIfaceName(o.dev)) { std::cerr << "error: invalid interface name '" << o.dev << "'\n"; return false; }
    if (!o.dry_run && !ifaceExists(o.dev)) { std::cerr << "error: interface '" << o.dev << "' does not exist (see: ip link)\n"; return false; }
    if (!o.dry_run && geteuid() != 0) { std::cerr << "error: root privileges required (use sudo), or try --dry-run\n"; return false; }
    if (!o.dry_run && !o.force && carriesSshSession(o.dev)) {
        std::cerr << "refusing: '" << o.dev << "' carries your SSH session; impairing it could lock you out.\n"
                  << "Use the userspace proxy, a lab namespace (scripts/lab.sh), or pass --force.\n";
        return false;
    }
    return true;
}

bool applyOnce(const NetemOptions& o, const Profile& p) {
    std::vector<std::string> a{"tc", "qdisc", "replace", "dev", o.dev, "root", "netem"};
    for (auto& t : netemArgs(p)) a.push_back(t);
    return runArgv(a, o.dry_run) == 0;
}
}  // namespace

bool validIfaceName(const std::string& n) {
    if (n.empty() || n.size() > 15) return false;
    for (unsigned char c : n) if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

std::vector<std::string> netemArgs(const Profile& p) {
    std::vector<std::string> a;
    if (p.delay_ms > 0) {
        a.push_back("delay");
        a.push_back(num(p.delay_ms) + "ms");
        if (p.jitter_ms > 0) {
            a.push_back(num(p.jitter_ms) + "ms");
            if (p.dist == Dist::Normal) { a.push_back("distribution"); a.push_back("normal"); }
        }
    }
    if (p.ge_p > 0) {  // netem: loss gemodel p r 1-h 1-k  (loss prob in bad / good state)
        for (auto s : {"loss", "gemodel"}) a.push_back(s);
        for (double v : {p.ge_p, p.ge_r, p.ge_bad, p.ge_good}) a.push_back(num(v) + "%");
    } else if (p.loss_pct > 0) {
        a.push_back("loss");
        a.push_back(num(p.loss_pct) + "%");
    }
    if (p.dup_pct > 0) { a.push_back("duplicate"); a.push_back(num(p.dup_pct) + "%"); }
    if (p.corrupt_pct > 0) { a.push_back("corrupt"); a.push_back(num(p.corrupt_pct) + "%"); }
    if (p.reorder_pct > 0 && p.delay_ms > 0) { a.push_back("reorder"); a.push_back(num(p.reorder_pct) + "%"); }
    if (p.rate_kbps > 0) { a.push_back("rate"); a.push_back(num(p.rate_kbps) + "kbit"); }
    return a;
}

std::string netemCommandLine(const std::string& dev, const Profile& p) {
    std::string s = "tc qdisc replace dev " + dev + " root netem";
    for (auto& t : netemArgs(p)) s += " " + t;
    return s;
}

bool netemShow(const NetemOptions& o) {
    if (!validIfaceName(o.dev)) { std::cerr << "error: invalid interface name\n"; return false; }
    return runArgv({"tc", "-s", "qdisc", "show", "dev", o.dev}, o.dry_run) == 0;  // live kernel state
}

bool netemReset(const NetemOptions& o) {
    if (!preflight(o)) return false;
    return runArgv({"tc", "qdisc", "del", "dev", o.dev, "root"}, o.dry_run) == 0;
}

int netemApplyHold(const NetemOptions& o, const Profile& p, double ttl_s, bool persist) {
    std::string e;
    if (!validate(p, e)) { std::cerr << "error: " << e << "\n"; return 2; }
    if (!preflight(o)) return 1;
    installHandlers();
    if (!applyOnce(o, p)) { std::cerr << "error: tc failed to apply the profile\n"; return 1; }
    if (persist || o.dry_run) {
        if (persist) std::cout << "Applied and left in place (--persist). Remove with: chaos netem reset --dev " << o.dev << "\n";
        return 0;
    }
    std::cout << "Active for " << ttl_s << "s on " << o.dev << ". Ctrl+C restores the interface immediately.\n";
    const double start = nowMs();   // wall-clock based: repeated usleep() accumulates drift
    while (!g_sig && nowMs() - start < ttl_s * 1000) usleep(50000);
    std::cout << (g_sig ? "\nsignal received" : "\nTTL expired") << " - removing impairment (dead-man switch)\n";
    return runArgv({"tc", "qdisc", "del", "dev", o.dev, "root"}, false) == 0 ? 0 : 1;
}

int netemRunScenario(const NetemOptions& o, const std::vector<Step>& steps) {
    if (!preflight(o)) return 1;
    installHandlers();
    const double start = nowMs();
    bool applied = false, ok = true;
    for (auto& s : steps) {
        while (!o.dry_run && !g_sig && nowMs() - start < s.at_s * 1000) usleep(50000);
        if (g_sig) break;
        std::cout << "[t=" << s.at_s << "s] " << describe(s.profile) << "\n";
        if (!applyOnce(o, s.profile)) { std::cerr << "error: tc failed\n"; ok = false; break; }
        applied = true;
    }
    if (applied && ok && !o.dry_run && !g_sig) {  // hold the last step for one second, then clean up
        for (int i = 0; i < 10 && !g_sig; i++) usleep(100000);
    }
    if (applied) runArgv({"tc", "qdisc", "del", "dev", o.dev, "root"}, o.dry_run);
    return ok ? 0 : 1;
}

}  // namespace chaos
