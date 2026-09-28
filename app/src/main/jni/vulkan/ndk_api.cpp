#include "ndk_api.h"

#include <dlfcn.h>
#include <mutex>
#include <android/log.h>

#define LOG_TAG "VulkanRenderer"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
    NdkApi api;
    bool apiLoaded = false;
    std::once_flag loadOnce;

    // NDK functions aren't always exported from the library their header suggests: the JNI
    // helpers in android/native_window_jni.h (ANativeWindow_fromSurface/toSurface) live in
    // libandroid.so, not libnativewindow.so. Every symbol is looked up in all of them.
    void* libraries[3];

    template <typename T>
    bool loadSymbol(const char* name, T& out, bool required = true) {
        out = nullptr;
        for (void* lib : libraries) {
            if (lib && (out = reinterpret_cast<T>(dlsym(lib, name)))) {
                return true;
            }
        }
        if (required) {
            ALOGE("Missing NDK function %s", name);
            return false;
        }
        return true;
    }

    void load() {
        libraries[0] = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        libraries[1] = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
        libraries[2] = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);

        bool ok = true;
        ok &= loadSymbol("AHardwareBuffer_acquire", api.AHardwareBuffer_acquire);
        ok &= loadSymbol("AHardwareBuffer_release", api.AHardwareBuffer_release);
        ok &= loadSymbol("AHardwareBuffer_describe", api.AHardwareBuffer_describe);
        ok &= loadSymbol("ANativeWindow_toSurface", api.ANativeWindow_toSurface);

        ok &= loadSymbol("AImageReader_newWithUsage", api.AImageReader_newWithUsage);
        ok &= loadSymbol("AImageReader_delete", api.AImageReader_delete);
        ok &= loadSymbol("AImageReader_getWindow", api.AImageReader_getWindow);
        ok &= loadSymbol("AImageReader_setImageListener", api.AImageReader_setImageListener);
        ok &= loadSymbol("AImageReader_acquireNextImage", api.AImageReader_acquireNextImage);
        ok &= loadSymbol("AImage_delete", api.AImage_delete);
        ok &= loadSymbol("AImage_getHardwareBuffer", api.AImage_getHardwareBuffer);
        ok &= loadSymbol("AImage_getTimestamp", api.AImage_getTimestamp);
        ok &= loadSymbol("AImage_getCropRect", api.AImage_getCropRect);

        ok &= loadSymbol("AChoreographer_getInstance", api.AChoreographer_getInstance);
        ok &= loadSymbol("AChoreographer_postFrameCallback64", api.AChoreographer_postFrameCallback64);
        loadSymbol("AChoreographer_registerRefreshRateCallback",
                   api.AChoreographer_registerRefreshRateCallback, false);
        loadSymbol("AChoreographer_unregisterRefreshRateCallback",
                   api.AChoreographer_unregisterRefreshRateCallback, false);

        // The libraries stay loaded for the life of the process
        apiLoaded = ok;
    }
}

const NdkApi* loadNdkApi() {
    std::call_once(loadOnce, load);
    return apiLoaded ? &api : nullptr;
}
