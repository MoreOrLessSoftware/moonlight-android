// Simulation tests for FramePacer. Not part of the app build; run on the host with:
//   g++ -std=c++17 -O2 -I.. ../frame_pacer.cpp frame_pacer_test.cpp -o frame_pacer_test && ./frame_pacer_test
//
// Each scenario generates host frame timestamps the way Sunshine produces them (frames taken
// as the host display presents them, optionally thinned by its frame_rate_limiter_t), adds
// encode/network/decode delay with jitter, and plays the result against a local vsync
// grid through the pacer.

#include "frame_pacer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <vector>

using namespace vkr;

namespace {

struct Scenario {
    const char* name;
    double hostDisplayHz;      // Host display refresh
    int streamFps;             // Client's requested frame rate
    double localDisplayHz;     // Client display refresh
    double transitMs = 20.0;   // Fixed part of encode + network + decode
    double jitterMs = 4.0;     // Uniform jitter on top of that
    double spikeChance = 0.0;  // Chance per frame of a network stall
    double spikeMs = 0.0;
    double durationS = 60.0;
    PacingMode mode = PacingMode::HostTimed;
    double vsyncPhase = 0.07;  // Where the local vsync grid sits against host frames, in periods
    double contentFps = 0;     // Frame cap on the host (e.g. RTSS), 0 for a new frame every refresh
    bool hostVrr = false;      // Host display refreshes when the game presents (G-Sync/FreeSync)
    double reportedDisplayHz = 0;  // What Android reports, if not the true rate; refined from vsyncs
    double contentJitterMs = 0;    // Standard deviation of the game's present times around its cap
    // The checks here were written against what's now LowLatency
    JitterBuffer jitterBuffer = JitterBuffer::LowLatency;
};

struct Result {
    int shown = 0;
    int skipped = 0;
    int emptySlots = 0;        // Vsyncs without a new frame where the stream should have had one
    double meanLatencyMs = 0;  // Vsync time minus frame arrival
    double maxLatencyMs = 0;
    double cadenceErrorMs = 0; // Mean |display interval - host interval| between shown frames
    int heldShort = 0;         // Frames on screen for fewer vsyncs than a slot
    int heldLong = 0;          // Frames on screen for more vsyncs than a slot
};

// Host present times of the frames Sunshine sends, in ns
std::vector<int64_t> hostFrames(const Scenario& s) {
    std::mt19937 rng(99);
    std::normal_distribution<double> gameJitter(0.0, s.contentJitterMs * 1e6);
    const double hostPeriod = 1e9 / s.hostDisplayHz;
    const double interval = 1e9 / s.streamFps;
    std::vector<int64_t> pts;

    // Sunshine's frame_rate_limiter_t: the host display is let run 1% fast when it is within
    // 5% of the stream rate, otherwise thinned to exactly the stream rate
    const bool allowFast = s.hostDisplayHz <= s.streamFps * 1.05;
    const double minInterval = allowFast ? interval * 100.0 / 101.0 : interval;
    const double allowance = interval / 2;
    double theoretical = 0;

    // When the desktop shows each game frame: at once on a VRR display, else at the next refresh
    std::vector<double> presents;
    for (int64_t k = 0;; k++) {
        double present;
        if (s.contentFps > 0) {
            const double game = k * 1e9 / s.contentFps + (s.contentJitterMs > 0 ? gameJitter(rng) : 0.0);
            present = s.hostVrr ? game : std::ceil(game / hostPeriod - 1e-9) * hostPeriod;
            if (!presents.empty() && present <= presents.back()) {
                continue;
            }
        }
        else {
            present = k * hostPeriod;
        }
        if (present > s.durationS * 1e9) {
            break;
        }
        presents.push_back(present);
    }

    for (double present : presents) {
        // Capture sleeps until next_due(), then takes the next frame the desktop presents
        if (present < theoretical - allowance) {
            continue;
        }
        pts.push_back(static_cast<int64_t>(present));
        theoretical = std::max(theoretical, present) + minInterval;
    }
    return pts;
}

Result run(const Scenario& s, bool verbose = false) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    const std::vector<int64_t> pts = hostFrames(s);
    const int64_t clockOffset = 5'000'000'000LL;  // Host and client clocks differ
    std::vector<int64_t> arrival(pts.size());
    int64_t last = 0;
    for (size_t i = 0; i < pts.size(); i++) {
        double delay = s.transitMs + uniform(rng) * s.jitterMs;
        if (uniform(rng) < s.spikeChance) {
            delay += s.spikeMs;
        }
        // The decoder hands frames out in order
        int64_t a = pts[i] + clockOffset + static_cast<int64_t>(delay * 1e6);
        a = std::max(a, last + 200'000);
        arrival[i] = last = a;
    }

    const int64_t period = static_cast<int64_t>(1e9 / s.localDisplayHz);
    // Starts from the rate Android reports and refines it from vsync intervals, like the renderer
    int64_t estimatedPeriod = s.reportedDisplayHz > 0 ? static_cast<int64_t>(1e9 / s.reportedDisplayHz) : period;
    FramePacer pacer(s.mode, s.streamFps, estimatedPeriod, s.jitterBuffer);

    struct Queued {
        FrameTiming timing;
        size_t index;
    };
    std::deque<Queued> queue;
    size_t next = 0;

    Result r;
    double latencySum = 0;
    double cadenceSum = 0;
    int cadenceCount = 0;
    int64_t lastShownVsync = 0;
    size_t lastShownIndex = 0;
    bool haveShown = false;
    const int64_t warmup = clockOffset + 3'000'000'000LL;
    const int64_t end = clockOffset + static_cast<int64_t>(s.durationS * 1e9);
    const double contentFps = s.contentFps > 0 ? s.contentFps : s.streamFps;
    const int64_t slotVsyncs = std::max<int64_t>(1, std::llround((1e9 / contentFps) / period));

    for (int64_t vsync = clockOffset + static_cast<int64_t>(s.vsyncPhase * period); vsync < end; vsync += period) {
        // Frames that arrived before the callback ran, just after the vsync
        while (next < pts.size() && arrival[next] <= vsync + 500'000) {
            Queued q {{pts[next], arrival[next], 0}, next};
            pacer.onFrameArrived(q.timing);
            queue.push_back(q);
            while (queue.size() > pacer.maxQueued()) {
                queue.pop_front();
                if (vsync > warmup) r.skipped++;
            }
            next++;
        }

        if (estimatedPeriod != period) {
            estimatedPeriod += (period - estimatedPeriod) / 16;
            pacer.setVsyncPeriod(estimatedPeriod);
        }

        std::vector<FrameTiming> timings;
        for (const auto& q : queue) timings.push_back(q.timing);
        const int choice = pacer.onVsync(vsync, timings.data(), timings.size());

        const bool counting = vsync > warmup;
        if (choice >= 0) {
            const Queued shown = queue[choice];
            queue.erase(queue.begin(), queue.begin() + choice + 1);
            if (counting) {
                r.shown++;
                r.skipped += choice;
                const double latency = (vsync - shown.timing.arrivalNs) / 1e6;
                latencySum += latency;
                r.maxLatencyMs = std::max(r.maxLatencyMs, latency);
                if (haveShown && shown.index == lastShownIndex + 1) {
                    const double displayed = (vsync - lastShownVsync) / 1e6;
                    const double host = (pts[shown.index] - pts[lastShownIndex]) / 1e6;
                    cadenceSum += std::fabs(displayed - host);
                    cadenceCount++;
                }
            }
            if (counting && haveShown) {
                const int64_t held = (vsync - lastShownVsync + period / 2) / period;
                if (held < slotVsyncs) r.heldShort++;
                if (held > slotVsyncs) r.heldLong++;
                if (getenv("PACER_TRACE") && (held != slotVsyncs || choice > 0)) {
                    printf("    t=%7.3f s held %lld skip %d  shift %.2f ms parity %lld locked %d slot %lld drift %.5f\n",
                           (vsync - clockOffset) / 1e9, (long long) held, choice, pacer.phaseShiftNs() / 1e6,
                           (long long) pacer.slotParity(), pacer.phaseLocked(), (long long) pacer.slotVsyncs(),
                           pacer.phaseDriftPerFrame());
                }
            }
            lastShownVsync = vsync;
            lastShownIndex = shown.index;
            haveShown = true;
        }
        else if (counting && haveShown && vsync - lastShownVsync >= slotVsyncs * period &&
                 (vsync - lastShownVsync) % (slotVsyncs * period) == 0) {
            r.emptySlots++;
        }
    }

    r.meanLatencyMs = r.shown ? latencySum / r.shown : 0;
    r.cadenceErrorMs = cadenceCount ? cadenceSum / cadenceCount : 0;
    if (verbose) {
        printf("  buffer %.2f ms, shift %.2f ms, locked %d\n",
               (pacer.timeline().bufferNs()) / 1e6, pacer.phaseShiftNs() / 1e6, pacer.phaseLocked());
    }
    return r;
}

int failures = 0;

void check(bool ok, const char* scenario, const char* what) {
    if (!ok) {
        printf("  FAIL %s: %s\n", scenario, what);
        failures++;
    }
}

void report(const Scenario& s, const Result& r) {
    printf("%-44s shown %5d  skipped %3d  short %3d  long %3d  latency %5.1f ms (max %5.1f)  cadence err %.2f ms\n",
           s.name, r.shown, r.skipped, r.heldShort, r.heldLong, r.meanLatencyMs, r.maxLatencyMs, r.cadenceErrorMs);
}

}  // namespace

