#include <jni.h>
#include <android/native_window_jni.h>

#include "vulkan_renderer.h"

using vkr::RendererConfig;
using vkr::VulkanRenderer;

namespace {
    VulkanRenderer* fromHandle(jlong handle) {
        return reinterpret_cast<VulkanRenderer*>(handle);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeProbe(JNIEnv*, jclass) {
    return VulkanRenderer::probe() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeCreate(
        JNIEnv* env, jclass, jobject outputSurface, jint streamWidth, jint streamHeight, jint streamFps,
        jint framePacing, jint ditherMode, jint colorspace, jboolean fullRange, jboolean tenBit,
        jfloat displayRefreshHz, jstring traceDirectory) {
    ANativeWindow* output = ANativeWindow_fromSurface(env, outputSurface);
    if (!output) {
        return 0;
    }

    RendererConfig config;
    config.streamWidth = streamWidth;
    config.streamHeight = streamHeight;
    config.streamFps = streamFps;
    config.framePacing = framePacing;
    config.ditherMode = ditherMode;
    config.colorspace = colorspace;
    config.fullRange = fullRange;
    config.tenBit = tenBit;
    config.displayRefreshHz = displayRefreshHz;
    if (traceDirectory) {
        const char* chars = env->GetStringUTFChars(traceDirectory, nullptr);
        config.traceDirectory = chars;
        env->ReleaseStringUTFChars(traceDirectory, chars);
    }

    std::unique_ptr<VulkanRenderer> renderer = VulkanRenderer::create(output, config);

    // The renderer holds its own reference
    ANativeWindow_release(output);
    return reinterpret_cast<jlong>(renderer.release());
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeGetDecoderSurface(JNIEnv* env, jclass, jlong handle) {
    const NdkApi* ndk = loadNdkApi();
    VulkanRenderer* renderer = fromHandle(handle);
    if (!ndk || !renderer || !renderer->decoderWindow()) {
        return nullptr;
    }
    return ndk->ANativeWindow_toSurface(env, renderer->decoderWindow());
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeSetHdrMode(
        JNIEnv* env, jclass, jlong handle, jboolean enabled, jbyteArray metadata) {
    VulkanRenderer* renderer = fromHandle(handle);
    if (!renderer) {
        return;
    }

    if (metadata) {
        jsize length = env->GetArrayLength(metadata);
        jbyte* bytes = env->GetByteArrayElements(metadata, nullptr);
        renderer->setHdrMode(enabled, reinterpret_cast<const uint8_t*>(bytes), static_cast<size_t>(length));
        env->ReleaseByteArrayElements(metadata, bytes, JNI_ABORT);
    }
    else {
        renderer->setHdrMode(enabled, nullptr, 0);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeIsTracing(JNIEnv*, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    return renderer && renderer->tracing() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeNoteReceived(
        JNIEnv*, jclass, jlong handle, jlong hostPtsUs, jlong receiveUs, jlong enqueueUs) {
    VulkanRenderer* renderer = fromHandle(handle);
    if (renderer) {
        renderer->noteReceived(hostPtsUs * 1000, receiveUs * 1000, enqueueUs * 1000);
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeTakePresentedFrames(JNIEnv*, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    return renderer ? static_cast<jint>(renderer->takePresentedFrames()) : 0;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeGetStatsText(JNIEnv* env, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    return renderer ? env->NewStringUTF(renderer->statsText().c_str()) : nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeStop(JNIEnv*, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    if (renderer) {
        renderer->stop();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    delete fromHandle(handle);
}
