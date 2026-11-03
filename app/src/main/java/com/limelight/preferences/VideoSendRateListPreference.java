package com.limelight.preferences;

import android.content.Context;
import android.util.AttributeSet;

import com.limelight.R;

// How fast to ask Sunshine to send each video frame's packets: the host's own setting,
// common rates, or any in the range Sunshine takes
public class VideoSendRateListPreference extends NumberListPreference {
    public VideoSendRateListPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    public VideoSendRateListPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }

    public VideoSendRateListPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public VideoSendRateListPreference(Context context) {
        super(context);
    }

    @Override
    protected int getEntriesId() {
        return R.array.video_send_rate_names;
    }

    @Override
    protected int getEntryValuesId() {
        return R.array.video_send_rate_values;
    }

    @Override
    protected String getCustomEntryName(String value) {
        return getContext().getString(R.string.video_send_rate_custom_entry, value);
    }

    @Override
    protected int getMinCustomValue() {
        return PreferenceConfiguration.MIN_VIDEO_SEND_RATE_MBPS;
    }

    @Override
    protected int getMaxCustomValue() {
        return PreferenceConfiguration.MAX_VIDEO_SEND_RATE_MBPS;
    }

    @Override
    protected String getSuffix() {
        return getContext().getString(R.string.suffix_seekbar_bitrate_mbps);
    }
}
