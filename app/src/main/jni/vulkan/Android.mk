LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := vulkan_renderer
LOCAL_SRC_FILES := \
    frame_pacer.cpp \
    ndk_api.cpp \
    pacer_trace.cpp \
    present_scheduler.cpp \
    pyrowave_decoder.cpp \
    vk_api.cpp \
    vulkan_bridge.cpp \
    vulkan_renderer.cpp \

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_CPPFLAGS += -std=c++17 -Wall

# Vulkan and the API 26+ NDK libraries are loaded at runtime (see ndk_api.h and vk_api.h),
# since the app's native code is built for API 21
LOCAL_LDLIBS := -llog -landroid -ldl

include $(BUILD_SHARED_LIBRARY)
