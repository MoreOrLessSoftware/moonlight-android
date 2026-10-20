#include <jni.h>
#include <android/native_window_jni.h>
#include <vector>

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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeProbePyrowave(JNIEnv*, jclass) {
    return VulkanRenderer::probePyrowave() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeCreate(
        JNIEnv* env, jclass, jobject outputSurface, jint streamWidth, jint streamHeight, jint streamFps,
        jint framePacing, jint jitterBuffer, jint ditherMode, jint colorspace, jboolean fullRange, jboolean tenBit,
        jboolean pyrowave, jboolean pyrowaveCenterChroma, jint pyrowaveLateFrames, jfloat displayRefreshHz,
        jstring codec, jstring decoderName, jstring traceDirectory, jboolean frontBuffer, jboolean pyrowaveGpuBoost) {
    ANativeWindow* output = ANativeWindow_fromSurface(env, outputSurface);
    if (!output) {
        return 0;
    }

    RendererConfig config;
    config.streamWidth = streamWidth;
    config.streamHeight = streamHeight;
    config.streamFps = streamFps;
    config.framePacing = framePacing;
    config.jitterBuffer = jitterBuffer;
    config.ditherMode = ditherMode;
    config.colorspace = colorspace;
    config.fullRange = fullRange;
    config.tenBit = tenBit;
    config.pyrowave = pyrowave;
    config.pyrowaveCenterChroma = pyrowaveCenterChroma;
    config.pyrowaveLateFrames = pyrowaveLateFrames;
    config.displayRefreshHz = displayRefreshHz;
    config.frontBuffer = frontBuffer;
    config.pyrowaveGpuBoost = pyrowaveGpuBoost;
    auto toString = [env](jstring string) {
        std::string value;
        if (string) {
            const char* chars = env->GetStringUTFChars(string, nullptr);
            value = chars;
            env->ReleaseStringUTFChars(string, chars);
        }
        return value;
    };
    config.codec = toString(codec);
    config.decoderName = toString(decoderName);
    config.traceDirectory = toString(traceDirectory);

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

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeSetJitterBuffer(JNIEnv*, jclass, jlong handle, jint jitterBuffer) {
    VulkanRenderer* renderer = fromHandle(handle);
    if (renderer) {
        renderer->setJitterBuffer(static_cast<vkr::JitterBuffer>(jitterBuffer));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeSetVsyncOffset(JNIEnv*, jclass, jlong handle,
                                                                          jlong offsetNs) {
    VulkanRenderer* renderer = fromHandle(handle);
    if (renderer) {
        renderer->setVsyncOffsetNs(offsetNs);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeSubmitPyrowaveFrame(
        JNIEnv* env, jclass, jlong handle, jbyteArray data, jint length, jlong hostPtsUs, jlong lastPacketUs,
        jintArray missingRanges, jintArray recordStartOffsets, jint partialKind) {
    VulkanRenderer* renderer = fromHandle(handle);
    if (!renderer || length <= 0) {
        return JNI_FALSE;
    }

    // Copied out rather than held with critical access, since the decode can wait on the GPU. A
    // frame is at most a few MB, so the copy is cheap next to the decode.
    static thread_local std::vector<uint8_t> frame;
    frame.resize(static_cast<size_t>(length));
    env->GetByteArrayRegion(data, 0, length, reinterpret_cast<jbyte*>(frame.data()));
    if (env->ExceptionCheck()) {
        return JNI_FALSE;
    }
    // Offset and length pairs of the parts of a partial frame that didn't arrive
    static thread_local std::vector<vkr::PyrowaveDecoder::Gap> gaps;
    gaps.clear();
    if (missingRanges) {
        const jsize count = env->GetArrayLength(missingRanges) / 2;
        std::vector<jint> ranges(static_cast<size_t>(count) * 2);
        env->GetIntArrayRegion(missingRanges, 0, count * 2, ranges.data());
        for (jsize i = 0; i < count; i++) {
            if (ranges[i * 2] >= 0 && ranges[i * 2 + 1] > 0) {
                gaps.push_back({static_cast<size_t>(ranges[i * 2]), static_cast<size_t>(ranges[i * 2 + 1])});
            }
        }
    }

    // In record framing, the offsets where records start in it
    static thread_local std::vector<size_t> recordStarts;
    recordStarts.clear();
    if (recordStartOffsets) {
        const jsize count = env->GetArrayLength(recordStartOffsets);
        std::vector<jint> offsets(static_cast<size_t>(count));
        env->GetIntArrayRegion(recordStartOffsets, 0, count, offsets.data());
        for (jint offset : offsets) {
            if (offset >= 0) {
                recordStarts.push_back(static_cast<size_t>(offset));
            }
        }
    }

    return renderer->submitPyrowaveFrame(frame.data(), frame.size(), gaps.data(), gaps.size(),
                                         recordStartOffsets ? &recordStarts : nullptr,
                                         static_cast<vkr::PyrowaveDecoder::Partial>(partialKind), hostPtsUs * 1000,
                                         lastPacketUs)
           ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeNoteReceived(
        JNIEnv*, jclass, jlong handle, jlong hostPtsUs, jlong receiveUs, jlong enqueueUs, jchar hostProcessingLatency) {
    VulkanRenderer* renderer = fromHandle(handle);
    if (renderer) {
        renderer->noteReceived(hostPtsUs * 1000, receiveUs * 1000, enqueueUs * 1000,
                               static_cast<int64_t>(hostProcessingLatency) * 100'000);
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeTakePresentedFrames(JNIEnv*, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    return renderer ? static_cast<jint>(renderer->takePresentedFrames()) : 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeIsFrontBufferUnavailable(JNIEnv*, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    return renderer && renderer->frontBufferUnavailable() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeIsFrontBufferActive(JNIEnv*, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    return renderer && renderer->frontBufferActive() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeTakeCaptureLatency(JNIEnv* env, jclass, jlong handle,
                                                                              jlongArray out) {
    VulkanRenderer* renderer = fromHandle(handle);
    if (!renderer) {
        return;
    }
    const VulkanRenderer::CaptureLatency latency = renderer->takeCaptureLatency();
    const jlong values[6] = {
            latency.decoded.totalNs, latency.decoded.frames, latency.decoded.unsynced,
            latency.presented.totalNs, latency.presented.frames, latency.presented.unsynced,
    };
    env->SetLongArrayRegion(out, 0, 6, values);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeTakeDecodedNs(JNIEnv*, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    return renderer ? static_cast<jlong>(renderer->takeDecodedNs()) : 0;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeGetRendererText(JNIEnv* env, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    return renderer ? env->NewStringUTF(renderer->rendererText().c_str()) : nullptr;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_limelight_binding_video_VulkanRendererBridge_nativeGetPacingText(JNIEnv* env, jclass, jlong handle) {
    VulkanRenderer* renderer = fromHandle(handle);
    return renderer ? env->NewStringUTF(renderer->pacingText().c_str()) : nullptr;
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
