package com.limelight.binding.video;

import android.os.SystemClock;

class VideoStats {

    long decoderTimeMs;
    long totalTimeMs;
    int totalFrames;
    int totalFramesReceived;
    int totalFramesRendered;
    int frameLossEvents;
    int framesLost;
    int framesPartial;
    char minHostProcessingLatency;
    char maxHostProcessingLatency;
    int totalHostProcessingLatency;
    int framesWithHostProcessingLatency;
    // From capture until decoded, and until presented. Frames measured without clock sync are
    // counted apart, since their time leaves out the network transit.
    long totalCaptureToDecodedUs;
    int framesWithCaptureToDecoded;
    int decodedWithoutNetworkTransit;
    long totalCaptureToPresentUs;
    int framesWithCaptureToPresent;
    int presentedWithoutNetworkTransit;
    long measurementStartTimestamp;

    void add(VideoStats other) {
        this.decoderTimeMs += other.decoderTimeMs;
        this.totalTimeMs += other.totalTimeMs;
        this.totalFrames += other.totalFrames;
        this.totalFramesReceived += other.totalFramesReceived;
        this.totalFramesRendered += other.totalFramesRendered;
        this.frameLossEvents += other.frameLossEvents;
        this.framesLost += other.framesLost;
        this.framesPartial += other.framesPartial;

        if (this.minHostProcessingLatency == 0) {
            this.minHostProcessingLatency = other.minHostProcessingLatency;
        } else {
            this.minHostProcessingLatency = (char) Math.min(this.minHostProcessingLatency, other.minHostProcessingLatency);
        }
        this.maxHostProcessingLatency = (char) Math.max(this.maxHostProcessingLatency, other.maxHostProcessingLatency);
        this.totalHostProcessingLatency += other.totalHostProcessingLatency;
        this.framesWithHostProcessingLatency += other.framesWithHostProcessingLatency;
        this.totalCaptureToDecodedUs += other.totalCaptureToDecodedUs;
        this.framesWithCaptureToDecoded += other.framesWithCaptureToDecoded;
        this.decodedWithoutNetworkTransit += other.decodedWithoutNetworkTransit;
        this.totalCaptureToPresentUs += other.totalCaptureToPresentUs;
        this.framesWithCaptureToPresent += other.framesWithCaptureToPresent;
        this.presentedWithoutNetworkTransit += other.presentedWithoutNetworkTransit;

        if (this.measurementStartTimestamp == 0) {
            this.measurementStartTimestamp = other.measurementStartTimestamp;
        }

        assert other.measurementStartTimestamp >= this.measurementStartTimestamp;
    }

    void copy(VideoStats other) {
        this.decoderTimeMs = other.decoderTimeMs;
        this.totalTimeMs = other.totalTimeMs;
        this.totalFrames = other.totalFrames;
        this.totalFramesReceived = other.totalFramesReceived;
        this.totalFramesRendered = other.totalFramesRendered;
        this.frameLossEvents = other.frameLossEvents;
        this.framesLost = other.framesLost;
        this.framesPartial = other.framesPartial;
        this.minHostProcessingLatency = other.minHostProcessingLatency;
        this.maxHostProcessingLatency = other.maxHostProcessingLatency;
        this.totalHostProcessingLatency = other.totalHostProcessingLatency;
        this.framesWithHostProcessingLatency = other.framesWithHostProcessingLatency;
        this.totalCaptureToDecodedUs = other.totalCaptureToDecodedUs;
        this.framesWithCaptureToDecoded = other.framesWithCaptureToDecoded;
        this.decodedWithoutNetworkTransit = other.decodedWithoutNetworkTransit;
        this.totalCaptureToPresentUs = other.totalCaptureToPresentUs;
        this.framesWithCaptureToPresent = other.framesWithCaptureToPresent;
        this.presentedWithoutNetworkTransit = other.presentedWithoutNetworkTransit;
        this.measurementStartTimestamp = other.measurementStartTimestamp;
    }

    void clear() {
        this.decoderTimeMs = 0;
        this.totalTimeMs = 0;
        this.totalFrames = 0;
        this.totalFramesReceived = 0;
        this.totalFramesRendered = 0;
        this.frameLossEvents = 0;
        this.framesLost = 0;
        this.framesPartial = 0;
        this.minHostProcessingLatency = 0;
        this.maxHostProcessingLatency = 0;
        this.totalHostProcessingLatency = 0;
        this.framesWithHostProcessingLatency = 0;
        this.totalCaptureToDecodedUs = 0;
        this.framesWithCaptureToDecoded = 0;
        this.decodedWithoutNetworkTransit = 0;
        this.totalCaptureToPresentUs = 0;
        this.framesWithCaptureToPresent = 0;
        this.presentedWithoutNetworkTransit = 0;
        this.measurementStartTimestamp = 0;
    }

    VideoStatsFps getFps() {
        float elapsed = (SystemClock.uptimeMillis() - this.measurementStartTimestamp) / (float) 1000;

        VideoStatsFps fps = new VideoStatsFps();
        if (elapsed > 0) {
            fps.totalFps = this.totalFrames / elapsed;
            fps.receivedFps = this.totalFramesReceived / elapsed;
            fps.renderedFps = this.totalFramesRendered / elapsed;
        }
        return fps;
    }
}

class VideoStatsFps {

    float totalFps;
    float receivedFps;
    float renderedFps;
}