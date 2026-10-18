#pragma once

#include <cstdint>
#include <mutex>
#include <sys/types.h>
#include <vector>

#include "ndk_api.h"

namespace vkr {

// An Android performance hint session (ADPF, API 33) for the threads that render and present
// frames. Thread priority only decides which thread runs first; the CPU governor still puts a
// light, periodic thread like the render thread on a small core at a low clock, where a render
// that takes 1 ms can take several and miss its vsync. With a session, each render's CPU time is
// reported against a target, and the system raises the clocks or moves the threads before
// renders run over it.
//
// Threads join from wherever they start (addThread()); the session is created with the first,
// and given the rest as they join. Without the API or the system's support, it does nothing.
class PerfHint {
public:
    // targetNs: how long one render should take on the CPU. 0 turns hints off.
    void start(const NdkApi* ndk, int64_t targetNs);
    ~PerfHint();

    // The calling thread joins the session
    void addThread();

    // How long one render took on the CPU, leaving out waits
    void reportWork(int64_t workNs);

    bool active() const;

    static constexpr int64_t kDefaultTargetNs = 2'000'000;

private:
    // Under mutex_
    void applyThreads();

    mutable std::mutex mutex_;
    const NdkApi* ndk_ = nullptr;
    int64_t targetNs_ = 0;
    APerformanceHintSession* session_ = nullptr;
    std::vector<pid_t> threads_;
    bool failed_ = false;
};

}  // namespace vkr
