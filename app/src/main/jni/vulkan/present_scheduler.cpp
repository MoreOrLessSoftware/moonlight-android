#include "present_scheduler.h"

#include <algorithm>

namespace vkr {

void PresentScheduler::onAheadOutcome(int64_t slackNs, bool failed, int64_t periodNs) {
    // A new swapchain's first presents fail for reasons of their own (onSwapchainCreated())
    if (slackNs < 0 || periodNs <= 0 || settling_ > 0) {
        return;
    }
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

    // Down from the most slack, while each 1 ms band fails rarely enough. Bands with too few
    // frames to judge are passed over above the first one judged, and end the search below it.
    // Only a band that fails raises the guard: frames rarely presented with that little slack
    // isn't a reason to keep them further from it. Failing means clearly more often than the
    // frames with more slack, too: on a tablet, bursts of failures at every slack alike (the
    // compositor stalling) otherwise took the guard to half a vsync.
    const int64_t maxGuardNs = periodNs / 2;
    int lowest = -1;
    bool failing = false;
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
    else if (target < guardNs_) {
        guardNs_ = std::max(target, guardNs_ - kGuardBucketNs);
    }
}

void PresentScheduler::resetWindows() {
    raiseCount_ = raiseMisses_ = 0;
    lowerCount_ = lowerNotSooner_ = lowerMisses_ = 0;
}

void PresentScheduler::onPresented(int64_t vsyncNs, int delayUsed, int64_t periodNs, int64_t actualNs,
                                   int64_t earliestNs, int64_t marginNs) {
    if (periodNs <= 0) {
        return;
    }

    const int64_t target = vsyncNs + delayUsed * periodNs;
    const bool missed = actualNs > target + periodNs / 2;
    missedTotal_ += missed ? 1 : 0;

    if (settling_ > 0) {
        settling_--;
        return;
    }

    // Presents made before the last change tell us nothing about the current delay
    if (delayUsed != delay_) {
        return;
    }

    raiseCount_++;
    raiseMisses_ += missed ? 1 : 0;
    if (raiseCount_ >= kRaiseWindow || raiseMisses_ >= kRaiseMisses) {
        if (raiseMisses_ >= kRaiseMisses && delay_ < kMaxDelay) {
            delay_++;
            // A shorter delay that didn't hold: wait longer before trying it again
            if (loweredLast_) {
                lowerBackoff_ = std::min<uint32_t>(lowerBackoff_ * 2, 16);
            }
            loweredLast_ = false;
            resetWindows();
            return;
        }
        raiseCount_ = raiseMisses_ = 0;
    }

    // Could it have made the vsync before its target? Either the image was ready in time for
    // it (earliest), or it was ready over a vsync before the compositor needed it (margin).
    const bool couldBeSooner = earliestNs <= target - periodNs + periodNs / 4 ||
                               marginNs >= periodNs + periodNs / 8;
    lowerCount_++;
    lowerNotSooner_ += couldBeSooner ? 0 : 1;
    lowerMisses_ += missed ? 1 : 0;
    if (lowerCount_ >= kLowerWindow * lowerBackoff_) {
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
