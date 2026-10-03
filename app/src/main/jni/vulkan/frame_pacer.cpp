#include "frame_pacer.h"

#include <algorithm>
#include <cmath>

namespace vkr {

namespace {

    // A timestamp this far outside the running timeline means the stream restarted
    constexpr int64_t kMaxPtsBackstepNs = 100'000'000;
    constexpr int64_t kMaxPtsGapNs = 5'000'000'000;

    // Phase locking (see updatePhase)
    constexpr int64_t kMinPhaseSpanNs = 2'000'000'000;
    constexpr double kMaxDriftPerFrame = 0.02;
    constexpr double kLockArc = 0.7;         // Spread of frames around the fitted line, in slots
    constexpr double kUnlockArc = 1.0;       // Past a full slot, frames would collide
    constexpr double kPhaseMargin = 0.1;     // Clearance from the vsyncs where frames change
    constexpr double kScheduleWidth = 0.04;  // Allowance for the fit moving
    constexpr double kLateDecayPerFrame = 0.0005;
    constexpr int64_t kPlacementSettleNs = 5'000'000'000;
    // While frames are scheduled on the fitted line with the buffer measured against it (Balanced
    // and Smooth, see updatePhase()), the buffer gives delay back over about 17 s (1024 frames at
    // 60 fps): stretches of late host timestamps come back, and a buffer that forgot them between
    // stretches left the frames at the start of each one late
    constexpr int64_t kLineDecayDivisor = 1024;
    constexpr size_t kSlotDeltaWindow = 63;

    // Vsyncs the period is measured over
    constexpr size_t kVsyncWindow = 480;
    constexpr size_t kMinVsyncsForPeriod = 60;

