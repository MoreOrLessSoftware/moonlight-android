package com.limelight.binding.video;

import android.view.Surface;

import com.limelight.LimeLog;

/**
 * Java side of the native Vulkan video renderer (app/src/main/jni/vulkan).
 *
 * The decoder renders into {@link #getDecoderSurface()}. Native code imports each decoded
 * frame into Vulkan without a copy, converts and dithers it, and presents it on the output
 * surface at the vsync its frame pacing mode picks.
 *
 * For PyroWave streams there is no decoder surface: whole frames go to
 * {@link #submitPyrowaveFrame}, and the renderer decodes them itself.
 */
public class VulkanRendererBridge {
    private static Boolean supported;
    private static Boolean pyrowaveSupported;

    private long handle;
    private Surface decoderSurface;
    private boolean tracing;

    private VulkanRendererBridge(long handle) {
        this.handle = handle;
    }

    /**
     * Whether this device can run the renderer. Needs Android 10 for the NDK APIs it uses,
     * plus Vulkan 1.1 with AHardwareBuffer import and YCbCr sampling.
     */
    public static synchronized boolean isSupported() {
        if (supported == null) {
            boolean result = false;
            try {
                System.loadLibrary("vulkan_renderer");
                result = nativeProbe();
            } catch (UnsatisfiedLinkError e) {
                LimeLog.warning("Vulkan renderer library unavailable: " + e.getMessage());
            }
            LimeLog.info("Vulkan renderer supported: " + result);
            supported = result;
        }
        return supported;
    }

    /**
     * Whether the renderer can decode PyroWave here: libpyrowave-shared.so is packaged for this
     * ABI, and the GPU can run it (Vulkan 1.3 and the features PyroWave needs). Tried once, by
     * making a decoder, which takes a moment.
     */
    public static synchronized boolean isPyrowaveSupported() {
        if (pyrowaveSupported == null) {
            boolean result = isSupported() && nativeProbePyrowave();
            LimeLog.info("PyroWave decoding supported: " + result);
            pyrowaveSupported = result;
        }
        return pyrowaveSupported;
    }

    /**
     * Starts a renderer on the output surface, or returns null if it can't run there.
     *
     * @param framePacing one of PreferenceConfiguration.FRAME_PACING_*
     * @param jitterBuffer one of PreferenceConfiguration.JITTER_BUFFER_*
     * @param ditherMode 0 = off, 1 = low, 2 = high
     * @param colorspace one of MoonBridge.COLORSPACE_*
     * @param pyrowave the stream is PyroWave, which the renderer decodes itself
     * @param traceDirectory where frame pacing traces are written when switched on with
     *                       {@code adb shell setprop debug.moonlight.pacer_trace 1}, or null
     */
    public static VulkanRendererBridge create(Surface output, int streamWidth, int streamHeight, int streamFps,
                                              int framePacing, int jitterBuffer, int ditherMode, int colorspace,
                                              boolean fullRange, boolean tenBit, boolean pyrowave,
                                              float displayRefreshHz, String traceDirectory) {
        if (!isSupported() || (pyrowave && !isPyrowaveSupported())) {
            return null;
        }

        long handle = nativeCreate(output, streamWidth, streamHeight, streamFps, framePacing, jitterBuffer, ditherMode,
                colorspace, fullRange, tenBit, pyrowave, displayRefreshHz, traceDirectory);
        if (handle == 0) {
            return null;
        }

        VulkanRendererBridge bridge = new VulkanRendererBridge(handle);
        bridge.tracing = nativeIsTracing(handle);
        if (!pyrowave) {
            bridge.decoderSurface = nativeGetDecoderSurface(handle);
            if (bridge.decoderSurface == null) {
                bridge.destroy();
                return null;
            }
        }
        return bridge;
    }

    /**
     * Decodes a whole PyroWave frame and queues it to be shown.
     *
     * @param hostPtsUs the host's timestamp for the frame
     * @return false if the frame couldn't be decoded
     */
    public boolean submitPyrowaveFrame(byte[] frame, int length, long hostPtsUs) {
        return handle != 0 && nativeSubmitPyrowaveFrame(handle, frame, length, hostPtsUs);
    }

    /** Surface for the decoder to render into (null for PyroWave) */
    public Surface getDecoderSurface() {
        return decoderSurface;
    }

    /**
     * Records a frame's network timing in the frame pacing trace, if one is being recorded.
     * All times in microseconds; receive and enqueue times are moonlight-common-c's.
     */
    public void noteFrameReceived(long hostPtsUs, long receiveTimeUs, long enqueueTimeUs) {
        if (tracing && handle != 0) {
            nativeNoteReceived(handle, hostPtsUs, receiveTimeUs, enqueueTimeUs);
        }
    }

    public void setHdrMode(boolean enabled, byte[] hdrMetadata) {
        if (handle != 0) {
            nativeSetHdrMode(handle, enabled, hdrMetadata);
        }
    }

    /** Frames presented since the last call */
    public int takePresentedFrames() {
        return handle != 0 ? nativeTakePresentedFrames(handle) : 0;
    }

    /** One line for the performance overlay */
    public String getRendererText() {
        return handle != 0 ? nativeGetRendererText(handle) : null;
    }

    /**
     * Pacing numbers for the performance overlay: a headline and details, separated by a
     * newline. Either may be empty.
     */
    public String getPacingText() {
        return handle != 0 ? nativeGetPacingText(handle) : null;
    }

    /**
     * Stops presenting and lets go of the output surface. Must be called before the output
     * surface is destroyed.
     */
    public void stop() {
        if (handle != 0) {
            nativeStop(handle);
        }
    }

    /** Frees the renderer. The decoder must be released first. */
    public void destroy() {
        if (decoderSurface != null) {
            decoderSurface.release();
            decoderSurface = null;
        }
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    private static native boolean nativeProbe();
    private static native boolean nativeProbePyrowave();
    private static native long nativeCreate(Surface output, int streamWidth, int streamHeight, int streamFps,
                                            int framePacing, int jitterBuffer, int ditherMode, int colorspace,
                                            boolean fullRange, boolean tenBit, boolean pyrowave,
                                            float displayRefreshHz, String traceDirectory);
    private static native boolean nativeSubmitPyrowaveFrame(long handle, byte[] frame, int length, long hostPtsUs);
    private static native Surface nativeGetDecoderSurface(long handle);
    private static native boolean nativeIsTracing(long handle);
    private static native void nativeNoteReceived(long handle, long hostPtsUs, long receiveTimeUs, long enqueueTimeUs);
    private static native void nativeSetHdrMode(long handle, boolean enabled, byte[] hdrMetadata);
    private static native int nativeTakePresentedFrames(long handle);
    private static native String nativeGetRendererText(long handle);
    private static native String nativeGetPacingText(long handle);
    private static native void nativeStop(long handle);
    private static native void nativeDestroy(long handle);
}
