// Simulation tests for PresentScheduler. Not part of the app build; run on the host with:
//   g++ -std=c++17 -O2 -I.. ../present_scheduler.cpp present_scheduler_test.cpp -o present_scheduler_test && ./present_scheduler_test
//
// Frames are presented ahead the way the renderer does with host frame timing: most a little
// under a vsync before their vsync, the rest scattered (frames that arrived late). The
// simulated compositor shows a frame `delay` vsyncs after its vsync if it was presented at least
// `needNs` minus (delay - 1) vsyncs before it, plus some jitter, and reports when it could
// have at the earliest and how much time it had to spare for that (presentMargin), as
// VK_GOOGLE_display_timing does.

#include "present_scheduler.h"

#include <cmath>
#include <cstdio>
#include <random>

using namespace vkr;

namespace {

int failures = 0;

void check(bool ok, const char* scenario, const char* what) {
    if (!ok) {
        printf("  FAIL [%s]: %s\n", scenario, what);
        failures++;
    }
}

struct Scenario {
    const char* name;
    double refreshHz;
    double needMs;              // Slack the compositor needs before a vsync to show a frame one vsync after it
    double needJitterMs = 0.3;  // Standard deviation of that
    double lateShare = 0.02;    // Frames that arrived late, with any slack under a vsync
    int frames = 40000;
    double needMsAfter = -1;    // From halfway, if set: the compositor gets slower
    bool reportsMargin = true;  // Some devices may leave presentMargin at 0
};

struct Result {
    int finalDelay;
    double finalGuardMs;
    int changes;
    double missedShare;  // Of frames presented with at least the guard
};

Result run(const Scenario& s) {
    PresentScheduler scheduler;
    std::mt19937 rng(12345);
    std::normal_distribution<double> jitter(0.0, s.needJitterMs * 1e6);
    std::normal_distribution<double> commit(0.0, 0.08e6);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    const int64_t period = static_cast<int64_t>(std::llround(1e9 / s.refreshHz));
    int64_t vsync = 1'000'000'000;
    int lastDelay = scheduler.delayVsyncs();
    Result r {0, 0, 0, 0};
    int counted = 0, missed = 0;

    for (int i = 0; i < s.frames; i++) {
        vsync += period;
        const double needMs = (s.needMsAfter >= 0 && i >= s.frames / 2) ? s.needMsAfter : s.needMs;
        const int delay = scheduler.delayVsyncs();
        const int64_t guard = scheduler.guardNs();

        // Most frames are committed at the vsync callback before their vsync, which runs about
        // 0.3 ms late. The pacer only commits a frame for a vsync with at least the guard to spare;
        // frames that arrive late get whatever slack is left.
        int64_t slack = period - 300'000 + static_cast<int64_t>(commit(rng));
        if (uniform(rng) < s.lateShare) {
            slack = static_cast<int64_t>(uniform(rng) * static_cast<double>(period));
        }
        const bool late = slack < guard;

        // The compositor: a frame presented with `slack` makes the vsync k after its own if the
        // slack plus (k - 1) vsyncs covers what it needs
        const int64_t need = static_cast<int64_t>(needMs * 1e6 + jitter(rng));
        int earliestK = 1;
        while (slack + (earliestK - 1) * period < need) {
            earliestK++;
        }
        const int shownK = std::max(earliestK, delay);
        const int64_t target = vsync + delay * period;
        const int64_t actual = vsync + shownK * period;
        const int64_t earliest = vsync + earliestK * period - period / 32;
        const int64_t margin = s.reportsMargin ? slack + (earliestK - 1) * period - need : 0;
        const bool failed = actual > target + period / 2;

        scheduler.onAheadOutcome(slack, vsync, delay, true, actual, earliest, margin, period);
        if (!late) {
            scheduler.onPresented(vsync, delay, period, actual, earliest, margin);
            if (i > s.frames / 2) {
                counted++;
                missed += failed ? 1 : 0;
            }
        }
        if (scheduler.delayVsyncs() != lastDelay) {
            printf("    frame %6d: delay %d -> %d, guard %.2f ms\n", i, lastDelay, scheduler.delayVsyncs(),
                   scheduler.guardNs() / 1e6);
            lastDelay = scheduler.delayVsyncs();
            r.changes++;
        }
    }
    r.finalDelay = scheduler.delayVsyncs();
    r.finalGuardMs = scheduler.guardNs() / 1e6;
    r.missedShare = counted ? static_cast<double>(missed) / counted : 0;
    return r;
}

Result runAndReport(const Scenario& s) {
    printf("%s\n", s.name);
    Result r = run(s);
    printf("  ends at delay %d, guard %.2f ms, %d change(s), %.2f%% of the second half's frames missed\n",
           r.finalDelay, r.finalGuardMs, r.changes, r.missedShare * 100);
    return r;
}

}  // namespace

int main() {
    {
        // iPlay 70 mini Ultra: frames needed 5.1-5.9 ms for one vsync (1st-99th percentile)
        Scenario s {"144 Hz, compositor needs 5.4 ms for one vsync", 144.0, 5.4};
        s.needJitterMs = 0.2;
        Result r = runAndReport(s);
        check(r.finalDelay == 1, s.name, "should lower to a delay of one vsync");
        check(r.finalGuardMs >= 5.5 && r.finalGuardMs <= 6.44, s.name, "guard should cover what the compositor needs");
        check(r.missedShare < 0.004, s.name, "frames with the guard to spare should all but never miss");
    }
    {
        // Without margins, what frames needed is unknown: stay put
        Scenario s {"144 Hz, 5.4 ms, no presentMargin", 144.0, 5.4};
        s.needJitterMs = 0.2;
        s.reportsMargin = false;
        Result r = runAndReport(s);
        check(r.finalDelay == 2, s.name, "shouldn't lower without knowing what frames need");
    }
    {
        // Frames need more than a vsync of notice: one vsync can't work
        Scenario s {"144 Hz, compositor needs 8 ms for one vsync", 144.0, 8.0};
        Result r = runAndReport(s);
        check(r.finalDelay == 2, s.name, "should stay at two vsyncs");
        check(r.finalGuardMs <= 3.5, s.name, "guard shouldn't take on what only one vsync needs");
    }
    {
        // Not worth it: the guard would grow by more than most of the vsync saved
        Scenario s {"240 Hz, compositor needs 3.9 ms for one vsync", 240.0, 3.9};
        Result r = runAndReport(s);
        check(r.finalDelay == 2, s.name, "a guard near a whole vsync isn't worth the vsync it saves");
    }
    {
        Scenario s {"60 Hz, compositor needs 4 ms for one vsync", 60.0, 4.0};
        Result r = runAndReport(s);
        check(r.finalDelay == 1, s.name, "should lower to a delay of one vsync");
        check(r.finalGuardMs >= 4.0 && r.finalGuardMs <= 5.5, s.name, "guard should cover what the compositor needs");
    }
    {
        // Lowered, then the compositor slows down: back to two vsyncs and the guard from before
        Scenario s {"144 Hz, compositor slows from 5.4 to 7.5 ms halfway", 144.0, 5.4};
        s.needJitterMs = 0.2;
        s.needMsAfter = 7.5;
        Result r = runAndReport(s);
        check(r.finalDelay == 2, s.name, "should go back to two vsyncs");
        check(r.finalGuardMs <= 3.5, s.name, "should go back to the guard from before lowering");
    }

    printf(failures ? "\n%d check(s) failed\n" : "\nAll checks passed\n", failures);
    return failures ? 1 : 0;
}
