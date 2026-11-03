package com.limelight.preferences;

import android.content.Context;
import android.util.AttributeSet;

import com.limelight.R;

// The percentage of error correction packets to ask Sunshine to add to each video frame:
// the host's own setting, off, common percentages, or any up to what Sunshine takes
public class FecListPreference extends NumberListPreference {
    public FecListPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    public FecListPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }

    public FecListPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public FecListPreference(Context context) {
        super(context);
    }

    @Override
    protected int getEntriesId() {
        return R.array.fec_percent_names;
    }

    @Override
    protected int getEntryValuesId() {
        return R.array.fec_percent_values;
    }

    @Override
    protected String getCustomEntryName(String value) {
        return getContext().getString(R.string.fec_percent_custom_entry, value);
    }

    @Override
    protected int getMinCustomValue() {
        return 1;
    }

    @Override
    protected int getMaxCustomValue() {
        return PreferenceConfiguration.MAX_FEC_PERCENT;
    }

    @Override
    protected String getSuffix() {
        return "%";
    }
}