int main() {
    const double seconds = 57.0;  // After warm-up

    {
        Scenario s {"60 Hz host, 60 fps, 60 Hz display", 60.0, 60, 60.0};
        Result r = run(s, true);
        report(s, r);
        // A matched host and display should show every frame exactly once
        check(r.skipped <= 2, s.name, "frames skipped");
        check(r.emptySlots <= 2, s.name, "frames repeated");
        check(r.cadenceErrorMs < 1.0, s.name, "uneven cadence");
    }
    {
        // The limiter lets a display up to 1% fast through, so the stream runs 0.5% fast and
        // one frame in 200 has to go. They should go one at a time, not with repeats, and
        // latency must not build up.
        Scenario s {"60.3 Hz host (0.5% fast), 60 Hz display", 60.3, 60, 60.0};
        Result r = run(s, true);
        report(s, r);
        const double expectedSkips = 0.3 * seconds;
        check(r.skipped >= expectedSkips * 0.7 && r.skipped <= expectedSkips * 1.5, s.name, "skips not near rate difference");
        check(r.emptySlots <= r.skipped / 4 + 2, s.name, "skips come with repeats");
        check(r.maxLatencyMs < 45.0, s.name, "latency built up");
    }
    {
        Scenario s {"59.94 Hz host, 60 Hz display", 59.94, 60, 60.0};
        Result r = run(s, true);
        report(s, r);
        const double expectedRepeats = 0.06 * seconds;
        check(r.emptySlots <= expectedRepeats * 2 + 2, s.name, "too many repeats");
        check(r.skipped <= r.emptySlots / 4 + 2, s.name, "repeats come with skips");
    }
    {
        // A 144 Hz host thinned to 60 fps: host timestamps sit on the 144 Hz grid, so their
        // spacing alternates between 2 and 3 host frames. Still one frame per vsync.
        Scenario s {"144 Hz host thinned to 60 fps, 60 Hz display", 144.0, 60, 60.0};
        Result r = run(s, true);
        report(s, r);
        check(r.skipped <= seconds * 0.5, s.name, "frames skipped");
        check(r.emptySlots <= seconds * 0.5, s.name, "frames repeated");
    }
    {
        Scenario s {"60 Hz host, 60 fps, 120 Hz display", 60.0, 60, 120.0};
        Result r = run(s, true);
        report(s, r);
        check(r.skipped <= 2, s.name, "frames skipped");
        check(r.emptySlots <= 2, s.name, "frames repeated");
        check(r.cadenceErrorMs < 1.0, s.name, "uneven cadence");
    }
    {
        Scenario s {"120 Hz host, 120 fps, 120 Hz display", 120.0, 120, 120.0};
        s.jitterMs = 3.0;
        Result r = run(s, true);
        report(s, r);
        check(r.skipped <= 4, s.name, "frames skipped");
        check(r.emptySlots <= 4, s.name, "frames repeated");
    }
    {
        // Wi-Fi style stalls: 2% of frames 25 ms late. The buffer covers 98% of transit
        // times, so the stalled frames are shown late rather than raising latency for all.
        Scenario s {"60 Hz, 2% of frames delayed 25 ms", 60.0, 60, 60.0};
        s.spikeChance = 0.02;
        s.spikeMs = 25.0;
        Result r = run(s, true);
        report(s, r);
        check(r.meanLatencyMs < 20.0, s.name, "latency too high");
    }
    {
        // Every phase of the local vsync grid against the host's frames, including ones where
        // frames land right on a vsync
        const Scenario bases[] = {
            {"phase sweep: 60 Hz host, 60 Hz display", 60.0, 60, 60.0},
            {"phase sweep: 120 Hz host, 120 Hz display", 120.0, 120, 120.0},
            {"phase sweep: 60 Hz host, 120 Hz display", 60.0, 60, 120.0},
            {"phase sweep: 144 Hz host thinned, 60 Hz", 144.0, 60, 60.0},
        };
        for (Scenario s : bases) {
            s.durationS = 20.0;
            Result worst;
            double worstLatency = 0;
            for (int i = 0; i < 20; i++) {
                s.vsyncPhase = i / 20.0;
                Result r = run(s);
                worst.skipped = std::max(worst.skipped, r.skipped);
                worst.emptySlots = std::max(worst.emptySlots, r.emptySlots);
                worst.heldShort = std::max(worst.heldShort, r.heldShort);
                worst.heldLong = std::max(worst.heldLong, r.heldLong);
                worstLatency = std::max(worstLatency, r.meanLatencyMs);
                worst.maxLatencyMs = std::max(worst.maxLatencyMs, r.maxLatencyMs);
            }
            printf("%-44s worst of 20 phases: skipped %d, short %d, long %d, mean latency %.1f ms (max %.1f)\n",
                   s.name, worst.skipped, worst.heldShort, worst.heldLong, worstLatency, worst.maxLatencyMs);
            check(worst.skipped <= 1 && worst.heldShort <= 2 && worst.heldLong <= 2, s.name,
                  "uneven frames at some phase");
        }
    }
    {
        // Frequent stalls are worth buffering for: fewer skips at the cost of latency
        Scenario s {"60 Hz, 10% of frames delayed 12 ms", 60.0, 60, 60.0};
        s.spikeChance = 0.10;
        s.spikeMs = 12.0;
        Result r = run(s, true);
        report(s, r);
        check(r.skipped <= 10, s.name, "stalls not buffered");
    }
    {
        // Android set to "144 Hz" on a panel that really runs at 143.64 Hz, stream at 72 fps,
        // game capped at 71.82 fps (half the panel rate) with RTSS. Every frame should get
        // exactly two vsyncs, whatever the host display does.
        const double panelHz = 143.64;
        Scenario cases[] = {
            {"144/72/71.82: host 144 Hz fixed refresh", 144.0, 72, panelHz},
            {"144/72/71.82: host 165 Hz fixed refresh", 165.0, 72, panelHz},
            {"144/72/71.82: host 240 Hz fixed refresh", 240.0, 72, panelHz},
            {"144/72/71.82: host VRR", 144.0, 72, panelHz},
        };
        cases[3].hostVrr = true;
        for (Scenario s : cases) {
            s.contentFps = 71.82;
            s.reportedDisplayHz = 144.0;
            s.durationS = 20.0;
            Result worst;
            double worstLatency = 0;
            for (int i = 0; i < 20; i++) {
                s.vsyncPhase = i / 20.0;
                Result r = run(s);
                worst.skipped = std::max(worst.skipped, r.skipped);
                worst.emptySlots = std::max(worst.emptySlots, r.emptySlots);
                worst.heldShort = std::max(worst.heldShort, r.heldShort);
                worst.heldLong = std::max(worst.heldLong, r.heldLong);
                worstLatency = std::max(worstLatency, r.meanLatencyMs);
                worst.maxLatencyMs = std::max(worst.maxLatencyMs, r.maxLatencyMs);
            }
            printf("%-44s worst of 20 phases: skipped %d, short %d, long %d, mean latency %.1f ms (max %.1f)\n",
                   s.name, worst.skipped, worst.heldShort, worst.heldLong, worstLatency, worst.maxLatencyMs);
            check(worst.skipped <= 1 && worst.heldShort <= 2 && worst.heldLong <= 2, s.name,
                  "uneven frames at some phase");
        }

        // The same setup when the cap doesn't exactly match half the panel rate. The rate
        // difference forces an occasional 1- or 3-vsync frame; check there are no more than that.
        struct Mismatch { const char* name; double panelHz; double capFps; };
        const Mismatch mismatches[] = {
            {"panel 143.60 Hz, cap 71.82 (host 0.03% fast)", 143.60, 71.82},
            {"panel 144.00 Hz, cap 71.82 (host 0.25% slow)", 144.00, 71.82},
            {"panel 143.64 Hz, cap 72.00 (host 0.25% fast)", 143.64, 72.00},
        };
        for (const Mismatch& m : mismatches) {
            Scenario s {m.name, 165.0, 72, m.panelHz};
            s.contentFps = m.capFps;
            s.reportedDisplayHz = 144.0;
            s.durationS = 120.0;
            Result r = run(s, true);
            report(s, r);
            // Vsyncs the stream gains or loses against the panel after warm-up. Each costs one
            // frame held a vsync short or long; a skipped frame costs two.
            const double driftVsyncs = std::fabs(m.capFps * 2 - m.panelHz) * 117.0;
            const int events = r.heldShort + r.heldLong + 2 * r.skipped;
            check(events <= driftVsyncs * 1.3 + 4, m.name,
                  "more uneven frames than the rate difference needs");
        }
    }
    {
        // Host display at 143.64 Hz too, with RTSS capping the game to 71.82 fps on the CPU's
        // clock. The cap and the host display drift slowly against each other, and while the
        // game's presents sit near a host refresh, RTSS's timing jitter lands consecutive frames
        // one refresh early or late: host timestamps alternate 1, 2 and 3 refreshes apart for a
        // few seconds at a time. The game itself is evenly paced, so every frame should still get
        // two vsyncs, apart from the corrections the rate difference needs.
        struct Clocks { const char* name; double capError; double panelError; };
        const Clocks clocks[] = {
            {"143.64 host, cap 0.01% fast vs host", 1e-4, 0},
            {"143.64 host, cap 0.01% slow vs host", -1e-4, 0},
            {"143.64 host, cap 0.003% fast, panel 20 ppm", 3e-5, 2e-5},
            {"143.64 host, cap 0.03% slow, panel -20 ppm", -3e-4, -2e-5},
        };
        for (const Clocks& c : clocks) {
            Scenario s {c.name, 143.64, 72, 143.64 * (1 + c.panelError)};
            s.contentFps = 71.82 * (1 + c.capError);
            s.contentJitterMs = 0.3;
            s.reportedDisplayHz = 144.0;
            s.durationS = 120.0;
            Result r = run(s, true);
            report(s, r);
            const double driftVsyncs = std::fabs(s.contentFps * 2 - s.localDisplayHz) * 117.0;
            const int events = r.heldShort + r.heldLong + 2 * r.skipped;
            printf("  minimum uneven frames for the rate difference: %.1f, got %d\n", driftVsyncs, events);
            check(events <= driftVsyncs * 1.3 + 4, c.name, "more uneven frames than the rate difference needs");
        }
    }
    {
        // For comparison: the existing balanced mode on a fast host keeps a full queue
        Scenario s {"Balanced mode, 60.3 Hz host, 60 Hz display", 60.3, 60, 60.0};
        s.mode = PacingMode::Balanced;
        Result r = run(s);
        report(s, r);
    }

    printf(failures ? "\n%d check(s) failed\n" : "\nAll checks passed\n", failures);
    return failures ? 1 : 0;
}
