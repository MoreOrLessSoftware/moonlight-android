// Simulation tests for FramePacer's host frame timing pacing (see FRAME_PACING.md). Not part of
// the app build; build and run on the host with:
//   g++ -std=c++17 -O2 -I.. ../frame_pacer.cpp frame_pacer_test.cpp -o frame_pacer_test
//   ./frame_pacer_test
// Exits non-zero if any check fails.
//
// Each scenario streams host frames with known timing to a simulated display, through the pacer,
// with and without presenting ahead. The checks are the properties the pacer promises, not exact
// counts: frames keep the host's cadence, the delay grows only for frames that need it and comes
// back, no frame waits past the preset's cap, and a host off our rate costs about one uneven
// frame per vsync of drift.

#include "frame_pacer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace vkr;

namespace {

struct Scenario {
    std::string name;
    double refreshHz = 143.655;  // What the panel really runs at; the pacer is told 144
    double seconds = 30;
    // The host's frame interval at a time into the stream, in seconds
    std::function<double(double)> hostInterval;
    double transitMs = 5;
    double jitterMs = 0.5;  // Standard deviation of transit
    // Extra transit for the frame at about this time (s, ms)
    std::vector<std::pair<double, double>> spikes;
    // Frames whose host timestamps fall in this window arrive together at its end
    double stallAtS = -1;
    double stallMs = 0;
    // The host skips a frame this often (s), or never
    double hostGapEveryS = 0;
    JitterBuffer jitterBuffer = JitterBuffer::LowLatency;
    double switchAtS = -1;
    JitterBuffer switchTo = JitterBuffer::LowLatency;
    bool presentAhead = false;
    double guardMs = 2;
    unsigned seed = 1;

