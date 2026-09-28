#include "present_scheduler.h"

#include <algorithm>

namespace vkr {

void PresentScheduler::resetWindows() {
    raiseCount_ = raiseMisses_ = 0;
    lowerCount_ = lowerNotSooner_ = 0;
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
    if (lowerCount_ >= kLowerWindow * lowerBackoff_) {
        // All but one in two hundred (frames that arrived late have little time to spare),
        // and none missed
        if (delay_ > kMinDelay && lowerNotSooner_ * 200 <= lowerCount_ && raiseMisses_ == 0) {
            delay_--;
            loweredLast_ = true;
            resetWindows();
            return;
        }
        lowerCount_ = lowerNotSooner_ = 0;
    }
}

}  // namespace vkr
