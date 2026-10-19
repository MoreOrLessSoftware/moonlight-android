package com.limelight.binding.video;

public interface PerfOverlayListener {
    void onPerfUpdate(final String text);

    // Front buffer rendering is on but this device couldn't use it. Called once a stream.
    void onFrontBufferUnavailable();
}
