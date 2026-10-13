package com.limelight.nvstream.av.video;

public abstract class VideoDecoderRenderer {
    public abstract int setup(int format, int width, int height, int redrawRate);

    public abstract void start();

    public abstract void stop();

    // This is called once for each frame-start NALU. This means it will be called several times
    // for an IDR frame which contains several parameter sets and the I-frame data.
    //
    // partialKind (MoonBridge.PARTIAL_KIND_*) says whether the frame is partial, which it can
    // only be if the renderer asked for partial frames (MoonBridge.CAPABILITY_PARTIAL_FRAMES):
    // it lost packets, or was cut short at its deadline (LiSetPartialFrameDeadline(), which the Vulkan renderer sets).
    // missingRanges then holds offset and length pairs of the gaps in decodeUnitData, whose
    // contents there are undefined, or is null if it has none. Data after the last byte may
    // also be missing. For a partial PyroWave frame in record framing, whose records needn't be in
    // block order, recordStarts holds the offsets in decodeUnitData where the host flagged a
    // record as starting (besides the start of the frame), possibly none. It is null otherwise.
    public abstract int submitDecodeUnit(byte[] decodeUnitData, int decodeUnitLength, int decodeUnitType,
                                         int frameNumber, int frameType, char frameHostProcessingLatency,
                                         long receiveTimeUs, long enqueueTimeUs, long presentationTimeUs,
                                         int[] missingRanges, int[] recordStarts, int partialKind);
    
    public abstract void cleanup();

    public abstract int getCapabilities();

    public abstract void setHdrMode(boolean enabled, byte[] hdrMetadata);
}