    // PyroWave: with decodeMs above 0, each frame's last packet comes transitMs after its host
    // timestamp and its first spreadMs before that, and it's decoded decodeMs (plus up to about
    // twice decodeJitterMs) after its last, or after the frame before it if that's later: one
    // decode at a time, so a decoder near its limit has frames wait behind each other
    double decodeMs = 0;
    double decodeJitterMs = 0;
    double spreadMs = 1.5;
    // Frames still arriving at their deadline are cut short then, with at least cutMinPercent
    // of them in, cutMarginMs before the pacer's cut-off (as VulkanRenderer does)
    bool cutting = false;
    int cutMinPercent = 30;
    double cutMarginMs = 2;
    // This share of frames have their last packet come stragglerMs late, after the first
    // stragglerFromS; with stragglerWhole, their first packet too
    double stragglerShare = 0;
    double stragglerMs = 0;
    bool stragglerWhole = false;
    double stragglerFromS = 0;
};

struct Result {
    // Counted after the first 2 s, as the pacer settles
    int shown = 0;
    int uneven = 0;  // Held a different number of vsyncs than the host's own step
    int queueDrops = 0;
    uint64_t late = 0;
    uint64_t pulls = 0;
    double waitP50Ms = 0;
    double maxWaitMs = 0;
    double waitP50BeforeSwitchMs = 0;
    double waitP50AfterSwitchMs = 0;
    double minWaitAfterSwitchMs = 0;
    double maxWaitAfterSwitchMs = 0;
    int64_t slotAtEnd = 0;
    // late and pulls over a window of the stream (see countFrom/countTo)
    uint64_t lateIn = 0;
    uint64_t pullsIn = 0;
    int cuts = 0;  // Frames cut short
};

struct Frame {
    int64_t ptsNs;
    int64_t arrivalNs;
    int64_t firstPacketNs = 0;
    int64_t lastPacketNs = 0;
    int64_t decodeNs = 0;
};

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

Result run(const Scenario& s, double countFromS = 2, double countToS = 1e9) {
    const int64_t periodNs = static_cast<int64_t>(std::llround(1e9 / s.refreshHz));
    const int64_t startNs = 1'000'000'000;
    const int64_t endNs = startNs + static_cast<int64_t>(s.seconds * 1e9);
    std::mt19937 rng(s.seed);
    std::normal_distribution<double> jitter(0, s.jitterMs * 1e6);
    std::uniform_real_distribution<double> uniform(0, 1);
    std::normal_distribution<double> unit(0, 1);

    // The host's frames and when each finishes decoding here
    std::vector<Frame> frames;
    double hostNs = startNs;
    double nextGapNs = s.hostGapEveryS > 0 ? startNs + s.hostGapEveryS * 1e9 : 1e30;
    int64_t lastArrival = 0;
    size_t spikeIndex = 0;
    while (hostNs < endNs) {
        const double t = (hostNs - startNs) / 1e9;
        const double interval = s.hostInterval(t) * 1e9;
        if (hostNs >= nextGapNs) {
            nextGapNs += s.hostGapEveryS * 1e9;
            hostNs += interval;  // Skipped by the host
            continue;
        }
        double transit = s.transitMs * 1e6 + jitter(rng);
        if (spikeIndex < s.spikes.size() && t >= s.spikes[spikeIndex].first) {
            transit += s.spikes[spikeIndex].second * 1e6;
            spikeIndex++;
        }
        int64_t arrival = static_cast<int64_t>(hostNs + std::max(transit, 1e6));
        int64_t firstPacket = 0, lastPacket = 0, decode = 0;
        if (s.decodeMs > 0) {
            decode = static_cast<int64_t>((s.decodeMs + std::fabs(unit(rng)) * s.decodeJitterMs) * 1e6);
            lastPacket = arrival;
            firstPacket = lastPacket - static_cast<int64_t>(s.spreadMs * 1e6);
            // Evenly spread, so a run of luck doesn't leave seconds without one
            if (t >= s.stragglerFromS && s.stragglerShare > 0 &&
                    frames.size() % static_cast<size_t>(std::lround(1.0 / s.stragglerShare)) == 0) {
                lastPacket += static_cast<int64_t>(s.stragglerMs * 1e6);
                if (s.stragglerWhole) firstPacket += static_cast<int64_t>(s.stragglerMs * 1e6);
            }
            arrival = lastPacket + decode;  // For the order only: decoded as delivered (decoded())
        }
        if (s.stallAtS >= 0 && t >= s.stallAtS && t < s.stallAtS + s.stallMs / 1e3) {
            arrival = std::max(arrival, startNs + static_cast<int64_t>((s.stallAtS + s.stallMs / 1e3) * 1e9 + s.transitMs * 1e6));
        }
        // The decoder delivers in order
        arrival = std::max(arrival, lastArrival + 100'000);
        lastArrival = arrival;
        frames.push_back({static_cast<int64_t>(hostNs), arrival, firstPacket, lastPacket, decode});
        hostNs += interval;
    }

    // Told the nominal rate (144 Hz for 143.655), which the pacer then measures exactly
    const double nominalHz = static_cast<double>(std::lround(s.refreshHz));
    FramePacer pacer(PacingMode::HostTimed, static_cast<int>(nominalHz), static_cast<int64_t>(std::llround(1e9 / nominalHz)),
                     s.jitterBuffer);
    pacer.setPresentAhead(s.presentAhead);
    pacer.setPresentGuardNs(static_cast<int64_t>(s.guardMs * 1e6));
    const int64_t guardNs = pacer.presentGuardNs();

    // The cut-off, as VulkanRenderer::updatePartialDeadline() works it out from the frame that
    // just arrived: frames still arriving then are cut short and decoded from there
    bool haveDeadline = false;
    int64_t deadlineOffsetNs = 0;
    int64_t lastDecodedNs = 0;
    const int64_t cutMarginNs = static_cast<int64_t>(s.cutMarginMs * 1e6);
    // Its decode, as things stand: cut short at its deadline if it would still be arriving then,
    // and after the frame before it is decoded. Returns whether it was cut.
    auto decoded = [&](size_t i) {
        Frame& f = frames[i];
        if (f.lastPacketNs == 0) return false;
        bool cut = false;
        if (s.cutting && haveDeadline) {
            const int64_t cutNs = f.ptsNs + deadlineOffsetNs - cutMarginNs;
            if (cutNs < f.lastPacketNs && cutNs > f.firstPacketNs &&
                    (cutNs - f.firstPacketNs) * 100 >= static_cast<int64_t>(s.cutMinPercent) * (f.lastPacketNs - f.firstPacketNs)) {
                f.lastPacketNs = cutNs;
                cut = true;
            }
        }
        f.arrivalNs = std::max(f.lastPacketNs, lastDecodedNs) + f.decodeNs;
        return cut;
    };

    struct Waiting {
        FrameTiming timing;
        size_t index;
    };
    std::vector<Waiting> queue;
    std::map<int64_t, size_t> display;  // Vsync to the frame shown at it; a later one replaces
    Result r;
    const int64_t countFromNs = startNs + static_cast<int64_t>(countFromS * 1e9);
    const int64_t countToNs = startNs + static_cast<int64_t>(std::min(countToS, s.seconds) * 1e9);
    uint64_t lateAtFrom = 0, pullsAtFrom = 0, lateAtTo = 0, pullsAtTo = 0;
    bool fromTaken = false, toTaken = false;
    bool switched = false;

    auto presentAhead = [&]() {
        while (s.presentAhead && !queue.empty()) {
            const int64_t vsync = pacer.plannedVsyncNs(queue.front().timing);
            if (vsync == 0) break;
            pacer.onPresentedAhead(vsync);
            display[vsync] = queue.front().index;
            queue.erase(queue.begin());
        }
    };

    size_t next = 0;
    const int64_t callbackLateNs = 400'000;
    for (int64_t vsync = startNs; vsync < endNs; vsync += periodNs) {
        const int64_t callbackNs = vsync + callbackLateNs;
        while (next < frames.size()) {
            // Cut short at its deadline, if it would still be arriving then
            Frame saved = frames[next];
            const bool cut = decoded(next);
            if (frames[next].arrivalNs > callbackNs) {
                frames[next] = saved;  // Looked at again at the next vsync, with that deadline
                break;
            }
            if (cut && frames[next].arrivalNs >= countFromNs) r.cuts++;
            const double t = (frames[next].arrivalNs - startNs) / 1e9;
            if (s.switchAtS >= 0 && !switched && t >= s.switchAtS) {
                pacer.setJitterBuffer(s.switchTo);
                switched = true;
            }
            FrameTiming timing {frames[next].ptsNs, frames[next].arrivalNs, 0};
            pacer.onFrameArrived(timing);
            if (s.decodeMs > 0) {
                pacer.addReadyCost(timing.arrivalNs - std::max(frames[next].lastPacketNs, lastDecodedNs));
                int64_t offsetNs = 0;
                haveDeadline = pacer.partialDeadlineOffsetNs(timing, &offsetNs);
                deadlineOffsetNs = offsetNs;
            }
            lastDecodedNs = timing.arrivalNs;
            queue.push_back({timing, next});
            while (queue.size() > pacer.maxQueued()) {
                queue.erase(queue.begin());
                if (frames[next].arrivalNs >= countFromNs) r.queueDrops++;
            }
            presentAhead();
            next++;
        }
        if (!fromTaken && vsync >= countFromNs) {
            lateAtFrom = pacer.lateFrames();
            pullsAtFrom = pacer.pulls();
            fromTaken = true;
        }
        if (!toTaken && vsync >= countToNs) {
            lateAtTo = pacer.lateFrames();
            pullsAtTo = pacer.pulls();
            toTaken = true;
        }

        std::vector<FrameTiming> timings;
        for (const Waiting& w : queue) timings.push_back(w.timing);
        const int choice = pacer.onVsync(vsync, timings.data(), timings.size());
        if (choice >= 0) {
            display[vsync] = queue[static_cast<size_t>(choice)].index;
            queue.erase(queue.begin(), queue.begin() + choice + 1);
        }
        presentAhead();
    }
    if (!toTaken) {
        lateAtTo = pacer.lateFrames();
        pullsAtTo = pacer.pulls();
    }

    // What the display showed
    std::vector<double> waits, before, after;
    const int64_t switchNs = startNs + static_cast<int64_t>(s.switchAtS * 1e9);
    int64_t lastVsync = 0;
    size_t lastFrame = 0;
    for (const auto& [vsync, index] : display) {
        const Frame& f = frames[index];
        if (vsync >= countFromNs) {
            const double wait = (vsync - (f.arrivalNs + guardNs)) / 1e6;
            waits.push_back(wait);
            r.maxWaitMs = std::max(r.maxWaitMs, wait);
            if (s.switchAtS >= 0) {
                // Leaving the second after the switch out, as the schedule moves
                if (vsync < switchNs) before.push_back(wait);
                else if (vsync > switchNs + 1'000'000'000) after.push_back(wait);
            }
            if (lastVsync != 0) {
                const int64_t held = (vsync - lastVsync + periodNs / 2) / periodNs;
                const int64_t hostStep = std::max<int64_t>(1, std::llround(static_cast<double>(f.ptsNs - frames[lastFrame].ptsNs) / periodNs));
                if (held != hostStep) r.uneven++;
            }
            r.shown++;
        }
        lastVsync = vsync;
        lastFrame = index;
    }
    r.late = pacer.lateFrames() - lateAtFrom;
    r.pulls = pacer.pulls() - pullsAtFrom;
    r.lateIn = lateAtTo - lateAtFrom;
    r.pullsIn = pullsAtTo - pullsAtFrom;
    r.waitP50Ms = median(waits);
    r.waitP50BeforeSwitchMs = median(before);
    r.waitP50AfterSwitchMs = median(after);
    if (!after.empty()) {
        r.minWaitAfterSwitchMs = *std::min_element(after.begin(), after.end());
        r.maxWaitAfterSwitchMs = *std::max_element(after.begin(), after.end());
    }
    r.slotAtEnd = pacer.slotVsyncs();
    return r;
}

int failures = 0;

void check(bool ok, const std::string& what) {
    printf("    %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

void report(const Scenario& s, const Result& r) {
    printf("  %s%s: %d shown, %d uneven, %llu late, %llu pulled, %d queue drops, %d cut, wait p50 %.1f max %.1f ms\n",
           s.name.c_str(), s.presentAhead ? " (present ahead)" : "", r.shown, r.uneven,
           static_cast<unsigned long long>(r.late), static_cast<unsigned long long>(r.pulls), r.queueDrops, r.cuts,
           r.waitP50Ms, r.maxWaitMs);
}

std::string fmt(const char* format, double a, double b = 0) {
    char text[160];
    snprintf(text, sizeof(text), format, a, b);
    return text;
}

}  // namespace

int main() {
    const double refresh = 143.655;
    const double period = 1.0 / refresh;
    const double periodMs = period * 1e3;

    for (bool ahead : {false, true}) {
        printf("\n=== %s ===\n", ahead ? "Presenting ahead" : "Shown at the vsync callback");

        {
            Scenario s;
            s.name = "Frame rate matched to the display";
            s.presentAhead = ahead;
            s.hostInterval = [&](double) { return period; };
            const Result r = run(s);
            report(s, r);
            check(r.uneven <= 2, "frames keep the host's cadence");
            check(r.late + r.pulls <= 2, "the schedule settles");
            check(r.maxWaitMs < 2 * periodMs, "no frame waits 2 vsyncs or more");
            check(r.queueDrops == 0, "no queue drops");
        }
        {
            Scenario s;
            s.name = "Half the display's rate";
            s.presentAhead = ahead;
            s.hostInterval = [&](double) { return 2 * period; };
            const Result r = run(s);
            report(s, r);
            check(r.slotAtEnd == 2, "two vsyncs a frame");
            check(r.uneven <= 2, "frames held two vsyncs each");
            check(r.late + r.pulls <= 2, "the schedule settles");
        }
        {
            // 0.1% slow over 60 s is 8.6 vsyncs of drift: about that many late frames, one each
            Scenario s;
            s.name = "Host 0.1% slower than the display";
            s.presentAhead = ahead;
            s.seconds = 62;
            s.hostInterval = [&](double) { return period * 1.001; };
            const Result r = run(s);
            report(s, r);
            check(r.late >= 6 && r.late <= 12, "about one late frame per vsync of drift");
            check(r.pulls <= 2, "no pulls fighting the drift");
            check(r.uneven <= static_cast<int>(r.late + r.pulls) + 2, "one uneven frame per late frame");
        }
        {
            Scenario s;
            s.name = "Host 0.1% faster than the display";
            s.presentAhead = ahead;
            s.seconds = 62;
            s.hostInterval = [&](double) { return period / 1.001; };
            const Result r = run(s);
            report(s, r);
            check(r.pulls >= 6 && r.pulls <= 12, "about one pull per vsync of drift");
            check(r.late <= 2, "no late frames fighting the drift");
            check(r.uneven <= static_cast<int>(r.late + r.pulls) + 2, "one uneven frame per pull");
            check(r.maxWaitMs < 2 * periodMs, "no frame waits 2 vsyncs or more");
        }
        {
            // One frame a vsync late: the schedule moves later for it, then comes back once the
            // hold has passed
            Scenario s;
            s.name = "A lone frame 10 ms late";
            s.presentAhead = ahead;
            s.spikes = {{10.0, 10.0}};
            s.hostInterval = [&](double) { return period; };
            const Result r = run(s);
            report(s, r);
            check(r.late >= 1 && r.late <= 2, "the late frame moves the schedule");
            check(r.pulls >= 1 && r.pulls <= 2, "the delay is given back");
            check(r.uneven <= 4, "a couple of uneven frames, no more");
        }
        {
            // Spikes of 3 ms every 2 s: Balanced's headroom absorbs them
            Scenario low;
            low.name = "3 ms spikes, Low latency";
            low.presentAhead = ahead;
            low.hostInterval = [&](double) { return period; };
            for (double t = 3; t < 30; t += 2) low.spikes.push_back({t, 3.0});
            Scenario balanced = low;
            balanced.name = "3 ms spikes, Balanced";
            balanced.jitterBuffer = JitterBuffer::Balanced;
            const Result rl = run(low);
            const Result rb = run(balanced);
            report(low, rl);
            report(balanced, rb);
            // Not that Balanced is always the more even: a spike that only eats into its
            // headroom still moves its schedule later, where Low latency's frames may happen to
            // sit far enough from the vsync to take it
            check(rb.uneven <= 2, "Balanced shows them evenly");
            check(rb.maxWaitMs < 3 * periodMs + 4, "Balanced's frames wait under 3 vsyncs plus its headroom");
            check(rb.queueDrops == 0, "Balanced's queue holds every frame it schedules");
        }
        for (JitterBuffer jb : {JitterBuffer::LowLatency, JitterBuffer::Balanced}) {
            // Frames bunched up after a stall come back under the cap at once
            Scenario s;
            s.name = jb == JitterBuffer::Balanced ? "100 ms stall, Balanced" : "100 ms stall, Low latency";
            s.presentAhead = ahead;
            s.jitterBuffer = jb;
            s.stallAtS = 10;
            s.stallMs = 100;
            s.hostInterval = [&](double) { return period; };
            const Result r = run(s);
            report(s, r);
            const double cap = jb == JitterBuffer::Balanced ? 3 * periodMs + 4 : 2 * periodMs;
            check(r.maxWaitMs < cap, fmt("no frame waits past the cap (%.1f ms)", cap));
            check(r.uneven <= 20, "back to even after the stall");
        }
        {
            Scenario s;
            s.name = "Host skips a frame every second";
            s.presentAhead = ahead;
            s.hostGapEveryS = 1.0;
            s.hostInterval = [&](double) { return period; };
            const Result r = run(s);
            report(s, r);
            check(r.uneven <= 2, "the host's gaps are reproduced, not added to");
            check(r.late + r.pulls <= 2, "the gaps don't move the schedule");
        }
        {
            // A game going from 72 fps to 100: no whole number of vsyncs a frame, so the slot
            // goes back to one. Kept at two, the schedule was pulled back hundreds of times.
            Scenario s;
            s.name = "72 fps, then 100 fps";
            s.presentAhead = ahead;
            s.seconds = 22;
            s.hostInterval = [&](double t) { return t < 10 ? 2 * period : 0.01; };
            const Result r = run(s, 12, 22);
            report(s, r);
            check(r.slotAtEnd == 1, "back to one vsync a frame");
            check(r.pullsIn < 100, fmt("the schedule isn't pulled back over and over (%.0f pulls in 10 s)", r.pullsIn));
        }
        {
            Scenario s;
            s.name = "Low latency, then Balanced at 15 s";
            s.presentAhead = ahead;
            s.switchAtS = 15;
            s.switchTo = JitterBuffer::Balanced;
            s.hostInterval = [&](double) { return period; };
            const Result r = run(s);
            report(s, r);
            // The headroom is the least room a frame must have, so frames that already had that
            // much stay where they were
            check(r.minWaitAfterSwitchMs >= 4,
                  fmt("every frame then has Balanced's headroom (wait p50 %.1f to %.1f ms)", r.waitP50BeforeSwitchMs,
                      r.waitP50AfterSwitchMs));
            check(r.queueDrops == 0, "no queue drops through the switch");
            check(r.uneven <= 3, "a vsync or so of unevenness while the schedule moves");
        }
        {
            Scenario s;
            s.name = "Balanced, then Low latency at 15 s";
            s.presentAhead = ahead;
            s.jitterBuffer = JitterBuffer::Balanced;
            s.switchAtS = 15;
            s.switchTo = JitterBuffer::LowLatency;
            s.hostInterval = [&](double) { return period; };
            const Result r = run(s);
            report(s, r);
            // Whether frames then move a vsync sooner depends on where they sit against the vsyncs
            check(r.maxWaitAfterSwitchMs < 2 * periodMs,
                  fmt("no frame then waits past Low latency's cap (wait p50 %.1f to %.1f ms)", r.waitP50BeforeSwitchMs,
                      r.waitP50AfterSwitchMs));
            check(r.waitP50AfterSwitchMs <= r.waitP50BeforeSwitchMs + 0.5, "the wait doesn't grow");
            check(r.maxWaitMs < 3 * periodMs + 4, "no frame waits past Balanced's cap");
        }
    }

    // PyroWave frames cut short at their deadline (VulkanRenderer::updatePartialDeadline()).
    // 1.5% of frames have their last packets come 5 ms late, too late for their vsync: cut
    // short, they're shown on time, where shown whole they move frames a vsync later, and the
    // hold never sees 3 s without one to move them back.
    for (bool ahead : {false, true}) {
        printf("\n=== PyroWave cutting, %s ===\n", ahead ? "presenting ahead" : "shown at the vsync callback");
        const double period120 = 1.0 / 119.712;
        const double periodMs120 = period120 * 1e3;
        Scenario base;
        base.refreshHz = 119.712;
        base.hostInterval = [=](double) { return period120; };
        base.presentAhead = ahead;
        base.transitMs = 0.5;
        base.jitterMs = 0.3;
        base.decodeMs = 2.5;
        base.spreadMs = 1.5;
        base.stragglerShare = 0.015;
        base.stragglerMs = 5;
        base.stragglerFromS = 4;

        Scenario whole = base;
        whole.name = "Late packets, shown whole";
        Scenario cut = base;
        cut.name = "Late packets, cut short";
        cut.cutting = true;
        Scenario smoother = cut;
        smoother.name = "Late packets, cut short with 20% in";
        smoother.cutMinPercent = 20;
        const Result rw = run(whole, 6);
        const Result rc = run(cut, 6);
        const Result rs = run(smoother, 6);
        report(whole, rw);
        report(cut, rc);
        report(smoother, rs);
        check(rc.late <= 1 && rc.cuts > 0, fmt("cut short, they're on time (%.0f late, %.0f cut)", static_cast<double>(rc.late), rc.cuts));
        check(rc.waitP50Ms < rw.waitP50Ms - periodMs120 * 0.6,
              fmt("and frames stay a vsync sooner than shown whole (wait p50 %.1f against %.1f ms)", rc.waitP50Ms, rw.waitP50Ms));
        check(rc.uneven <= 2, fmt("evenly (%.0f uneven)", rc.uneven));
        check(rs.cuts >= rc.cuts, fmt("a lower minimum cuts no fewer (%.0f against %.0f)", rs.cuts, rc.cuts));

        // A decoder near its limit (a Pixel 10 Pro at 120 fps, about 8 ms a decode): frames
        // wait behind each other, which cutting can't help. Cutting mustn't make it worse.
        Scenario busyWhole = base;
        busyWhole.name = "Busy decoder, shown whole";
        busyWhole.transitMs = 2.5;
        busyWhole.decodeMs = 7.8;
        busyWhole.decodeJitterMs = 0.6;
        busyWhole.stragglerShare = 0;
        busyWhole.spikes = {{3.0, 12.0}};
        busyWhole.seconds = 60;
        Scenario busyCut = busyWhole;
        busyCut.name = "Busy decoder, cut short";
        busyCut.cutting = true;
        const Result rbw = run(busyWhole, 8);
        const Result rbc = run(busyCut, 8);
        report(busyWhole, rbw);
        report(busyCut, rbc);
        check(rbc.late + rbc.pulls <= rbw.late + rbw.pulls + 2,
              fmt("with a busy decoder, no back and forth (%.0f late and pulled, against %.0f)",
                  static_cast<double>(rbc.late + rbc.pulls), static_cast<double>(rbw.late + rbw.pulls)));
        check(rbc.uneven <= rbw.uneven + 2, fmt("and no less even (%.0f uneven, against %.0f)", rbc.uneven, rbw.uneven));
    }

    printf("\n%s: %d check%s failed\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
