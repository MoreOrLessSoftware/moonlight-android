#include "frame_pacer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vkr {

namespace {
    // Vsyncs the period is measured over
    constexpr size_t kVsyncWindow = 480;
    constexpr size_t kMinVsyncsForPeriod = 60;

    // Host timestamp spacings the slot is taken from
    constexpr size_t kSlotDeltaWindow = 15;
    constexpr size_t kMinSlotDeltas = 8;

    // How much of the part of a slot that host timestamps were early or late is carried to the
    // next frame. Host timestamps jitter: a frame stamped late and the next on time are 1.4 and
    // 0.6 slots apart, which rounded alone would put the second on the same vsync as the first.
    // Carried in full, a host slightly slow or fast against our display would add or drop a
    // vsync by itself, on top of the late frames and pulls that already handle that.
    constexpr double kCarryKeep = 0.5;
}

// Presets chosen by replaying two HEVC sessions on a 144 Hz iPlay 70 mini Ultra over Wi-Fi, one
// at 72 fps and one at 144 fps, where a lone frame arrived 5-10 ms later than the rest every few
// seconds:
// - Low latency, with no headroom: at 72 fps, a 3 s pull hold rather than 1 s cut the uneven
//   frames by about a quarter for about 1 ms more median wait (7.5 ms from arrival to vsync)
// - Balanced, with 4 ms of headroom and frames allowed to wait up to 3 vsyncs: at 72 fps, 9
//   uneven frames in 80 s where Low latency had 12, for a median wait of 13.9 ms. At 144 fps
//   neither changed the uneven frames, which weren't late arrivals; the wait went from 8.1 to
//   14.8 ms. A 30 s hold took 72 fps down to 4 uneven frames for 20.7 ms.
JitterBufferPreset JitterBufferPreset::forJitterBuffer(JitterBuffer jitterBuffer) {
    if (jitterBuffer == JitterBuffer::Balanced) {
        return {4'000'000, 3, 3'000'000'000};
    }
    return {0, 2, 3'000'000'000};
}

FramePacer::FramePacer(PacingMode mode, int streamFps, int64_t vsyncPeriodNs, JitterBuffer jitterBuffer)
    : mode_(mode),
      streamIntervalNs_(1'000'000'000LL / std::max(streamFps, 1)),
      jitterBuffer_(jitterBuffer),
      preset_(JitterBufferPreset::forJitterBuffer(jitterBuffer)),
      periodNs_(vsyncPeriodNs > 0 ? vsyncPeriodNs : 16'666'667) {}

void FramePacer::setJitterBuffer(JitterBuffer jitterBuffer) {
    jitterBuffer_ = jitterBuffer;
    preset_ = JitterBufferPreset::forJitterBuffer(jitterBuffer);
    holdSinceNs_ = 0;
}

void FramePacer::setVsyncPeriod(int64_t periodNs) {
    // Small differences are the caller's estimate being less exact than our own measurement.
    // A large one is a new refresh rate.
    if (periodNs > 0 && std::llabs(periodNs - periodNs_) * 50 > periodNs_) {
        periodNs_ = periodNs;
        vsyncs_.clear();
        anchored_ = false;
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
            // Every frame that may be waiting out the headroom and the wait cap, plus the one due
            // next and one being committed. Fewer, and the caller drops frames the schedule
            // still means to show.
            const int64_t heldNs = preset_.headroomNs + preset_.maxWaitVsyncs * periodNs_;
            const size_t frames = static_cast<size_t>(heldNs / std::max<int64_t>(streamIntervalNs_, 1)) + 2;
            return std::clamp<size_t>(frames, 2, kMaxQueuedFrames);
        }
        case PacingMode::CapFps:
        case PacingMode::Smoothness:
        default:
            return 4;
    }
}

int64_t FramePacer::nearestVsyncNs(int64_t timeNs) const {
    const double vsyncs = static_cast<double>(timeNs - lastVsyncNs_) / static_cast<double>(periodNs_);
    return lastVsyncNs_ + static_cast<int64_t>(std::llround(vsyncs)) * periodNs_;
}

int64_t FramePacer::firstVsyncFromNs(int64_t timeNs) const {
    if (timeNs <= lastVsyncNs_ + periodNs_) {
        return lastVsyncNs_ + periodNs_;
    }
    return lastVsyncNs_ + (timeNs - lastVsyncNs_ + periodNs_ - 1) / periodNs_ * periodNs_;
}

// How many vsyncs each frame should stay on screen, from the spacing of host timestamps: one
// when the host's frame rate matches our refresh rate, two for 60 fps on 120 Hz, and one when
// it's no whole number of vsyncs (100 fps on 144 Hz), so frames chain on the finest grid
void FramePacer::updateSlot(int64_t hostPtsNs) {
    if (lastPtsNs_ != 0) {
        const int64_t delta = hostPtsNs - lastPtsNs_;
        if (delta > 0 && delta < kMaxPtsGapNs) {
            ptsDeltas_.push_back(delta);
            if (ptsDeltas_.size() > kSlotDeltaWindow) {
                ptsDeltas_.pop_front();
            }
        }
    }
    if (ptsDeltas_.size() < kMinSlotDeltas) {
        return;
    }
    std::vector<int64_t> sorted(ptsDeltas_.begin(), ptsDeltas_.end());
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const int64_t median = sorted[sorted.size() / 2];
    const int64_t vsyncs = (median + periodNs_ / 2) / periodNs_;
    if (vsyncs >= 1 && vsyncs <= 8 && std::llabs(median - vsyncs * periodNs_) * 100 <= periodNs_ * 15) {
        slotVsyncs_ = vsyncs;
    }
    else {
        // Kept at a slot that no longer fits, frames chained too far apart and had to be pulled
        // back over and over (a game going from 72 to 100 fps on 144 Hz)
        slotVsyncs_ = 1;
    }
}

void FramePacer::onFrameArrived(FrameTiming& frame) {
    if (mode_ != PacingMode::HostTimed) {
        frame.targetNs = frame.arrivalNs;
        return;
    }

    updateSlot(frame.hostPtsNs);
    const int64_t delta = frame.hostPtsNs - lastPtsNs_;
    const bool continuous = lastPtsNs_ != 0 && delta > 0 && delta <= kMaxPtsGapNs;
    lastPtsNs_ = frame.hostPtsNs;

    if (lastVsyncNs_ == 0) {
        frame.targetNs = frame.arrivalNs;
        return;
    }

    // The earliest the frame could be shown: the first vsync at least the present guard (and the
    // preset's headroom) after it arrived
    const int64_t readyNs = frame.arrivalNs + presentGuardNs() + preset_.headroomNs;
    const int64_t earliestNs = firstVsyncFromNs(readyNs);

    int64_t assignedNs;
    if (!anchored_ || !continuous) {
        // Start (or start over) at the first vsync the frame can make
        assignedNs = earliestNs;
        anchored_ = true;
        carry_ = 0;
        holdSinceNs_ = 0;
    }
    else {
        // The vsync after the last frame's, as many slots on as the host timestamps moved
        const int64_t slotPeriodNs = slotVsyncs_ * periodNs_;
        const double slots = carry_ + static_cast<double>(delta) / static_cast<double>(slotPeriodNs);
        const int64_t step = std::max<int64_t>(1, std::llround(slots));
        carry_ = (slots - static_cast<double>(step)) * kCarryKeep;
        assignedNs = nearestVsyncNs(lastAssignedNs_ + step * slotPeriodNs);

        const int64_t roomNs = assignedNs - readyNs;
        if (roomNs < 0) {
            // Too late for its vsync: the first one it can make, and the frames after it follow
            assignedNs = earliestNs;
            lateFrames_++;
            holdSinceNs_ = 0;
        }
        else if (roomNs >= preset_.maxWaitVsyncs * periodNs_) {
            // Waiting too long (frames bunched up after a stall, or the host running fast)
            const int64_t excess = roomNs / periodNs_ - (preset_.maxWaitVsyncs - 1);
            assignedNs = std::max(assignedNs - excess * periodNs_, lastAssignedNs_);
            pulls_++;
            holdSinceNs_ = 0;
        }
        else if (roomNs >= periodNs_ + periodNs_ / kPullMarginDivisor) {
            // Could have made the vsync before its own. Once every frame could for long enough,
            // move a vsync sooner. With a frame a vsync, the frame assigned that vsync is
            // replaced by this one.
            if (holdSinceNs_ == 0) {
                holdSinceNs_ = frame.arrivalNs;
            }
            else if (frame.arrivalNs - holdSinceNs_ >= preset_.pullHoldNs) {
                assignedNs = std::max(assignedNs - periodNs_, lastAssignedNs_);
                pulls_++;
                holdSinceNs_ = 0;
            }
        }
        else {
            holdSinceNs_ = 0;
        }
    }

    lastAssignedNs_ = assignedNs;
    waitNs_ = assignedNs - (readyNs - preset_.headroomNs);
    frame.targetNs = assignedNs;
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
            // Frames that can be presented ahead are, by the caller, right after this
            if (presentAhead_ && frameVsyncNs(frames[0], true) != 0) {
                break;
            }
            // The newest frame assigned this vsync or an earlier one
            for (size_t i = 0; i < count; i++) {
                if (frames[i].targetNs <= vsyncNs + periodNs_ / 2) {
                    choice = static_cast<int>(i);
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
    if (mode_ != PacingMode::HostTimed || lastVsyncNs_ == 0 || !anchored_) {
        return 0;
    }

    // Its assigned vsync, or if that has passed (it waited for a vsync callback), the next one
    int64_t vsyncNs = std::max(frame.targetNs, lastVsyncNs_ + periodNs_);

    if (committable && vsyncNs - lastVsyncNs_ > presentAheadVsyncs() * periodNs_ + periodNs_ / 2) {
        return 0;
    }
    // Frames are shown in order. The same vsync as the last frame committed replaces it.
    if (lastPresentVsyncNs_ != 0 && vsyncNs < lastPresentVsyncNs_ - periodNs_ / 2) {
        vsyncNs = lastPresentVsyncNs_;
    }
    return vsyncNs;
}

void FramePacer::onPresentedAhead(int64_t vsyncNs) {
    // Replacing the frame committed to the same vsync: the compositor shows only this one
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
    if (mode_ != PacingMode::HostTimed || !anchored_ || readyCosts_.size() < kPartialMinReadySamples) {
        return false;
    }

    std::vector<int64_t> costs(readyCosts_.begin(), readyCosts_.end());
    const auto percentile = costs.begin() + (costs.size() - 1) * kPartialReadyPercentile / 100;
    std::nth_element(costs.begin(), percentile, costs.end());

    // Ready the present guard before its vsync. The frames after this one are taken to have as
    // long after their host timestamps as this one does.
    const int64_t readyBy = frame.targetNs - presentGuardNs();
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
