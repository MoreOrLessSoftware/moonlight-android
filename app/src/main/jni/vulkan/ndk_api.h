#pragma once

// The app builds native code against API 21, but the Vulkan renderer needs NDK functions
// from API 26-30. They are looked up at runtime so the library still loads on older
// devices, where the Java side never enables the Vulkan renderer.

#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/choreographer.h>
#include <android/native_window.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

struct NdkApi {
    // libnativewindow.so (API 26)
    void (*AHardwareBuffer_acquire)(AHardwareBuffer* buffer);
    void (*AHardwareBuffer_release)(AHardwareBuffer* buffer);
    void (*AHardwareBuffer_describe)(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);
    jobject (*ANativeWindow_toSurface)(JNIEnv* env, ANativeWindow* window);

    // libmediandk.so (API 24-26)
    media_status_t (*AImageReader_newWithUsage)(int32_t width, int32_t height, int32_t format,
                                                uint64_t usage, int32_t maxImages, AImageReader** reader);
    void (*AImageReader_delete)(AImageReader* reader);
    media_status_t (*AImageReader_getWindow)(AImageReader* reader, ANativeWindow** window);
    media_status_t (*AImageReader_setImageListener)(AImageReader* reader, AImageReader_ImageListener* listener);
    media_status_t (*AImageReader_acquireNextImage)(AImageReader* reader, AImage** image);
    void (*AImage_delete)(AImage* image);
    media_status_t (*AImage_getHardwareBuffer)(const AImage* image, AHardwareBuffer** buffer);
    media_status_t (*AImage_getTimestamp)(const AImage* image, int64_t* timestampNs);
    media_status_t (*AImage_getCropRect)(const AImage* image, AImageCropRect* rect);

    // libandroid.so (API 24-30)
    AChoreographer* (*AChoreographer_getInstance)();
    void (*AChoreographer_postFrameCallback64)(AChoreographer* choreographer,
                                               AChoreographer_frameCallback64 callback, void* data);
    // Optional (API 30)
    void (*AChoreographer_registerRefreshRateCallback)(AChoreographer* choreographer,
                                                       AChoreographer_refreshRateCallback callback, void* data);
    void (*AChoreographer_unregisterRefreshRateCallback)(AChoreographer* choreographer,
                                                         AChoreographer_refreshRateCallback callback, void* data);
};

// Loads the functions above. Returns nullptr if a required one is missing.
const NdkApi* loadNdkApi();
