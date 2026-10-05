package com.limelight.preferences;

import android.app.AlertDialog;
import android.content.Context;
import android.os.Bundle;
import android.preference.DialogPreference;
import android.util.AttributeSet;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.View.OnClickListener;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import com.limelight.R;

import java.util.Locale;

// Based on a Stack Overflow example: http://stackoverflow.com/questions/1974193/slider-on-my-preferencescreen
public class SeekBarPreference extends DialogPreference
{
    private static final String ANDROID_SCHEMA_URL = "http://schemas.android.com/apk/res/android";
    private static final String SEEKBAR_SCHEMA_URL = "http://schemas.moonlight-stream.com/apk/res/seekbar";

    private SeekBar seekBar;
    private TextView valueText;
    private final Context context;

    private final String dialogMessage;
    private final String suffix;
    private final int defaultValue;
    private final int maxValue;
    private final int minValue;
    private final int stepSize;
    private final int keyStepSize;
    private final int divisor;
    private final boolean allowCustom;
    private int currentValue;
    private boolean sliderTouched;

    public SeekBarPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
        this.context = context;

        // Read the message from XML
        int dialogMessageId = attrs.getAttributeResourceValue(ANDROID_SCHEMA_URL, "dialogMessage", 0);
        if (dialogMessageId == 0) {
            dialogMessage = attrs.getAttributeValue(ANDROID_SCHEMA_URL, "dialogMessage");
        }
        else {
            dialogMessage = context.getString(dialogMessageId);
        }

        // Get the suffix for the number displayed in the dialog
        int suffixId = attrs.getAttributeResourceValue(ANDROID_SCHEMA_URL, "text", 0);
        if (suffixId == 0) {
            suffix = attrs.getAttributeValue(ANDROID_SCHEMA_URL, "text");
        }
        else {
            suffix = context.getString(suffixId);
        }