    double wrapCycles(double x) {
        x -= std::floor(x);
        return x >= 1.0 ? 0.0 : x;
    }
}

// Presets chosen by replaying recorded sessions over Wi-Fi at 72 and 144 fps
// (tools/pacer-trace.sh). Compared with covering 95% of transit times, Balanced roughly halved
// the frames that arrived too late for their vsync for 3-4 ms more delay, and Smooth left about
// a sixth of them for 10-12 ms more. LowLatency covers 98%: at a frame a vsync that roughly
// halved the uneven frames for 0.5-2 ms more delay.
//
// LowestLatency covers 90%, for when delay matters more than evenness. Replayed through
// PyroWave sessions on a Pixel 10 Pro (whose GPU decode time varies by several ms), it scheduled
// frames 10 ms sooner than LowLatency at 60 fps on 120 Hz (11 ms against 21) and 4.6 ms sooner at
// 120 fps, for about 1.5-2.5% of frames held a vsync long or short. Covering 95% saved about a
// quarter less; 80% doubled the uneven frames again for 3 ms more.
//
// Balanced looks at the last 3 s rather than 10, and gives delay back over about a second
// (64 frames) rather than eight: with 10 s, a burst of late frames held the buffer up for
// most of a minute. Replayed through four 60 fps Pixel 10 Pro sessions at 60 and 120 Hz, as
// many frames arrived after their schedule (0.8-1.0%), the median buffer was 1-2 ms lower, and
// the 90th percentile 7-20 ms lower where bursts had held it up. Covering 98% instead saved
// 2-3 ms more but doubled the late frames.
//
// With a frame every two vsyncs or more, LowLatency covers 95% instead: over 13 steady
// 72 fps on 144 Hz sessions, that took 1.3 ms off the delay for 0.3 more uneven frames a minute
// (36 a minute). At a frame a vsync it cost 40% more uneven frames, so 98% stays there.
constexpr double kLowLatencySlowSlotCoverage = 0.95;

HostTimeline::HostTimeline(JitterBuffer jitterBuffer) : jitterBuffer_(jitterBuffer) {
    switch (jitterBuffer) {
        case JitterBuffer::LowLatency:
            windowNs_ = 2'000'000'000;
            coverage_ = 0.98;
            decayDivisor_ = 32;
            break;
        case JitterBuffer::LowestLatency:
            windowNs_ = 2'000'000'000;
            coverage_ = 0.90;
            decayDivisor_ = 32;
            break;
        case JitterBuffer::Smooth:
            windowNs_ = 30'000'000'000;
            coverage_ = 0.999;
            decayDivisor_ = 4096;
            break;
        case JitterBuffer::Balanced:
        default:
            windowNs_ = 3'000'000'000;
            coverage_ = 0.99;
            decayDivisor_ = 64;
            break;
    }
    baseCoverage_ = coverage_;
    baseWindowNs_ = windowNs_;
    baseDecayDivisor_ = decayDivisor_;
}

void HostTimeline::setSlotVsyncs(int64_t slotVsyncs) {
    if (jitterBuffer_ == JitterBuffer::LowLatency) {
        coverage_ = slotVsyncs >= 2 ? std::min(baseCoverage_, kLowLatencySlowSlotCoverage) : baseCoverage_;
    }
}

void HostTimeline::reset() {
    window_.clear();
    minQueue_.clear();
    offsetNs_ = 0;
    lastPtsNs_ = 0;
    started_ = false;
}

void HostTimeline::restartWindow(int64_t minWindowNs, int64_t minDecayDivisor) {
    window_.clear();
    minQueue_.clear();
    windowNs_ = std::max(baseWindowNs_, minWindowNs);
    decayDivisor_ = std::max(baseDecayDivisor_, minDecayDivisor);
}

bool HostTimeline::checkContinuity(int64_t hostPtsNs) {
    bool continuous = true;
    if (started_ && (hostPtsNs < lastPtsNs_ - kMaxPtsBackstepNs || hostPtsNs > lastPtsNs_ + kMaxPtsGapNs)) {
        reset();
        continuous = false;
    }
    lastPtsNs_ = hostPtsNs;
    return continuous;
}

int64_t HostTimeline::minTransitNs() const {
    return minQueue_.empty() ? 0 : minQueue_.front().transitNs;
}

void HostTimeline::addSample(int64_t scheduledPtsNs, int64_t arrivalNs) {
    if (!started_) {
        started_ = true;
        startNs_ = arrivalNs;
        warmedUp_ = false;
    }
    const bool warmingUp = arrivalNs - startNs_ < kWarmupNs;
    const Sample sample {arrivalNs, arrivalNs - scheduledPtsNs};

    // The warm-up's frames (the first keyframe is large, and the decoder is starting) say little
    // about the stream once it settles. Kept in the window, they held the buffer near its cap
    // for seconds: 50 ms at the start of a 60 fps stream, taking five seconds to come down.
    if (!warmingUp && !warmedUp_) {
        window_.clear();
        minQueue_.clear();
    }

    window_.push_back(sample);
    while (!minQueue_.empty() && minQueue_.back().transitNs >= sample.transitNs) {
        minQueue_.pop_back();
    }
    minQueue_.push_back(sample);

    while (window_.front().arrivalNs < arrivalNs - windowNs_) {
        const Sample& old = window_.front();
        if (minQueue_.front().arrivalNs == old.arrivalNs && minQueue_.front().transitNs == old.transitNs) {
            minQueue_.pop_front();
        }
        window_.pop_front();
    }

    // Through the warm-up, no buffer: frames are scheduled by the quickest transit seen, and any
    // that arrive later are shown as they arrive. The buffer then starts from the settled stream,
    // growing at once if frames arrive later and coming down slowly.
    if (warmingUp) {
        offsetNs_ = minTransitNs();
        return;
    }
    warmedUp_ = true;

    // Until there are enough samples for a percentile, cover all of them. After that, leave out
    // at least the two slowest: with few samples, the percentile is otherwise just the slowest
    // frame, which may be a one-off.
    scratch_.clear();
    for (const Sample& s : window_) {
        scratch_.push_back(s.transitNs);
    }
    const double n = static_cast<double>(scratch_.size());
    size_t index = scratch_.size() - 1;
    if (scratch_.size() >= 8) {
        const double coverage = std::min(coverage_, 1.0 - 2.0 / n);
        index = static_cast<size_t>(std::ceil(coverage * (n - 1)));
    }
    std::nth_element(scratch_.begin(), scratch_.begin() + index, scratch_.end());
    const int64_t target = scratch_[index];

    // Take more delay right away when frames start arriving later, and give it back slowly
    // so one quiet stretch doesn't leave us exposed to the next burst of jitter
    if (target > offsetNs_) {
        offsetNs_ = target;
    }
    else {
        offsetNs_ -= (offsetNs_ - target) / decayDivisor_;
    }

    if (maxBufferNs_ > 0) {
        offsetNs_ = std::min(offsetNs_, minTransitNs() + maxBufferNs_);
    }
}

FramePacer::FramePacer(PacingMode mode, int streamFps, int64_t vsyncPeriodNs, JitterBuffer jitterBuffer)
    : mode_(mode),
      streamIntervalNs_(1'000'000'000LL / std::max(streamFps, 1)),
      periodNs_(vsyncPeriodNs > 0 ? vsyncPeriodNs : 16'666'667),
      timeline_(jitterBuffer) {
    // Frames waiting out the buffer sit in the caller's queue (maxQueued()), which also holds
    // the frame due next and one being shown
    timeline_.setMaxBufferNs(static_cast<int64_t>(kMaxQueuedFrames - 3) * streamIntervalNs_);
}

void FramePacer::setVsyncPeriod(int64_t periodNs) {
    // Small differences are the caller's estimate being less exact than our own measurement.
    // A large one is a new refresh rate.
    if (periodNs > 0 && std::llabs(periodNs - periodNs_) * 50 > periodNs_) {
        periodNs_ = periodNs;
        vsyncs_.clear();
        resetPhase();
    }
}

void FramePacer::trackVsync(int64_t vsyncNs) {
    if (lastVsyncNs_ != 0) {
        // Count the vsyncs since the last callback, including any whose callbacks we missed
        vsyncIndex_ += std::max<int64_t>(1, (vsyncNs - lastVsyncNs_ + periodNs_ / 2) / periodNs_);
    }
    lastVsyncNs_ = vsyncNs;

    vsyncs_.push_back({vsyncIndex_, vsyncNs});
    if (vsyncs_.size() > kVsyncWindow) {
        vsyncs_.pop_front();
    }
    if (vsyncs_.size() < kMinVsyncsForPeriod) {
        return;
    }

    // Least squares fit of vsync time against vsync index
    const VsyncSample& first = vsyncs_.front();
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const VsyncSample& v : vsyncs_) {
        const double x = static_cast<double>(v.index - first.index);
        const double y = static_cast<double>(v.timeNs - first.timeNs);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double n = static_cast<double>(vsyncs_.size());
    const double denominator = n * sxx - sx * sx;
    if (denominator <= 0) {
        return;
    }
    const int64_t measured = static_cast<int64_t>(std::llround((n * sxy - sx * sy) / denominator));
    if (std::llabs(measured - periodNs_) * 10 < periodNs_) {
        periodNs_ = measured;
    }
}

size_t FramePacer::maxQueued() const {
    switch (mode_) {
        case PacingMode::MinLatency:
            return 1;
        case PacingMode::Balanced:
            return 2;
        case PacingMode::HostTimed: {
            // Enough to hold every frame inside the buffer we schedule with, plus one slot
            const int64_t held = timeline_.bufferNs() + scheduleDelayNs() + slotPeriodNs();
            const size_t frames = static_cast<size_t>(held / std::max<int64_t>(streamIntervalNs_, 1)) + 2;
            return std::min<size_t>(std::max<size_t>(frames, 2), kMaxQueuedFrames);
        }
        case PacingMode::CapFps:
        case PacingMode::Smoothness:
        default:
            return 4;
    }
}

void FramePacer::resetPhase() {
    phaseSamples_.clear();
    frameIndex_ = 0;
    shiftNs_ = 0;
    slotParity_ = 0;
    phaseLocked_ = false;
    driftPerFrame_ = 0;
    lateNs_ = 0;
}

void FramePacer::onFrameArrived(FrameTiming& frame) {
    if (mode_ != PacingMode::HostTimed) {
        frame.targetNs = frame.arrivalNs;
        return;
    }

    if (!timeline_.checkContinuity(frame.hostPtsNs)) {
        ptsDeltas_.clear();
        lastPtsNs_ = 0;
        resetPhase();
    }

    updateSlot(frame);
    updatePhase(frame);
}

// Adds the frame to the jitter buffer, measured against its host timestamp or, with onLine, its
// place on the schedule carried along the fitted line (measuredPtsNs), and schedules it at
// scheduledPtsNs plus the buffer. Measured against the line itself while scheduled after it, the
// time between them was counted twice.
// Changing what the buffer measures against starts its window over, since the two don't mix.
void FramePacer::scheduleFrom(FrameTiming& frame, int64_t measuredPtsNs, bool onLine, int64_t scheduledPtsNs) {
    if (onLine != transitOnLine_) {
        transitOnLine_ = onLine;
        timeline_.restartWindow(0, onLine ? kLineDecayDivisor : 0);
    }
    timeline_.addSample(measuredPtsNs, frame.arrivalNs);
    frame.targetNs = scheduledPtsNs + timeline_.offsetNs();
}

// How many vsyncs each frame should stay on screen, from the spacing of host timestamps.
// This is the game's frame rate, which can be below the stream's: a game capped at 71.82 fps
// is two vsyncs a frame on a 143.64 Hz panel, whatever the stream is set to.
void FramePacer::updateSlot(const FrameTiming& frame) {
    if (lastPtsNs_ != 0) {
        const int64_t delta = frame.hostPtsNs - lastPtsNs_;
        if (delta > 0 && delta < 1'000'000'000) {
            ptsDeltas_.push_back(delta);
            if (ptsDeltas_.size() > kSlotDeltaWindow) {
                ptsDeltas_.pop_front();
            }
        }
    }
    lastPtsNs_ = frame.hostPtsNs;

    int64_t slot = 1;
    if (ptsDeltas_.size() >= 8) {
        // The mean spacing, since a fixed refresh host display shows frames at uneven
        // spacings around it (a 71.82 fps game on a 165 Hz display alternates two and three
        // refreshes). Gaps from a host hitch are left out.
        std::vector<int64_t> sorted(ptsDeltas_.begin(), ptsDeltas_.end());
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const int64_t median = sorted[sorted.size() / 2];
        int64_t sum = 0;
        int64_t n = 0;
        for (int64_t delta : ptsDeltas_) {
            if (delta <= median * 2) {
                sum += delta;
                n++;
            }
        }
        const int64_t mean = sum / std::max<int64_t>(n, 1);

        // Only a whole number of vsyncs makes a slot. A frame rate between two of them has no
        // steady place on the vsync grid.
        const int64_t vsyncs = (mean + periodNs_ / 2) / periodNs_;
        if (vsyncs >= 1 && vsyncs <= 8 && std::llabs(mean - vsyncs * periodNs_) * 100 <= periodNs_ * 15) {
            slot = vsyncs;
        }
    }

    if (slot != slotVsyncs_) {
        slotVsyncs_ = slot;
        timeline_.setSlotVsyncs(slot);
        resetPhase();
    }
}

// Frames are due at the first vsync at or after their target time.
//
// Host timestamps vary even when the game's own pacing is perfect. With a fixed refresh host
// display, each frame's timestamp is the refresh at which the host showed it, which is up to a
// host refresh after the game presented it:
// - a game capped at 71.82 fps on a 144 Hz host is shown every second refresh, with an extra
//   refresh every 2.8 seconds, so its timestamps slide by a refresh and jump back
// - on a 165 Hz host they alternate between two and three refreshes
// - on a 143.64 Hz host, the cap and the display drift slowly against each other, and while
//   the game's presents sit near a refresh, frame time jitter lands them one refresh early or
//   late at random for seconds at a time
// Following those timestamps on our vsyncs reproduces the host display's judder.
//
// When the game runs at a steady rate, host timestamp against frame number is a straight line
// plus that jitter. We fit the line over several seconds and schedule each frame on it, with the
// jitter buffer measuring how late frames arrive against the line (scheduleFrom()). Frames then
// come out evenly, one per slot (the vsyncs each frame stays on screen), and a frame the host
// showed a refresh late is still in its own slot.
//
// The buffer once measured frames against their own timestamps, and the schedule sat on the line
// as late as the latest frame in the last 16 s: two extremes added, for frames that rarely are
// both stamped late and slow to arrive. At 60 fps on a 120 Hz Pixel 10 Pro with the Balanced
// buffer, that crept up to 30-40 ms; 99% of arrivals against the line came to 5-8 ms less. The host's frame rate is rarely exactly a whole fraction of our refresh
// rate, so the schedule drifts slowly against our vsyncs; we delay it, by less than a slot, to
// keep it clear of the vsyncs where frames change, and as the drift carries it up to one we move
// it past, which holds one frame a vsync longer or shorter: the least the rate difference
// requires.
//
// If the timestamps don't fit a line (the game's frame rate varies), frames are shown at their
// own host timestamps.
void FramePacer::updatePhase(FrameTiming& frame) {
    if (lastVsyncNs_ == 0) {
        scheduleFrom(frame, frame.hostPtsNs, false, frame.hostPtsNs);
        return;
    }

    const int64_t slotPeriod = slotPeriodNs();

    // At a frame a vsync, locking only evens out the host's own timing, which on a Pixel 10 Pro
    // at 120 fps cost about 3 ms more delay for 15-60% fewer uneven frames (measured when the
    // schedule covered the latest host timestamp on top of the buffer). Low latency and Lowest latency take the delay off. With a
    // frame every few vsyncs, both lock: unlocked, frames change on whichever vsync their own
    // timestamp lands before, and when that sits near a vsync, the slightest jitter shows one a
    // vsync early or late. At 60 fps on a 120 Hz Pixel 10 Pro, unlocked Lowest latency showed
    // about one frame in a hundred for one vsync or three instead of two.
    if (slotVsyncs_ == 1 && (timeline_.jitterBuffer() == JitterBuffer::LowLatency ||
                             timeline_.jitterBuffer() == JitterBuffer::LowestLatency)) {
        phaseSamples_.clear();
        phaseLocked_ = false;
        shiftNs_ = 0;
        slotParity_ = 0;
        lateNs_ = 0;
        scheduleFrom(frame, frame.hostPtsNs, false, frame.hostPtsNs);
        return;
    }

    if (!phaseSamples_.empty()) {
        // A gap of about two slots or more means the host didn't send a frame. Gaps of one
        // and a half slots are a frame shown a host refresh late, which comes paired with a
        // half-slot gap when the game's presents sit near a host refresh: counting it as two
        // frames would add a frame for every such pair and wreck the fit.
        const int64_t delta = frame.hostPtsNs - phaseSamples_.back().hostPtsNs;
        frameIndex_ += std::max<int64_t>(1, (delta + slotPeriod * 3 / 10) / slotPeriod);
    }
    phaseSamples_.push_back({frame.arrivalNs, frameIndex_, frame.hostPtsNs});
    while (phaseSamples_.front().timeNs < frame.arrivalNs - kPhaseWindowNs) {
        phaseSamples_.pop_front();
    }

    auto unlock = [&]() {
        // Frames go up at their own host timestamps, and a delay would only add latency
        phaseLocked_ = false;
        shiftNs_ = 0;
        slotParity_ = 0;
        lateNs_ = 0;
        scheduleFrom(frame, frame.hostPtsNs, false, frame.hostPtsNs);
    };

    if (frame.arrivalNs - phaseSamples_.front().timeNs < kMinPhaseSpanNs) {
        unlock();
        return;
    }

    // The game's frame interval, which with host and client clocks only parts per million
    // apart is the same on our clock. Two estimates, since host timestamps come in two shapes:
    // - a least squares fit, for timestamps scattered around the line (a 71.82 fps game on a
    //   165 Hz host, alternating two and three refreshes)
    // - the median spacing, for timestamps that march exactly with the host display and step
    //   by a refresh now and then (a 71.82 fps game on a 143.64 Hz host), which would tilt a
    //   least squares line whenever a step is in the window
    // Whichever leaves the frames packed tighter around its line wins.
    const PhaseSample& first = phaseSamples_.front();
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    std::vector<double> spacings;
    spacings.reserve(phaseSamples_.size());
    const PhaseSample* previous = nullptr;
    for (const PhaseSample& s : phaseSamples_) {
        const double x = static_cast<double>(s.frameIndex - first.frameIndex);
        const double y = static_cast<double>(s.hostPtsNs - first.hostPtsNs);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        if (previous) {
            spacings.push_back(static_cast<double>(s.hostPtsNs - previous->hostPtsNs) /
                               static_cast<double>(s.frameIndex - previous->frameIndex));
        }
        previous = &s;
    }
    const double n = static_cast<double>(phaseSamples_.size());
    const double denominator = n * sxx - sx * sx;
    if (denominator <= 0 || spacings.empty()) {
        unlock();
        return;
    }
    std::nth_element(spacings.begin(), spacings.begin() + spacings.size() / 2, spacings.end());
    const double candidates[] = {(n * sxy - sx * sy) / denominator, spacings[spacings.size() / 2]};

    double interval = 0;
    double latest = 0;
    double spread = 1e18;
    for (double candidate : candidates) {
        double high = -1e18;
        double low = 1e18;
        for (const PhaseSample& s : phaseSamples_) {
            const double residual = static_cast<double>(s.hostPtsNs - first.hostPtsNs) -
                                    candidate * static_cast<double>(s.frameIndex - first.frameIndex);
            high = std::max(high, residual);
            low = std::min(low, residual);
        }
        if (high - low < spread) {
            spread = high - low;
            interval = candidate;
            latest = high;
        }
    }

    driftPerFrame_ = (interval - static_cast<double>(slotPeriod)) / static_cast<double>(slotPeriod);
    if (std::fabs(driftPerFrame_) > kMaxDriftPerFrame ||
            spread / static_cast<double>(slotPeriod) > (phaseLocked_ ? kUnlockArc : kLockArc)) {
        unlock();
        // Start over from the last few seconds, so a burst of uneven frames stops counting
        // against the fit once it's over rather than after the whole window (seconds of steady
        // frames left unlocked on a Pixel 10 Pro)
        while (!phaseSamples_.empty() && phaseSamples_.front().timeNs < frame.arrivalNs - kRelockWindowNs) {
            phaseSamples_.pop_front();
        }
        return;
    }

    // Schedule this frame on the line. Balanced and Smooth measure the jitter buffer against the
    // line, which covers frames the host stamped late and frames slow to arrive in one percentile.
    // Low and Lowest latency, whose buffers cover less, schedule frames as late as the latest
    // recent one against the line and measure the buffer against frames' own timestamps: with
    // their buffer measured against the line, the frames at the start of each stretch of late
    // host timestamps came out late (up to 18 uneven frames in two minutes where 1 is needed,
    // and 30-50% more on a Pixel 10 Pro at 120 Hz).
    //
    // A frame landing later moves the schedule at once; once late frames stop, it comes back
    // slowly, since on a fixed refresh host they tend to return (the next time the game's
    // presents cross a host refresh). The schedule is carried from frame to frame so that
    // refitting the line doesn't move it.
    const bool againstLine = timeline_.jitterBuffer() == JitterBuffer::Balanced ||
                             timeline_.jitterBuffer() == JitterBuffer::Smooth;
    const double x = static_cast<double>(frameIndex_ - first.frameIndex);
    const double line = static_cast<double>(first.hostPtsNs) + interval * x;
    double scheduled = againstLine ? line : line + latest;
    if (phaseLocked_) {
        const double carried = scheduledPtsNs_ + interval * static_cast<double>(frameIndex_ - scheduledIndex_);
        if (scheduled < carried) {
            scheduled = std::max(scheduled, carried - static_cast<double>(slotPeriod) * kLateDecayPerFrame);
        }
    }
    else {
        lockedSinceNs_ = frame.arrivalNs;
    }
    scheduledPtsNs_ = scheduled;
    scheduledIndex_ = frameIndex_;
    lateNs_ = scheduled - line;
    phaseLocked_ = true;
    const int64_t scheduledNs = static_cast<int64_t>(std::llround(scheduled));
    if (againstLine) {
        scheduleFrom(frame, scheduledNs, true, scheduledNs);
    }
    else {
        scheduleFrom(frame, frame.hostPtsNs, false, scheduledNs);
    }

    // Where the schedule sits within the slot, as a narrow arc that allows for the fit
    // wobbling as frames enter and leave the window
    const int64_t slotStartNs = lastVsyncNs_ - (vsyncIndex_ % slotVsyncs_) * periodNs_;
    const double width = kScheduleWidth;
    const double start = wrapCycles(static_cast<double>(frame.targetNs - slotStartNs) / slotPeriod - width / 2);
    const double margin = kPhaseMargin;

    // With the slot starting `parity` vsyncs later, the delay that clears the arc from the
    // vsyncs where frames change, and how long the last frame in the arc then waits
    struct Placement {
        double shift;
        double delay;
    };
    auto place = [&](int64_t parity) {
        const double s = wrapCycles(start - static_cast<double>(parity) / slotVsyncs_);
        double shift;
        if (s >= margin && s + width <= 1.0 - margin) {
            shift = 0.0;
        }
        else if (s < margin) {
            shift = margin - s;
        }
        else {
            shift = 1.0 + margin - s;
        }
        const double end = s + shift + width;
        return Placement {shift, shift + (std::ceil(end) - end)};
    };

    // Is the arc, as currently placed, still clear of the vsyncs?
    const double current = wrapCycles(start - static_cast<double>(slotParity_) / slotVsyncs_ +
                                      static_cast<double>(shiftNs_) / slotPeriod);
    const bool clear = current >= margin / 2 && current + width <= 1.0 - margin / 2;
    const double currentEnd = current + width;
    const double currentDelay = static_cast<double>(shiftNs_) / slotPeriod + (std::ceil(currentEnd) - currentEnd);

    // Which vsync the newest frame would be shown at under a placement. A new placement may
    // move frames by at most one vsync, in the direction the host drifts: one frame held a
    // vsync shorter (host running fast) or longer (host running slow). Otherwise a change
    // meant to gain a vsync could skip a frame and then hold another one long.
    auto shownAt = [&](int64_t parity, double shift) {
        const double x = static_cast<double>(vsyncIndex_) +
                (static_cast<double>(frame.targetNs - lastVsyncNs_) + shift * slotPeriod) / periodNs_;
        int64_t n = static_cast<int64_t>(std::ceil(x - 1e-6));
        while (((n - parity) % slotVsyncs_ + slotVsyncs_) % slotVsyncs_ != 0) {
            n++;
        }
        return n;
    };
    const int64_t shownNow = shownAt(slotParity_, static_cast<double>(shiftNs_) / slotPeriod);
    const int64_t preferredStep = driftPerFrame_ < 0 ? -1 : 1;

    bool found = false;
    int64_t bestParity = slotParity_;
    Placement best {0.0, 0.0};
    int bestRank = 0;
    for (int64_t parity = 0; parity < slotVsyncs_; parity++) {
        const Placement p = place(parity);
        const int64_t step = shownAt(parity, p.shift) - shownNow;
        // No move at all is best, then one vsync with the drift, then one against it
        int rank;
        if (step == 0) {
            rank = 0;
        }
        else if (step == preferredStep) {
            rank = 1;
        }
        else if (std::llabs(step) == 1) {
            rank = 2;
        }
        else {
            rank = 3 + static_cast<int>(std::llabs(step));
        }
        if (!found || rank < bestRank || (rank == bestRank && p.delay < best.delay)) {
            found = true;
            best = p;
            bestParity = parity;
            bestRank = rank;
        }
    }

    // Move when the arc is about to reach a vsync, or when another placement would show
    // frames at least half a vsync sooner for no more than one frame held a vsync short
    // Only right after locking: later, each such move would be a visible hitch traded for
    // under a vsync of latency
    bool muchSooner = false;
    if (clear && frame.arrivalNs - lockedSinceNs_ < kPlacementSettleNs) {
        for (int64_t parity = 0; parity < slotVsyncs_; parity++) {
            const Placement p = place(parity);
            const int64_t step = shownAt(parity, p.shift) - shownNow;
            if (step == -1 && slotVsyncs_ > 1 && p.delay + 0.5 / slotVsyncs_ < currentDelay) {
                muchSooner = true;
                best = p;
                bestParity = parity;
                break;
            }
        }
    }
    if (!clear || muchSooner) {
        slotParity_ = bestParity;
        shiftNs_ = static_cast<int64_t>(best.shift * slotPeriod);
    }
}

int FramePacer::onVsync(int64_t vsyncNs, const FrameTiming* frames, size_t count) {
    trackVsync(vsyncNs);

    int choice = -1;
    switch (mode_) {
        case PacingMode::MinLatency:
            choice = count > 0 ? static_cast<int>(count) - 1 : -1;
            break;

        case PacingMode::Balanced:
            // Show frames in order, but no more often than the stream frame rate. This keeps
            // a stream below the refresh rate from alternating short and long frames.
            if (count > 0 && vsyncNs - lastPresentVsyncNs_ >= streamIntervalNs_ * 8 / 10) {
                choice = 0;
            }
            break;

        case PacingMode::CapFps:
        case PacingMode::Smoothness:
            choice = count > 0 ? 0 : -1;
            break;

        case PacingMode::HostTimed: {
            if (count == 0) {
                break;
            }

            // Frames presented ahead are only shown from here when they can't be. A frame that
            // waited because its vsync was too far off to commit to (often one moved to the
            // next slot, see plannedVsyncNs()) can be committed now, and the caller does that
            // right after this, for the vsync planned for it. Shown from here, it went out a
            // vsync late: on a Pixel 10 Pro at 60 Hz, half of the frames held on screen an
            // extra vsync came right before one of these.
            if (presentAhead_ && frameVsyncNs(frames[0], true) != 0) {
                break;
            }

            const int64_t tolerance = periodNs_ / 4;
            int newestDue = -1;
            for (size_t i = 0; i < count; i++) {
                if (frames[i].targetNs + shiftNs_ + presentGuardNs() <= vsyncNs) {
                    newestDue = static_cast<int>(i);
                }
            }

            // Whether a new frame would normally be shown at this vsync, and whether the slot
            // before this one should have had one but didn't
            const int64_t sinceLast = vsyncNs - lastPresentVsyncNs_;
            const bool frameExpected = sinceLast * 2 >= (2 * slotVsyncs_ - 1) * periodNs_;
            const bool missedLastSlot = sinceLast * 2 >= (2 * slotVsyncs_ + 1) * periodNs_;

            if (phaseLocked_ && slotVsyncs_ > 1 && (vsyncIndex_ - slotParity_) % slotVsyncs_ != 0) {
                // Between slot vsyncs, frames wait for the next one, unless the frame on screen
                // has already had a full slot. That happens after a late frame, or when the
                // slot vsyncs just moved and the first new one has already passed; waiting for
                // the one after would hold this frame a whole extra slot and then skip one.
                // The oldest due frame keeps the sequence unbroken; any backlog is caught up
                // at the next slot vsync.
                if (frameExpected && newestDue >= 0) {
                    choice = 0;
                }
                break;
            }

            if (newestDue < 0) {
                // Nothing is due yet. If the oldest frame is only just short of due and the
                // stream is otherwise one frame per slot, show it now rather than repeat the
                // last frame here and have two frames due at the next vsync.
                if (!presentAhead_ && frameExpected && frames[0].targetNs + shiftNs_ <= vsyncNs + tolerance) {
                    choice = 0;
                }
            }
            else {
                choice = newestDue;

                // Two or more frames are due after a slot went empty. If the newest is only
                // barely due, show the one before it and leave the newest for the next vsync,
                // which gets the stream back to one frame per slot without skipping one.
                if (!presentAhead_ && newestDue >= 1 && missedLastSlot &&
                        frames[newestDue].targetNs + shiftNs_ > vsyncNs - tolerance) {
                    choice = newestDue - 1;
                }
            }
            break;
        }
    }

    if (choice >= 0) {
        framesSkipped_ += static_cast<uint64_t>(choice);
        lastPresentVsyncNs_ = vsyncNs;
    }
    return choice;
}

int64_t FramePacer::frameVsyncNs(const FrameTiming& frame, bool committable) const {
    if (mode_ != PacingMode::HostTimed || lastVsyncNs_ == 0) {
        return 0;
    }

    // The first vsync after the last one seen that the frame is due at, as onVsync() would
    // find it. A frame already due (it arrived late) goes to the next vsync.
    const int64_t due = frame.targetNs + shiftNs_ + presentGuardNs();
    int64_t ahead = 1;
    if (due > lastVsyncNs_ + periodNs_) {
        ahead = (due - lastVsyncNs_ + periodNs_ - 1) / periodNs_;
    }
    int64_t index = vsyncIndex_ + ahead;

    // Locked to slots: only slot vsyncs, unless the frame on screen by then will have had a
    // full slot (the catch-up onVsync() allows between slot vsyncs)
    if (phaseLocked_ && slotVsyncs_ > 1) {
        while (((index - slotParity_) % slotVsyncs_ + slotVsyncs_) % slotVsyncs_ != 0) {
            const int64_t vsync = lastVsyncNs_ + (index - vsyncIndex_) * periodNs_;
            if ((vsync - lastPresentVsyncNs_) * 2 >= (2 * slotVsyncs_ - 1) * periodNs_) {
                break;
            }
            index++;
        }
    }

    // Unless the frames are locked to slots (whose schedule is already smooth), a frame whose
    // due time is right at a vsync would flip between that vsync and the next with the
    // slightest jitter: one frame replacing another at a vsync, then a vsync without a new frame.
    // Keep one frame per slot through that jitter; only a frame well off the slot moves.
    if (lastPresentVsyncNs_ != 0 && !(phaseLocked_ && slotVsyncs_ > 1)) {
        const int64_t tolerance = periodNs_ / 4;
        const int64_t planned = lastVsyncNs_ + (index - vsyncIndex_) * periodNs_;
        // The slot after the last frame committed, if it hasn't passed
        const int64_t nextSlot = lastPresentVsyncNs_ + slotVsyncs_ * periodNs_;
        const int64_t nextSlotIndex = vsyncIndex_ + (nextSlot - lastVsyncNs_ + periodNs_ / 2) / periodNs_;
        if (nextSlot > lastVsyncNs_ + periodNs_ / 2) {
            if (planned <= lastPresentVsyncNs_ + periodNs_ / 2 && due > lastPresentVsyncNs_ - tolerance) {
                // Only just due at the vsync the last frame was committed to: the next slot
                // rather than replacing that frame
                index = nextSlotIndex;
            }
            else if (planned >= nextSlot + periodNs_ / 2 && due <= nextSlot + tolerance) {
                // Only just short of the next slot: that slot rather than leaving it empty
                index = nextSlotIndex;
            }
        }
    }

    // Not further ahead than the next vsync. A frame due later is presented at the vsync
    // before its own, still a vsync early.
    if (committable && index - vsyncIndex_ > kMaxPresentAheadVsyncs) {
        return 0;
    }

    const int64_t vsync = lastVsyncNs_ + (index - vsyncIndex_) * periodNs_;
    if (vsync < lastPresentVsyncNs_ - periodNs_ / 2) {
        return 0;
    }
    // The same vsync as the last frame committed: that one arrived late and was put on the
    // next vsync, which is this frame's own. Two images asking for the same vsync, the
    // compositor shows the newer one, so this frame replaces it. Leaving this frame to its
    // vsync instead would present it with no time to spare, and it could miss the vsync too.
    return vsync;
}

void FramePacer::onPresentedAhead(int64_t vsyncNs) {
    // Replacing the frame committed to the same vsync (see plannedVsyncNs()): the compositor
    // shows only this one
    if (lastPresentVsyncNs_ != 0 && vsyncNs <= lastPresentVsyncNs_ + periodNs_ / 2) {
        framesSkipped_++;
    }
    lastPresentVsyncNs_ = vsyncNs;
}

void FramePacer::onPresentedImmediately(int64_t nowNs) {
    lastPresentVsyncNs_ = nowNs;
}

void FramePacer::addReadyCost(int64_t costNs) {
    readyCosts_.push_back(costNs);
    if (readyCosts_.size() > kPartialReadySamples) {
        readyCosts_.pop_front();
    }
}

bool FramePacer::partialDeadlineOffsetNs(const FrameTiming& frame, int64_t* offsetNs, int64_t* readyByNs,
                                         int64_t* readyCostNs) const {
    // Not through the warm-up: with no buffer then, nearly every frame would be cut
    if (mode_ != PacingMode::HostTimed || !timeline_.warmedUp() || readyCosts_.size() < kPartialMinReadySamples) {
        return false;
    }

    std::vector<int64_t> costs(readyCosts_.begin(), readyCosts_.end());
    const auto percentile = costs.begin() + (costs.size() - 1) * kPartialReadyPercentile / 100;
    std::nth_element(costs.begin(), percentile, costs.end());

    // When the frame must be ready by to be shown on time. Presented ahead, that's the guard
    // before the vsync it's planned for, which can be most of a vsync after it's due (more with
    // several vsyncs a frame). Otherwise, it's when it's due. The frames after this one are
    // taken to have as long after their host timestamps as this one does.
    int64_t readyBy = frame.targetNs + shiftNs_;
    if (presentAhead_) {
        // Its vsync even if that's too far off to commit the frame to yet, as for a frame
        // that arrived early
        const int64_t vsyncNs = frameVsyncNs(frame, false);
        if (vsyncNs != 0) {
            readyBy = std::max(readyBy, vsyncNs - presentGuardNs());
        }
    }
    *offsetNs = readyBy - frame.hostPtsNs - *percentile;
    if (readyByNs) {
        *readyByNs = readyBy;
    }
    if (readyCostNs) {
        *readyCostNs = *percentile;
    }
    return true;
}

}  // namespace vkr
