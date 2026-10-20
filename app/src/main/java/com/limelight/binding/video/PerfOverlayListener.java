package com.limelight.binding.video;

public interface PerfOverlayListener {
    void onPerfUpdate(final String text);

    // Front buffer rendering is on but this device couldn't use it. Called once a stream.
    void onFrontBufferUnavailable();

    // Frames started or stopped being drawn into a front buffer
    void onFrontBufferActive(boolean active);
}
