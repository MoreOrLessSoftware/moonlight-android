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
    int slotVsyncs = 1;         // Vsyncs per frame (2 for 72 fps on 144 Hz)
    int slotVsyncsAfter = 0;    // From halfway, if set: the game's frame rate changes
    double hiccupEveryS = 0;    // If set: this often, the stream pauses for about 90 ms, and the
                                // compositor shows the two frames after it a vsync late however
                                // early they were presented (the iPlay 70 mini Ultra at 120 Hz)
    double slowEveryS = 0;      // If set: this often, the compositor needs slowNeedMs for slowForS
    double slowForS = 0;
    double slowNeedMs = 0;
    double startNeedMs = 0;     // If set: the compositor needs this for the first startForS
    double startForS = 0;
    bool presentAtEdge = false; // Frames presented as early as the window allows, as in steady
                                // play (on the device, nearly all within a millisecond of it)
    unsigned seed = 12345;
};

struct Result {
    int finalDelay;
    double finalGuardMs;
    int changes;
    double missedShare;  // Of frames presented with at least the guard
    double firstChangeS; // Into the session, or -1 if the delay never changed
};

Result run(const Scenario& s, bool verbose = true) {
    PresentScheduler scheduler;
    std::mt19937 rng(s.seed);
    std::normal_distribution<double> jitter(0.0, s.needJitterMs * 1e6);
    std::normal_distribution<double> commit(0.0, 0.08e6);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    const int64_t period = static_cast<int64_t>(std::llround(1e9 / s.refreshHz));
    const int64_t startNs = 1'000'000'000;
    int64_t vsync = startNs;
    int lastDelay = scheduler.delayVsyncs();
    Result r {0, 0, 0, 0, -1};
    int counted = 0, missed = 0;
    int resumeLate = 0;

    for (int i = 0; i < s.frames; i++) {
        const bool secondHalf = i >= s.frames / 2;
        const int slotVsyncs = (s.slotVsyncsAfter > 0 && secondHalf) ? s.slotVsyncsAfter : s.slotVsyncs;
        vsync += period * slotVsyncs;
        double needMs = (s.needMsAfter >= 0 && secondHalf) ? s.needMsAfter : s.needMs;
        if (s.slowEveryS > 0 && std::fmod((vsync - startNs) / 1e9, s.slowEveryS) >= s.slowEveryS - s.slowForS) {
            needMs = s.slowNeedMs;
        }
        if (s.startForS > 0 && (vsync - startNs) / 1e9 < s.startForS) {
            needMs = s.startNeedMs;
        }

        // Frames can be committed up to a slot ahead (FramePacer::presentAheadVsyncs())
        const int64_t window = period * slotVsyncs;
        scheduler.setAheadWindowNs(window);
        const int delay = scheduler.delayVsyncs();
        const int64_t guard = scheduler.guardNs();

        // A frame is committed when it arrives, or at the vsync callback (about 0.3 ms after a
        // vsync) that brings its vsync within the window: from the guard to the window before
        // its vsync. Frames that arrive late get whatever slack is left.
        const int64_t latest = window - 300'000 + static_cast<int64_t>(commit(rng));
        int64_t slack = s.presentAtEdge ? std::max(latest, guard)
                : guard + static_cast<int64_t>(uniform(rng) * static_cast<double>(std::max<int64_t>(latest - guard, 0)));
        if (uniform(rng) < s.lateShare) {
            slack = static_cast<int64_t>(uniform(rng) * static_cast<double>(window));
        }
        const bool late = slack < guard;

        // The compositor: a frame presented with `slack` makes the vsync k after its own if the
        // slack plus (k - 1) vsyncs covers what it needs
        const int64_t need = static_cast<int64_t>(needMs * 1e6 + jitter(rng));
        int earliestK = 1;
        while (slack + (earliestK - 1) * period < need) {
            earliestK++;
        }
        int shownK = std::max(earliestK, delay);
        if (s.hiccupEveryS > 0) {
            const double phaseS = std::fmod((vsync - startNs) / 1e9, s.hiccupEveryS);
            if (phaseS >= s.hiccupEveryS - 0.09) {
                // Paused: no frame
                resumeLate = 2;
                continue;
            }
            if (resumeLate > 0) {
                resumeLate--;
                shownK++;
            }
        }
        const int64_t target = vsync + delay * period;
        const int64_t actual = vsync + shownK * period;
        const int64_t earliest = vsync + earliestK * period - period / 32;
        const int64_t margin = s.reportsMargin ? slack + (earliestK - 1) * period - need : 0;
        const bool failed = actual > target + period / 2;

        scheduler.onAheadOutcome(slack, vsync, delay, true, actual, earliest, margin, period);
        if (!late) {
            scheduler.onPresented(vsync, delay, period, actual, earliest, margin, slack);
            if (i > s.frames / 2) {
                counted++;
                missed += failed ? 1 : 0;
            }
        }
        if (scheduler.delayVsyncs() != lastDelay) {
            const double atS = (vsync - startNs) / 1e9;
            if (verbose) {
                printf("    %6.1f s: delay %d -> %d, guard %.2f ms\n", atS, lastDelay, scheduler.delayVsyncs(),
                       scheduler.guardNs() / 1e6);
            }
            if (r.firstChangeS < 0) {
                r.firstChangeS = atS;
            }
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
        // iPlay 70 mini Ultra: frames needed 5.1-5.9 ms for one vsync (1st-99th percentile),
        // streaming 72 fps on 144 Hz
        Scenario s {"144 Hz at 72 fps, compositor needs 5.4 ms for one vsync", 144.0, 5.4};
        s.needJitterMs = 0.2;
        s.slotVsyncs = 2;
        Result r = runAndReport(s);
        check(r.finalDelay == 1, s.name, "should lower to a delay of one vsync");
        check(r.firstChangeS > 0 && r.firstChangeS <= 10, s.name, "should lower within 10 s");
        check(r.finalGuardMs >= 5.5 && r.finalGuardMs <= 6.44, s.name, "guard should cover what the compositor needs");
        check(r.missedShare < 0.004, s.name, "frames with the guard to spare should all but never miss");
    }
    {
        // The iPlay on a slower evening (6.2-6.3 ms at the 99th percentile): more than one of
        // its vsyncs leaves, but 72 fps gives frames two
        Scenario s {"144 Hz at 72 fps, compositor needs 5.75 ms", 144.0, 5.75};
        s.needJitterMs = 0.2;
        s.slotVsyncs = 2;
        Result r = runAndReport(s);
        check(r.finalDelay == 1, s.name, "with two vsyncs a frame, should lower to one vsync");
        check(r.missedShare < 0.004, s.name, "frames with the guard to spare should all but never miss");
    }
    {
        // The same at 144 fps, just over the edge of one vsync. Learned over only seconds, how
        // often does it lower anyway and have to come back?
        int lowered = 0, wrongAtEnd = 0;
        const int runs = 20;
        for (int i = 0; i < runs; i++) {
            Scenario s {"144 Hz at 144 fps, compositor needs 5.75 ms", 144.0, 5.75};
            s.needJitterMs = 0.2;
            s.seed = 1000 + i;
            Result r = run(s, false);
            lowered += r.firstChangeS >= 0 ? 1 : 0;
            wrongAtEnd += r.finalDelay != 2 ? 1 : 0;
        }
        printf("144 Hz at 144 fps, compositor needs 5.75 ms (p99 about 6.2 ms), %d runs\n", runs);
        printf("  lowered in %d, still lowered at the end in %d\n", lowered, wrongAtEnd);
        check(lowered <= runs / 4, "just over the edge", "seconds of learning shouldn't often lower wrongly");
        check(wrongAtEnd == 0, "just over the edge", "a wrong lowering should be undone");
    }
    {
        // The iPlay at 144 fps, needing about a 6 ms guard for one vsync: every session should
        // decide the same, however far the guard has come down by then (it once lowered only in
        // sessions whose guard was still near 2 ms)
        int lowered = 0;
        const int runs = 20;
        for (int i = 0; i < runs; i++) {
            Scenario s {"144 Hz at 144 fps, compositor needs 5.4 ms", 144.0, 5.4};
            s.needJitterMs = 0.2;
            s.seed = 2000 + i;
            Result r = run(s, false);
            lowered += r.finalDelay == 1 ? 1 : 0;
        }
        printf("144 Hz at 144 fps, compositor needs 5.4 ms, %d runs\n", runs);
        printf("  at one vsync at the end in %d\n", lowered);
        check(lowered == runs, "144 fps, 5.4 ms", "every session should lower to one vsync");
    }
    {
        // The iPlay at 60 fps on 120 Hz: every so often the compositor shows a few frames a vsync
        // late within a couple of seconds, at any delay. That's no reason to raise the delay.
        Scenario s {"120 Hz at 60 fps, 7.1 ms, compositor hiccups every 30 s", 120.0, 7.1};
        s.needJitterMs = 0.2;
        s.slotVsyncs = 2;
        s.hiccupEveryS = 30;
        Result r = runAndReport(s);
        check(r.finalDelay == 1, s.name, "should lower to one vsync and stay there through the hiccups");
        check(r.changes == 1, s.name, "hiccups shouldn't move the delay");
    }
    {
        // The compositor slows for 3 s every minute: each time, back to two vsyncs, and one
        // again seconds after it recovers. Once, the slow seconds stayed in what was learned
        // and kept two vsyncs for three minutes; and each slowdown doubled the wait to retry.
        Scenario s {"144 Hz at 144 fps, 5.4 ms, compositor slows to 7.5 ms for 3 s a minute", 144.0, 5.4};
        s.needJitterMs = 0.2;
        s.frames = 144 * 290;
        s.slowEveryS = 60;
        s.slowForS = 3;
        s.slowNeedMs = 7.5;
        Result r = runAndReport(s);
        check(r.changes >= 9, s.name, "should go back to one vsync after every slowdown");
        check(r.finalDelay == 1, s.name, "should end at one vsync");
    }
    {
        // PyroWave on the iPlay at 144 fps: frames need 5.6 ms for one vsync at the median but
        // 9.5 ms at the 99th percentile, so two vsyncs it is. A slow start (the host idling on
        // its desktop) raises the guard; then nearly every frame is presented a vsync ahead, and
        // none with little slack show a lower guard is safe. It should still come down to what
        // two vsyncs need (about 2.6 ms at the 99th percentile), not stay where the start left it.
        Scenario s {"144 Hz at 144 fps, 6.5 +/- 1.2 ms after a 20 s start needing 10 ms", 144.0, 6.5};
        s.needJitterMs = 1.2;
        s.startNeedMs = 10.0;
        s.startForS = 20;
        s.presentAtEdge = true;
        s.frames = 144 * 120;
        Result r = runAndReport(s);
        check(r.finalDelay == 2, s.name, "should stay at two vsyncs");
        check(r.finalGuardMs >= 2.0 && r.finalGuardMs <= 3.75, s.name, "guard should come down to what two vsyncs need");
        check(r.missedShare < 0.004, s.name, "frames with the guard to spare should all but never miss");
    }
    {
        // Lowered at 72 fps, then the game runs at 144 fps: the guard no longer fits a frame's
        // window, so straight back to two vsyncs
        Scenario s {"144 Hz, 72 then 144 fps, compositor needs 5.75 ms", 144.0, 5.75};
        s.needJitterMs = 0.2;
        s.slotVsyncs = 2;
        s.slotVsyncsAfter = 1;
        Result r = runAndReport(s);
        check(r.changes >= 2, s.name, "should lower at 72 fps and come back up at 144 fps");
        check(r.finalDelay == 2, s.name, "should end at two vsyncs");
        check(r.finalGuardMs <= 3.5, s.name, "should go back to the guard from before lowering");
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
        check(r.firstChangeS > 0 && r.firstChangeS <= 10, s.name, "should lower within 10 s");
        check(r.finalDelay == 2, s.name, "should go back to two vsyncs");
        check(r.finalGuardMs <= 3.5, s.name, "should go back to the guard from before lowering");
    }

    printf(failures ? "\n%d check(s) failed\n" : "\nAll checks passed\n", failures);
    return failures ? 1 : 0;
}
