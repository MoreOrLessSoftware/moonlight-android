#include "present_scheduler.h"

#include <algorithm>
#include <iterator>

namespace vkr {

void PresentScheduler::fix(int delayVsyncs, int64_t guardNs) {
    if (delayVsyncs > 0) {
        delay_ = std::min(std::max(delayVsyncs, kMinDelay), kMaxDelay);
        delayFixed_ = true;
    }
    if (guardNs >= 0) {
        guardNs_ = guardNs;
        guardFixed_ = true;
    }
}

void PresentScheduler::resetGuardHistory() {
    std::fill(std::begin(guardSamples_), std::end(guardSamples_), 0.0);
    std::fill(std::begin(guardFails_), std::end(guardFails_), 0.0);
    std::fill(std::begin(needSamples_), std::end(needSamples_), 0.0);
    guardTotal_ = 0;
    needTotal_ = 0;
    guardSinceCheck_ = 0;
    guardRaiseHold_ = 0;
    needCount_ = 0;
    needStartNs_ = 0;
    std::fill(std::begin(delayNeedSamples_), std::end(delayNeedSamples_), 0u);
    delayNeedCount_ = 0;
    delayNeedNext_ = 0;
}

int64_t PresentScheduler::delayNeedGuardNs() const {
    if (delayNeedCount_ < kLowerMinNeedSamples) {
        return -1;
    }
    uint32_t below = 0;
    int bucket = 0;
    for (; bucket < kNeedBuckets; bucket++) {
        below += delayNeedSamples_[bucket];
        if (below * 100.0 >= delayNeedCount_ * kLowerNeedPercentile) {
            break;
        }
    }
    return std::max(static_cast<int64_t>(bucket + 2) * kGuardBucketNs, kMinGuardNs);
}

void PresentScheduler::onAheadOutcome(int64_t slackNs, int64_t vsyncNs, int delayUsed, bool reached,
                                      int64_t actualNs, int64_t earliestNs, int64_t marginNs, int64_t periodNs) {
    // A new swapchain's first presents fail for reasons of their own (onSwapchainCreated()), and
    // presents made before the delay last changed tell us nothing about the current one
    if (slackNs < 0 || periodNs <= 0 || settling_ > 0 || delayUsed != delay_) {
        return;
    }
    aheadSeen_ = true;

    // How long before its vsync it needed presenting to reach the screen at the delay, and a
    // vsync sooner: what it needed for the vsync it could have made at the earliest, moved by the
    // vsyncs between that one and the one wanted. Reports without a margin say nothing about it.
    const int64_t earliestVsyncs = (earliestNs - vsyncNs + periodNs / 2) / periodNs;
    if (reached && marginNs > 0) {
        const int64_t delayNeedNs = slackNs - marginNs + (earliestVsyncs - delay_) * periodNs;
        const auto bucket = static_cast<uint8_t>(std::clamp<int64_t>(delayNeedNs / kGuardBucketNs, 0, kNeedBuckets - 1));
        if (delayNeedCount_ == kNeedWindow) {
            delayNeedSamples_[delayNeedRing_[delayNeedNext_]]--;
        }
        else {
            delayNeedCount_++;
        }
        delayNeedRing_[delayNeedNext_] = bucket;
        delayNeedSamples_[bucket]++;
        delayNeedNext_ = (delayNeedNext_ + 1) % kNeedWindow;
    }
    if (reached && marginNs > 0 && delay_ > kMinDelay) {
        const int64_t needNs = slackNs - marginNs + (earliestVsyncs - (delay_ - 1)) * periodNs;
        const int bucket = static_cast<int>(
                std::clamp<int64_t>(needNs / kGuardBucketNs, 0, kNeedBuckets - 1));
        needSamples_[bucket] += 1;
        needTotal_ += 1;
        if (needCount_++ == 0) {
            needStartNs_ = vsyncNs;
        }
        if (needTotal_ > kGuardHistory) {
            for (double& n : needSamples_) {
                n /= 2;
            }
            needTotal_ /= 2;
        }
    }

    const bool failed = !reached || actualNs > vsyncNs + delayUsed * periodNs + periodNs / 2;
    const int bucket = static_cast<int>(std::min<int64_t>(slackNs / kGuardBucketNs, kGuardBuckets - 1));
    guardSamples_[bucket] += 1;
    guardFails_[bucket] += failed ? 1 : 0;
    guardTotal_ += 1;
    if (guardTotal_ > kGuardHistory) {
        for (int i = 0; i < kGuardBuckets; i++) {
            guardSamples_[i] /= 2;
            guardFails_[i] /= 2;
        }
        guardTotal_ /= 2;
    }
    if (guardRaiseHold_ > 0) {
        guardRaiseHold_--;
    }
    if (++guardSinceCheck_ < kGuardCheckSamples) {
        return;
    }
    guardSinceCheck_ = 0;

    const int delayBefore = delay_;
    considerLowering(vsyncNs, periodNs);
    if (guardFixed_ || delay_ != delayBefore) {
        // Fixed, or just set for a shorter delay along with a fresh history
        return;
    }

    // Down from the most slack, while each 1 ms band fails rarely enough. Bands with too few
    // frames to judge are passed over above the first one judged, and end the search below it.
    // Only a band that fails raises the guard: frames rarely presented with that little slack
    // isn't a reason to keep them further from it. Failing means clearly more often than the
    // frames with more slack, too: on a tablet, bursts of failures at every slack alike (the
    // compositor stalling) otherwise took the guard to half a vsync. A guard set for a shorter
    // delay may be more (considerLowering()).
    const int64_t maxGuardNs = std::max(periodNs / 2, loweredGuardCapNs_);
    int lowest = -1;
    bool failing = false;
    bool stoppedAtFails = false;  // Rather than for want of frames
    double aboveSamples = 0, aboveFails = 0;
    for (int start = kGuardBuckets - kGuardBandBuckets; start >= 0; start--) {
        if (start + kGuardBandBuckets < kGuardBuckets) {
            aboveSamples += guardSamples_[start + kGuardBandBuckets];
            aboveFails += guardFails_[start + kGuardBandBuckets];
        }
        double samples = 0, fails = 0;
        for (int i = start; i < start + kGuardBandBuckets; i++) {
            samples += guardSamples_[i];
            fails += guardFails_[i];
        }
        if (samples < kGuardMinBandSamples) {
            if (lowest >= 0) {
                break;
            }
            continue;
        }
        if (fails * 100 > samples * kGuardMaxFailPercent) {
            // Not going below this band. Raising the guard above it takes more: enough failures
            // to be sure of, and clearly more than with more slack.
            failing = fails >= kGuardMinBandFails && aboveSamples >= kGuardMinBandSamples &&
                      fails * aboveSamples > 2 * aboveFails * samples;
            stoppedAtFails = true;
            break;
        }
        lowest = start;
    }
    if (lowest < 0) {
        return;
    }

    // Stopped for want of frames just below: with the guard doing its job, frames presented
    // with only a little more slack than it are rare (a few dozen a session on a Pixel 10 Pro,
    // one in ten failing), too few for any one band. Taken together, from the guard up, they
    // can still show that it's too low.
    const int guardBucket = static_cast<int>(std::min<int64_t>(guardNs_ / kGuardBucketNs, kGuardBuckets - 1));
    if (!failing && lowest > guardBucket) {
        double samples = 0, fails = 0, higherSamples = 0, higherFails = 0;
        for (int i = 0; i < kGuardBuckets; i++) {
            if (i >= guardBucket && i < lowest) {
                samples += guardSamples_[i];
                fails += guardFails_[i];
            }
            else if (i >= lowest) {
                higherSamples += guardSamples_[i];
                higherFails += guardFails_[i];
            }
        }
        failing = fails >= kGuardMinBandFails && fails * 100 > samples * kGuardMaxFailPercent &&
                  higherSamples >= kGuardMinBandSamples && fails * higherSamples > 2 * higherFails * samples;
    }
    const int64_t target = std::min(std::max(lowest * kGuardBucketNs, kMinGuardNs), maxGuardNs);
    if (target > guardNs_ && failing) {
        // A step at a time, each on new frames, so one burst of failures still in the history
        // doesn't take it all the way
        if (guardRaiseHold_ == 0) {
            guardNs_ = std::min(target, guardNs_ + 2 * kGuardBucketNs);
            guardRaiseHold_ = kGuardRaiseHoldSamples;
        }
    }
    else {
        // Held where it was set for a shorter delay by the failure rates: that was from what
        // nearly all frames needed (considerLowering()), and lowered from there by the failure
        // rates, frames presented just over it missed until the delay went back up (in
        // present_scheduler_test, at 72 fps on 144 Hz, within 20-30 s). A compositor that gets
        // slower is caught by the misses.
        int64_t lowerTo = loweredLast_ ? guardNs_ : std::min(target, guardNs_);
        // Stopped for want of frames with less slack, not at a band that fails: down to what
        // recent frames needed (see guardNs()). Not below a band that fails. This is how a guard
        // for a shorter delay was chosen, so it applies to one too: chosen from a slow stretch
        // (the delay lowered from three vsyncs to two as the host's desktop went quiet), it's
        // otherwise kept for the session.
        const int64_t needGuardNs = delayNeedGuardNs();
        if (!stoppedAtFails && !failing && needGuardNs >= 0) {
            lowerTo = std::min(lowerTo, needGuardNs);
        }
        if (lowerTo < guardNs_) {
            guardNs_ = std::max(lowerTo, guardNs_ - kGuardBucketNs);
        }
    }
}

void PresentScheduler::considerLowering(int64_t vsyncNs, int64_t periodNs) {
    if (delayFixed_ || guardFixed_ || delay_ <= kMinDelay || needCount_ < kLowerMinNeedSamples ||
            vsyncNs - needStartNs_ < kLowerLearnNs) {
        return;
    }

    // The guard that covers nearly all of what frames needed for the shorter delay, taken if
    // it's clearly worth a vsync: frames due within the guard of a vsync go to the next one.
    double below = 0;
    int bucket = 0;
    for (; bucket < kNeedBuckets; bucket++) {
        below += needSamples_[bucket];
        if (below * 100 >= needTotal_ * kLowerNeedPercentile) {
            break;
        }
    }
    const int64_t guard = std::max(static_cast<int64_t>(bucket + 2) * kGuardBucketNs, kMinGuardNs);
    const int64_t windowNs = aheadWindowNs_ > 0 ? aheadWindowNs_ : periodNs;
    // The guard's growth is counted from the initial guard, or the current one if that's grown
    // past it. Counted from the current guard alone, the decision rested on how far it had come
    // down by the time this was learned: on an iPlay 70 mini Ultra at 144 fps, needing a 6 ms
    // guard, sessions whose guard was still at 2 ms lowered and ones where it had reached 0.5 ms
    // within the first seconds didn't, for the whole session.
    const int64_t baseGuardNs = std::max(guardNs_, kInitialGuardNs);
    if (guard > windowNs - kLowerGuardHeadroomNs ||
            static_cast<double>(guard - baseGuardNs) >= kLowerMaxGuardCost * static_cast<double>(periodNs)) {
        // Not now: learned afresh for the next try, so each decision rests on the last few
        // seconds. Kept, a few seconds of a slow compositor stayed over the percentile for
        // minutes after it recovered (in present_scheduler_test, 3 s of it kept two vsyncs for
        // three minutes).
        std::fill(std::begin(needSamples_), std::end(needSamples_), 0.0);
        needTotal_ = 0;
        needCount_ = 0;
        needStartNs_ = 0;
        return;
    }

    guardBeforeLower_ = guardNs_;
    loweredGuardCapNs_ = guard;
    guardNs_ = guard;
    delay_--;
    loweredLast_ = true;
    resetWindows();
    resetGuardHistory();
}

void PresentScheduler::setAheadWindowNs(int64_t windowNs) {
    aheadWindowNs_ = windowNs;
    if (loweredLast_ && loweredGuardCapNs_ > 0 && windowNs > 0 && guardNs_ > windowNs - kLowerGuardHeadroomNs) {
        // Not a failure of the shorter delay: it's tried again once frames can be committed
        // early enough
        undoLowering();
    }
}

void PresentScheduler::undoLowering() {
    delay_++;
    if (guardBeforeLower_ > 0 && !guardFixed_) {
        guardNs_ = guardBeforeLower_;
    }
    loweredGuardCapNs_ = 0;
    loweredLast_ = false;
    resetWindows();
    resetGuardHistory();
}

void PresentScheduler::resetWindows() {
    raiseCount_ = raiseMisses_ = 0;
    lowerCount_ = lowerNotSooner_ = lowerMisses_ = 0;
}

void PresentScheduler::onPresented(int64_t vsyncNs, int delayUsed, int64_t periodNs, int64_t actualNs,
                                   int64_t earliestNs, int64_t marginNs, int64_t slackNs) {
    if (periodNs <= 0) {
        return;
    }

    const int64_t target = vsyncNs + delayUsed * periodNs;
    const bool late = actualNs > target + periodNs / 2;
    missedTotal_ += late ? 1 : 0;

    // Not too short a delay, but a pause in the stream or a compositor hiccup (see the header)
    // A pause is at least a few frames' time, however slow the stream (the window is a frame)
    const int64_t pauseNs = std::max(kResumeGapNs, 3 * (aheadWindowNs_ > 0 ? aheadWindowNs_ : periodNs));
    if (lastReportVsyncNs_ != 0 && vsyncNs - lastReportVsyncNs_ >= pauseNs) {
        resumingPresents_ = kResumePresents;
    }
    lastReportVsyncNs_ = vsyncNs;
    const bool resuming = resumingPresents_ > 0;
    if (resuming) {
        resumingPresents_--;
    }
    const bool missed = late && !resuming && !(slackNs >= 0 && slackNs >= guardNs_ + periodNs);

    if (settling_ > 0) {
        settling_--;
        return;
    }

    // Presents made before the last change tell us nothing about the current delay
    if (delayFixed_ || delayUsed != delay_) {
        return;
    }

    raiseCount_++;
    raiseMisses_ += missed ? 1 : 0;
    if (raiseCount_ >= kRaiseWindow || raiseMisses_ >= kRaiseMisses) {
        if (raiseMisses_ >= kRaiseMisses && delay_ < kMaxDelay) {
            // A shorter delay that didn't hold: back to the guard from before it. It's tried
            // again after learning afresh for the usual time, not longer each time. A delay that
            // held for a while and then didn't is a compositor that slowed for a while, and a
            // wait that doubled with each one (up to 80 s of learning) could keep the longer
            // delay for the rest of a session. One that never holds still only tries again
            // after a further kLowerLearnNs and its misses.
            if (loweredLast_) {
                undoLowering();
                return;
            }
            delay_++;
            resetWindows();
            resetGuardHistory();
            return;
        }
        raiseCount_ = raiseMisses_ = 0;
    }

    // Frames presented ahead lower the delay along with the guard (considerLowering())
    if (aheadSeen_) {
        return;
    }

    lowerCount_++;
    lowerNotSooner_ += couldBeSooner(target, earliestNs, marginNs, periodNs) ? 0 : 1;
    lowerMisses_ += missed ? 1 : 0;
    if (lowerCount_ >= kLowerWindow) {
        // All but one in two hundred could have (frames that arrived late have little time to
        // spare), and hardly any missed
        if (delay_ > kMinDelay && lowerNotSooner_ * 200 <= lowerCount_ && lowerMisses_ <= kLowerMaxMisses) {
            delay_--;
            loweredLast_ = true;
            resetWindows();
            return;
        }
        lowerCount_ = lowerNotSooner_ = lowerMisses_ = 0;
    }
}

}  // namespace vkr
