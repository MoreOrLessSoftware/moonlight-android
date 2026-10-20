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
     * ABI, and the GPU can run it (Vulkan 1.1 and the extensions PyroWave needs). Tried once, by
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
     * @param pyrowaveCenterChroma the PyroWave host sites 4:2:0 chroma at the center (the nonary
     *                             host) rather than with the left luma sample
     * @param pyrowaveLateFrames one of PreferenceConfiguration.PYROWAVE_LATE_FRAMES_*
     * @param codec h264, hevc, av1 or pyrowave, and decoderName the MediaCodec decoder (null for
     *              PyroWave), both recorded in pacer traces
     * @param traceDirectory where frame pacing traces are written when switched on with
     *                       {@code adb shell setprop debug.moonlight.pacer_trace 1}, or null
     * @param frontBuffer draw into a front buffer the display reads directly, timed to each scan,
     *                    where the device has one
     * @param pyrowaveGpuBoost for PyroWave, keep the GPU busy between decodes so its clock stays up
     */
    public static VulkanRendererBridge create(Surface output, int streamWidth, int streamHeight, int streamFps,
                                              int framePacing, int jitterBuffer, int ditherMode, int colorspace,
                                              boolean fullRange, boolean tenBit, boolean pyrowave,
                                              boolean pyrowaveCenterChroma, int pyrowaveLateFrames,
                                              float displayRefreshHz, String codec, String decoderName,
                                              String traceDirectory, boolean frontBuffer, boolean pyrowaveGpuBoost) {
        if (!isSupported() || (pyrowave && !isPyrowaveSupported())) {
            return null;
        }

        long handle = nativeCreate(output, streamWidth, streamHeight, streamFps, framePacing, jitterBuffer, ditherMode,
                colorspace, fullRange, tenBit, pyrowave, pyrowaveCenterChroma, pyrowaveLateFrames, displayRefreshHz,
                codec, decoderName, traceDirectory, frontBuffer, pyrowaveGpuBoost);
        if (handle == 0) {
            return null;
        }

        VulkanRendererBridge bridge = new VulkanRendererBridge(handle);
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
     * Decodes a PyroWave frame and queues it to be shown.
     *
     * @param hostPtsUs the host's timestamp for the frame
     * @param lastPacketUs when its last packet arrived (its enqueue time), on moonlight-common-c's
     *                     clock. Frames still arriving at their deadline are cut short from it.
     * @param missingRanges for a partial frame, offset and length pairs of the gaps in it (see
     *                      VideoDecoderRenderer.submitDecodeUnit()), or null
     * @param recordStarts for a partial frame in record framing, offsets where records start in
     *                     it (possibly none), or null
     * @param partialKind MoonBridge.PARTIAL_KIND_*
     * @return false if the frame couldn't be decoded
     */
    public boolean submitPyrowaveFrame(byte[] frame, int length, long hostPtsUs, long lastPacketUs,
                                       int[] missingRanges, int[] recordStarts, int partialKind) {
        return handle != 0 && nativeSubmitPyrowaveFrame(handle, frame, length, hostPtsUs, lastPacketUs,
                missingRanges, recordStarts, partialKind);
    }

    /** Surface for the decoder to render into (null for PyroWave) */
    public Surface getDecoderSurface() {
        return decoderSurface;
    }

    /**
     * Notes a frame's network timing as it's handed to the decoder, for the capture latency
     * statistics and the frame pacing trace, if one is being recorded. All times in
     * microseconds; receive and enqueue times are moonlight-common-c's. hostProcessingLatency is
     * the host's, in tenths of a millisecond (0 if the host doesn't report it).
     */
    public void noteFrameReceived(long hostPtsUs, long receiveTimeUs, long enqueueTimeUs, char hostProcessingLatency) {
        if (handle != 0) {
            nativeNoteReceived(handle, hostPtsUs, receiveTimeUs, enqueueTimeUs, hostProcessingLatency);
        }
    }

    /**
     * Switches the jitter buffer for the rest of the stream.
     * @param jitterBuffer one of PreferenceConfiguration.JITTER_BUFFER_*
     */
    public void setJitterBuffer(int jitterBuffer) {
        if (handle != 0) {
            nativeSetJitterBuffer(handle, jitterBuffer);
        }
    }

    /**
     * How long after the display's vsync Choreographer's frame times are
     * ({@link android.view.Display#getAppVsyncOffsetNanos()}), which changes with the display mode
     */
    public void setVsyncOffset(long offsetNs) {
        if (handle != 0) {
            nativeSetVsyncOffset(handle, offsetNs);
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

    /**
     * Whether a front buffer was asked for but the device couldn't give one, so frames go
     * through the compositor's queue as usual. Known once the first frame is drawn.
     */
    public boolean isFrontBufferUnavailable() {
        return handle != 0 && nativeIsFrontBufferUnavailable(handle);
    }

    /** Whether frames are drawn into a front buffer. Known once the first frame is drawn. */
    public boolean isFrontBufferActive() {
        return handle != 0 && nativeIsFrontBufferActive(handle);
    }

    /**
     * Total PyroWave decode time since the last call, in nanoseconds, leaving out waits: each
     * frame's CPU work, then its run on the GPU. Where the GPU can't time it, from handing the
     * frame to the renderer until the GPU was done with it.
     */
    public long takeDecodedNs() {
        return handle != 0 ? nativeTakeDecodedNs(handle) : 0;
    }

    /**
     * Capture latency since the last call, into out: the total time in nanoseconds from frames'
     * capture until they were decoded, how many frames that counts and how many of them were
     * measured without clock sync (and so without the network transit), then the same three
     * for until they reached the screen.
     */
    public void takeCaptureLatency(long[] out) {
        if (handle != 0) {
            nativeTakeCaptureLatency(handle, out);
        }
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
                                            boolean pyrowaveCenterChroma, int pyrowaveLateFrames,
                                            float displayRefreshHz, String codec, String decoderName,
                                            String traceDirectory, boolean frontBuffer, boolean pyrowaveGpuBoost);
    private static native boolean nativeSubmitPyrowaveFrame(long handle, byte[] frame, int length, long hostPtsUs,
                                                            long lastPacketUs, int[] missingRanges,
                                                            int[] recordStarts, int partialKind);
    private static native Surface nativeGetDecoderSurface(long handle);
    private static native void nativeNoteReceived(long handle, long hostPtsUs, long receiveTimeUs, long enqueueTimeUs,
                                                  char hostProcessingLatency);
    private static native void nativeSetHdrMode(long handle, boolean enabled, byte[] hdrMetadata);
    private static native void nativeSetJitterBuffer(long handle, int jitterBuffer);
    private static native void nativeSetVsyncOffset(long handle, long offsetNs);
    private static native int nativeTakePresentedFrames(long handle);
    private static native boolean nativeIsFrontBufferUnavailable(long handle);
    private static native boolean nativeIsFrontBufferActive(long handle);
    private static native long nativeTakeDecodedNs(long handle);
    private static native void nativeTakeCaptureLatency(long handle, long[] out);
    private static native String nativeGetRendererText(long handle);
    private static native String nativeGetPacingText(long handle);
    private static native void nativeStop(long handle);
    private static native void nativeDestroy(long handle);
}
