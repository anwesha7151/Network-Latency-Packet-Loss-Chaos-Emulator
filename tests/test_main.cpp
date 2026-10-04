// Dependency-free unit tests. Statistical tests use fixed seeds and compare against the
// closed-form expectation (e.g. Gilbert-Elliott steady-state loss = p / (p + r)).
#include "impairment.h"
#include "netem.h"
#include "sim.h"
#include <cmath>
#include <iostream>

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { failures++; std::cerr << "FAIL: " << msg << "  [" #cond "]\n"; } } while (0)
#define NEAR(a, b, tol, msg) CHECK(std::fabs((a) - (b)) <= (tol), msg << " got " << (a) << " expected " << (b) << " +/- " << (tol))

using namespace chaos;

static Profile P(const char* kvs) {
    Profile p; std::string err;
    std::string s = kvs, tok;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == ' ') {
            if (!tok.empty()) { auto eq = tok.find('='); CHECK(applyKV(p, tok.substr(0, eq), tok.substr(eq + 1), err), err); tok.clear(); }
        } else tok += s[i];
    }
    return p;
}

int main() {
    // 1. Bernoulli loss
    { auto r = simulate(P("loss=10"), 200000, 1000, 100, 1); NEAR(r.loss_pct, 10.0, 0.3, "random loss %"); }

    // 2. Gilbert-Elliott: steady-state loss p/(p+r); mean burst length 1/r
    { auto r = simulate(P("ge_p=2 ge_r=25"), 400000, 1000, 100, 2);
      NEAR(r.loss_pct, 100.0 * 2 / (2 + 25), 0.5, "GE steady-state loss");
      NEAR(r.mean_burst, 100.0 / 25, 0.4, "GE mean burst length");
      auto b = simulate(P("loss=7.4"), 400000, 1000, 100, 2);   // same average loss, but independent
      CHECK(r.mean_burst > 2 * b.mean_burst, "burst model must be burstier than random loss at equal rate"); }

    // 3. Delay distributions
    { auto r = simulate(P("delay=100 jitter=20 dist=normal"), 100000, 1000, 100, 3); NEAR(r.delay.avg, 100.0, 0.5, "normal mean");
      auto u = simulate(P("delay=100 jitter=30"), 100000, 1000, 100, 3);
      CHECK(u.delay.min >= 70.0 && u.delay.max <= 130.0, "uniform delay stays within +/- jitter"); NEAR(u.delay.avg, 100.0, 0.5, "uniform mean"); }

    // 4. Token bucket: 1000 x 1000B at t=0 into 800 kbit/s (=100 B/ms), huge queue => ~10 s to drain
    { Profile p = P("rate=800 queue=600000 burst=1000"); Impairment imp(p, 1); double last = 0;
      for (int i = 0; i < 1000; i++) { auto o = imp.process(0, 1000); last = std::max(last, o[0].t_ms); }
      NEAR(last, 9990.0, 15.0, "shaper drain time (ms)"); }
    // ...and tail-drop when the queue limit is small
    { Profile p = P("rate=800 queue=50 burst=1000"); Impairment imp(p, 1); int ok = 0;
      for (int i = 0; i < 100; i++) ok += !imp.process(0, 1000).empty();
      CHECK(ok >= 5 && ok <= 7, "shaper tail-drop accepts only ~queue*rate bytes (accepted " << ok << ")"); }

    // 5. Duplication, corruption, reordering
    { auto r = simulate(P("dup=5"), 200000, 1000, 100, 4); NEAR(100.0 * r.duplicates / r.sent, 5.0, 0.3, "duplicate %"); }
    { auto r = simulate(P("corrupt=3"), 200000, 1000, 100, 5); NEAR(100.0 * r.corrupt / r.sent, 3.0, 0.3, "corrupt %"); }
    { auto r = simulate(P("delay=50 jitter=2 reorder=10"), 100000, 1000, 100, 6); CHECK(r.reordered_arrivals > 5000, "reorder produces out-of-order arrivals"); }
    { auto r = simulate(P("delay=50"), 100000, 1000, 100, 6); CHECK(r.reordered_arrivals == 0, "constant delay never reorders"); }
    // jitter larger than the inter-packet gap (1 ms here) reorders packets "naturally", as on real networks
    { auto r = simulate(P("delay=50 jitter=5"), 100000, 1000, 100, 6); CHECK(r.reordered_arrivals > 1000, "large jitter causes natural reordering"); }

    // 6. Determinism: same seed => identical result; different seed => different
    { auto a = simulate(P("loss=5 delay=10 jitter=5"), 20000, 1000, 100, 9), b = simulate(P("loss=5 delay=10 jitter=5"), 20000, 1000, 100, 9), c = simulate(P("loss=5 delay=10 jitter=5"), 20000, 1000, 100, 10);
      CHECK(a.dropped == b.dropped && a.delay.avg == b.delay.avg, "same seed is reproducible"); CHECK(a.dropped != c.dropped, "different seed differs"); }

    // 7. Profile parsing and validation
    { Profile p; std::string e;
      CHECK(!applyKV(p, "loss", "150", e), "loss > 100 rejected");   CHECK(!applyKV(p, "loss", "abc", e), "non-number rejected");
      CHECK(!applyKV(p, "loss", "5x", e), "trailing junk rejected");  CHECK(!applyKV(p, "bogus", "1", e), "unknown key rejected");
      CHECK(!validate(P("jitter=10"), e), "jitter without delay rejected"); CHECK(!validate(P("ge_p=5"), e), "GE without recovery rejected");
      Profile lte; CHECK(presetByName("lte", lte) && lte.delay_ms == 45, "preset lookup"); CHECK(!presetByName("nope", lte), "unknown preset"); }

    // 8. Scenario parsing
    { std::vector<Step> s; std::string e;
      CHECK(parseScenario("# c\nat 0 lte\nat 10 loss=20 delay=300\nat 20 none\n", s, e) && s.size() == 3, "scenario parses " << e);
      CHECK(s[1].profile.loss_pct == 20 && s[1].profile.jitter_ms == 15, "step modifies previous profile");
      CHECK(s[2].profile.any() == false, "preset 'none' resets");
      CHECK(!parseScenario("at 10 lte\nat 5 lte\n", s, e), "decreasing times rejected");
      CHECK(!parseScenario("at 0 wat\n", s, e), "unknown preset rejected"); CHECK(!parseScenario("", s, e), "empty scenario rejected"); }

    // 9. netem command generation (what the kernel backend will execute)
    { Profile lte; presetByName("lte", lte);
      CHECK(netemCommandLine("eth0", lte) == "tc qdisc replace dev eth0 root netem delay 45ms 15ms distribution normal loss 0.2%", netemCommandLine("eth0", lte));
      Profile w; presetByName("lossy-wifi", w);
      CHECK(netemCommandLine("eth0", w) == "tc qdisc replace dev eth0 root netem delay 8ms 6ms distribution normal loss gemodel 3% 30% 60% 0% reorder 1%", netemCommandLine("eth0", w));
      CHECK(netemCommandLine("lo", Profile()) == "tc qdisc replace dev lo root netem", "empty profile = bare netem"); }

    // 10. Interface-name whitelist (injection resistance)
    CHECK(validIfaceName("eth0") && validIfaceName("enp0s3") && validIfaceName("br-1.20"), "valid names accepted");
    CHECK(!validIfaceName("eth0; rm -rf /") && !validIfaceName("$(id)") && !validIfaceName("") && !validIfaceName("aaaaaaaaaaaaaaaa"), "hostile names rejected");

    std::cout << (checks - failures) << "/" << checks << " checks passed\n";
    return failures ? 1 : 0;
}
