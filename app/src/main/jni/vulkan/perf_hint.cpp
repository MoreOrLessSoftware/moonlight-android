#include "perf_hint.h"

#include <algorithm>
#include <android/log.h>
#include <unistd.h>

#define LOG_TAG "VulkanRenderer"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace vkr {

void PerfHint::start(const NdkApi* ndk, int64_t targetNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    ndk_ = ndk;
    targetNs_ = targetNs;
    if (targetNs <= 0) {
        ALOGI("Performance hints off");
    }
    else if (!ndk || !ndk->APerformanceHint_getManager) {
        ALOGI("No performance hints before Android 13");
        failed_ = true;
    }
}

PerfHint::~PerfHint() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_) {
        ndk_->APerformanceHint_closeSession(session_);
        session_ = nullptr;
    }
}

void PerfHint::addThread() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (targetNs_ <= 0 || failed_) {
        return;
    }
    const pid_t tid = gettid();
    if (std::find(threads_.begin(), threads_.end(), tid) != threads_.end()) {
        return;
    }
    threads_.push_back(tid);
    applyThreads();
}

void PerfHint::applyThreads() {
    if (session_ && ndk_->APerformanceHint_setThreads) {
        const int error = ndk_->APerformanceHint_setThreads(session_, threads_.data(), threads_.size());
        if (error == 0) {
            ALOGI("Performance hint session now has %zu threads", threads_.size());
            return;
        }
        ALOGW("Could not change the performance hint session's threads: %d", error);
    }

    // Before Android 14, a session's threads are fixed, so a new thread means a new session
    if (session_) {
        ndk_->APerformanceHint_closeSession(session_);
        session_ = nullptr;
    }
    APerformanceHintManager* manager = ndk_->APerformanceHint_getManager();
    if (manager) {
        session_ = ndk_->APerformanceHint_createSession(manager, threads_.data(), threads_.size(), targetNs_);
    }
    if (!session_) {
        ALOGI("No performance hint session on this device");
        failed_ = true;
        return;
    }
    ALOGI("Performance hint session for %zu threads, target %lld us", threads_.size(),
          static_cast<long long>(targetNs_ / 1000));
}

void PerfHint::reportWork(int64_t workNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_ && workNs > 0) {
        ndk_->APerformanceHint_reportActualWorkDuration(session_, workNs);
    }
}

bool PerfHint::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ != nullptr;
}

}  // namespace vkr
