package com.limelight.preferences;

import android.content.Context;
import android.preference.EditTextPreference;
import android.util.AttributeSet;

import com.limelight.R;
import com.limelight.utils.UiHelper;

// The actual display refresh rate setting, which shows its value as the summary. The summary
// from the XML is help text, shown as the message of the dialog that edits it.
public class RefreshRatePreference extends EditTextPreference {
    public RefreshRatePreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        takeHelpFromSummary();
    }

    public RefreshRatePreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        takeHelpFromSummary();
    }

    public RefreshRatePreference(Context context, AttributeSet attrs) {
        super(context, attrs);
        takeHelpFromSummary();
    }

    public RefreshRatePreference(Context context) {
        super(context);
        takeHelpFromSummary();
    }

    private void takeHelpFromSummary() {
        if (getDialogMessage() == null) {
            setDialogMessage(super.getSummary());
        }
    }

    @Override
    public CharSequence getSummary() {
        String text = getText();
        double rate = 0;
        try {
            rate = text != null ? Double.parseDouble(text) : 0;
        } catch (NumberFormatException ignored) {
        }

        // 0 means the rate is measured when the stream starts
        return rate > 0 ? UiHelper.formatRefreshRate(rate) : getContext().getString(R.string.summary_refresh_rate_measured);
    }

    @Override
    public void setText(String text) {
        super.setText(text);
        notifyChanged();
    }
}