        // Get default, min, and max seekbar values
        defaultValue = attrs.getAttributeIntValue(ANDROID_SCHEMA_URL, "defaultValue", PreferenceConfiguration.getDefaultBitrate(context));
        maxValue = attrs.getAttributeIntValue(ANDROID_SCHEMA_URL, "max", 100);
        minValue = attrs.getAttributeIntValue(SEEKBAR_SCHEMA_URL, "min", 1);
        stepSize = attrs.getAttributeIntValue(SEEKBAR_SCHEMA_URL, "step", 1);
        divisor = attrs.getAttributeIntValue(SEEKBAR_SCHEMA_URL, "divisor", 1);
        keyStepSize = attrs.getAttributeIntValue(SEEKBAR_SCHEMA_URL, "keyStep", 0);
        allowCustom = attrs.getAttributeBooleanValue(SEEKBAR_SCHEMA_URL, "custom", false);
    }

    @Override
    protected View onCreateDialogView() {
        sliderTouched = false;

        LinearLayout.LayoutParams params;
        LinearLayout layout = new LinearLayout(context);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(6, 6, 6, 6);

        TextView splashText = new TextView(context);
        splashText.setPadding(30, 10, 30, 10);
        if (dialogMessage != null) {
            splashText.setText(dialogMessage);
        }
        layout.addView(splashText);

        valueText = new TextView(context);
        valueText.setGravity(Gravity.CENTER_HORIZONTAL);
        valueText.setTextSize(32);
        // Default text for value; hides bug where OnSeekBarChangeListener isn't called when opacity is 0%
        valueText.setText("0%");
        params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        layout.addView(valueText, params);

        seekBar = new SeekBar(context);
        seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int value, boolean b) {
                if (value < minValue) {
                    seekBar.setProgress(minValue);
                    return;
                }

                if (b) {
                    sliderTouched = true;
                }

                int roundedValue = ((value + (stepSize - 1))/stepSize)*stepSize;
                if (roundedValue != value) {
                    seekBar.setProgress(roundedValue);
                    return;
                }

                // A custom value above the slider's max is clamped by the SeekBar; show the real value
                if (!sliderTouched && currentValue > maxValue) {
                    roundedValue = currentValue;
                    value = currentValue;
                }

                String t;
                if (divisor != 1) {
                    float floatValue = roundedValue / (float)divisor;
                    t = String.format((Locale)null, "%.1f", floatValue);
                }
                else {
                    t = String.valueOf(value);
                }
                valueText.setText(suffix == null ? t : t.concat(suffix.length() > 1 ? " "+suffix : suffix));
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        layout.addView(seekBar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        if (shouldPersist()) {
            currentValue = getPersistedInt(defaultValue);
        }

        seekBar.setMax(maxValue);
        if (keyStepSize != 0) {
            seekBar.setKeyProgressIncrement(keyStepSize);
        }
        seekBar.setProgress(currentValue);

        return layout;
    }

    @Override
    protected void onBindDialogView(View v) {
        super.onBindDialogView(v);
        seekBar.setMax(maxValue);
        if (keyStepSize != 0) {
            seekBar.setKeyProgressIncrement(keyStepSize);
        }
        seekBar.setProgress(currentValue);
    }

    @Override
    protected void onSetInitialValue(boolean restore, Object defaultValue)
    {
        super.onSetInitialValue(restore, defaultValue);
        if (restore) {
            currentValue = shouldPersist() ? getPersistedInt(this.defaultValue) : 0;
        }
        else {
            currentValue = (Integer) defaultValue;
        }
    }

    public void setProgress(int progress) {
        this.currentValue = progress;
        if (seekBar != null) {
            seekBar.setProgress(progress);
        }
    }
    public int getProgress() {
        return currentValue;
    }

    @Override
    protected void onPrepareDialogBuilder(AlertDialog.Builder builder) {
        super.onPrepareDialogBuilder(builder);
        if (allowCustom) {
            // Listener is replaced in showDialog() so the click doesn't auto-dismiss
            builder.setNeutralButton(R.string.seekbar_custom, null);
        }
    }

    private void showCustomValueDialog() {
        final EditText input = new EditText(context);
        input.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        int shownValue = (!sliderTouched && currentValue > maxValue) ? currentValue : seekBar.getProgress();
        input.setText(String.format((Locale)null, "%.1f", shownValue / (float)divisor));
        input.setSelectAllOnFocus(true);

        final AlertDialog dialog = new AlertDialog.Builder(context)
                .setTitle(getTitle())
                .setMessage(suffix == null ? null : context.getString(R.string.seekbar_custom_message, suffix))
                .setView(input)
                .setPositiveButton(android.R.string.ok, null)
                .setNegativeButton(android.R.string.cancel, null)
                .create();
        dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
        dialog.show();

        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View view) {
                float entered;
                try {
                    entered = Float.parseFloat(input.getText().toString().trim());
                } catch (NumberFormatException e) {
                    entered = 0;
                }

                int value = Math.round(entered * divisor);
                if (value <= 0) {
                    input.setError(context.getString(R.string.seekbar_custom_invalid));
                    return;
                }

                currentValue = value;
                if (shouldPersist()) {
                    persistInt(value);
                    callChangeListener(value);
                }
                dialog.dismiss();
                if (getDialog() != null) {
                    getDialog().dismiss();
                }
            }
        });
    }

    @Override
    public void showDialog(Bundle state) {
        super.showDialog(state);

        if (allowCustom) {
            ((AlertDialog) getDialog()).getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(new OnClickListener() {
                @Override
                public void onClick(View view) {
                    showCustomValueDialog();
                }
            });
        }

        Button positiveButton = ((AlertDialog) getDialog()).getButton(AlertDialog.BUTTON_POSITIVE);
        positiveButton.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View view) {
                // Keep an out-of-range custom value unless the slider was actually moved
                boolean keepCustom = !sliderTouched && currentValue > maxValue;
                if (shouldPersist() && !keepCustom) {
                    currentValue = seekBar.getProgress();
                    persistInt(currentValue);
                    callChangeListener(currentValue);
                }

                getDialog().dismiss();
            }
        });
    }
}
