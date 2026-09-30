package com.limelight.nvstream.av.video;

public abstract class VideoDecoderRenderer {
    public abstract int setup(int format, int width, int height, int redrawRate);

    public abstract void start();

    public abstract void stop();

    // This is called once for each frame-start NALU. This means it will be called several times
    // for an IDR frame which contains several parameter sets and the I-frame data.
    //
    // missingRanges is null unless the frame lost packets and the renderer asked for partial
    // frames (MoonBridge.CAPABILITY_PARTIAL_FRAMES): then it holds offset and length pairs of the
    // gaps in decodeUnitData, whose contents there are undefined. Data after the last byte may
    // also be missing.
    public abstract int submitDecodeUnit(byte[] decodeUnitData, int decodeUnitLength, int decodeUnitType,
                                         int frameNumber, int frameType, char frameHostProcessingLatency,
                                         long receiveTimeUs, long enqueueTimeUs, long presentationTimeUs,
                                         int[] missingRanges);
    
    public abstract void cleanup();

    public abstract int getCapabilities();

    public abstract void setHdrMode(boolean enabled, byte[] hdrMetadata);
}
