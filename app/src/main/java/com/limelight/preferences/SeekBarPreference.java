package com.limelight.preferences;

import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.res.TypedArray;
import android.os.Bundle;
import android.preference.DialogPreference;
import android.util.AttributeSet;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.View.OnClickListener;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import com.limelight.R;
import com.limelight.utils.InputDialogs;

import java.util.Locale;

// Based on a Stack Overflow example: http://stackoverflow.com/questions/1974193/slider-on-my-preferencescreen
public class SeekBarPreference extends DialogPreference
{
    // Works out the recommended bitrate, for a setting whose resolution and frame rate
    // aren't the global ones
    public interface RecommendedBitrateProvider {
        int getRecommendedBitrate();
    }

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
    private final boolean showRecommendedBitrate;
    private RecommendedBitrateProvider recommendedBitrateProvider;
    // When set, a value of 0 means this isn't set, and shows as this
    private CharSequence unsetName;
    // Whether tapping this goes straight to entering a value, without the slider
    private boolean customOnly;
    private int currentValue;
    // The value the dialog's slider started at, which may be past its end
    private int dialogStartValue;
    private boolean sliderTouched;

    public SeekBarPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
        this.context = context;

        // Read the message from XML. The summary shows the current value instead of
        // help text, so without a message the dialog shows the summary from XML.
        int dialogMessageId = attrs.getAttributeResourceValue(ANDROID_SCHEMA_URL, "dialogMessage", 0);
        if (dialogMessageId != 0) {
            dialogMessage = context.getString(dialogMessageId);
        }
        else if (attrs.getAttributeValue(ANDROID_SCHEMA_URL, "dialogMessage") != null) {
            dialogMessage = attrs.getAttributeValue(ANDROID_SCHEMA_URL, "dialogMessage");
        }
        else {
            CharSequence xmlSummary = super.getSummary();
            dialogMessage = xmlSummary != null ? xmlSummary.toString() : null;
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
        // The bitrate has no fixed default; 0 means the recommendation for the current settings
        defaultValue = attrs.getAttributeIntValue(ANDROID_SCHEMA_URL, "defaultValue", 0);
        maxValue = attrs.getAttributeIntValue(ANDROID_SCHEMA_URL, "max", 100);
        minValue = attrs.getAttributeIntValue(SEEKBAR_SCHEMA_URL, "min", 1);
        stepSize = attrs.getAttributeIntValue(SEEKBAR_SCHEMA_URL, "step", 1);
        divisor = attrs.getAttributeIntValue(SEEKBAR_SCHEMA_URL, "divisor", 1);
        keyStepSize = attrs.getAttributeIntValue(SEEKBAR_SCHEMA_URL, "keyStep", 0);
        allowCustom = attrs.getAttributeBooleanValue(SEEKBAR_SCHEMA_URL, "custom", false);
        showRecommendedBitrate = attrs.getAttributeBooleanValue(SEEKBAR_SCHEMA_URL, "recommendedBitrate", false);
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
                if (!sliderTouched && dialogStartValue > maxValue) {
                    roundedValue = dialogStartValue;
                    value = dialogStartValue;
                }

                valueText.setText(formatValue(roundedValue));
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        layout.addView(seekBar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        // The recommended bitrate and unsetting go under the slider, since the dialog's
        // buttons are taken by OK, Cancel and Custom. The recommendation may be past
        // the slider's end.
        if (showRecommendedBitrate) {
            // For the current resolution, frame rate and codec
            final int recommended = getRecommendedBitrate();
            Button recommendedButton = new Button(context, null, android.R.attr.borderlessButtonStyle);
            recommendedButton.setText(context.getString(R.string.seekbar_recommended, formatValue(recommended)));
            recommendedButton.setOnClickListener(new OnClickListener() {
                @Override
                public void onClick(View view) {
                    saveValueAndClose(recommended);
                }
            });
            addCenteredButton(layout, recommendedButton);
        }

        if (unsetName != null) {
            Button unsetButton = new Button(context, null, android.R.attr.borderlessButtonStyle);
            unsetButton.setText(unsetName);
            unsetButton.setOnClickListener(new OnClickListener() {
                @Override
                public void onClick(View view) {
                    saveValueAndClose(0);
                }
            });
            addCenteredButton(layout, unsetButton);
        }

        if (shouldPersist()) {
            currentValue = getPersistedInt(defaultValue);
        }

        // An unset value starts the slider at the recommendation
        dialogStartValue = currentValue == 0 && showRecommendedBitrate ?
                getRecommendedBitrate() : currentValue;

        seekBar.setMax(maxValue);
        if (keyStepSize != 0) {
            seekBar.setKeyProgressIncrement(keyStepSize);
        }
        seekBar.setProgress(dialogStartValue);

        return layout;
    }

    private static void addCenteredButton(LinearLayout layout, Button button) {
        LinearLayout.LayoutParams buttonParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        buttonParams.gravity = Gravity.CENTER_HORIZONTAL;
        layout.addView(button, buttonParams);
    }

    public void setRecommendedBitrateProvider(RecommendedBitrateProvider provider) {
        recommendedBitrateProvider = provider;
        notifyChanged();
    }

    // Lets this be unset, with a value of 0, shown as the given name
    public void setUnsetName(CharSequence name) {
        unsetName = name;
        notifyChanged();
    }

    // Skips the slider, so tapping this goes straight to entering a value. If this can be
    // unset, the dialog's Clear button unsets it.
    public void setCustomOnly(boolean customOnly) {
        this.customOnly = customOnly;
    }

    @Override
    protected void onClick() {
        if (customOnly) {
            showCustomValueDialog();
        }
        else {
            super.onClick();
        }
    }

    private int getRecommendedBitrate() {
        return recommendedBitrateProvider != null ?
                recommendedBitrateProvider.getRecommendedBitrate() :
                PreferenceConfiguration.getDefaultBitrate(context);
    }

    @Override
    protected void onBindDialogView(View v) {
        super.onBindDialogView(v);
        seekBar.setMax(maxValue);
        if (keyStepSize != 0) {
            seekBar.setKeyProgressIncrement(keyStepSize);
        }
        seekBar.setProgress(dialogStartValue);
    }

    // Without this, the default from XML is never passed to onSetInitialValue(), so a value
    // that was never saved shows as 0. This runs in the superclass constructor, before
    // this class's fields are set.
    @Override
    protected Object onGetDefaultValue(TypedArray a, int index) {
        return a.getInt(index, 0);
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

    private String formatValue(int value) {
        String t;
        if (divisor != 1 && value % divisor != 0) {
            // Only a value saved before values became whole numbers has a fraction
            t = String.format((Locale)null, "%.1f", value / (float)divisor);
        }
        else {
            t = String.valueOf(value / divisor);
        }
        return suffix == null ? t : t.concat(suffix.length() > 1 ? " "+suffix : suffix);
    }

    @Override
    public CharSequence getSummary() {
        if (currentValue == 0 && unsetName != null) {
            return unsetName;
        }
        // An unset bitrate streams at the recommendation, which follows the other settings
        if (currentValue == 0 && showRecommendedBitrate) {
            return formatValue(getRecommendedBitrate());
        }
        return formatValue(currentValue);
    }

    // Says what bitrate is recommended when the current one is lower. The settings list
    // redraws its rows whenever any setting changes, so this follows changes to the
    // resolution, frame rate and codec too.
    @Override
    public CharSequence getTitle() {
        CharSequence title = super.getTitle();

        // The superclass constructor asks for the title before this class is set up.
        // An unset value isn't compared, since it's whatever the default is.
        if (showRecommendedBitrate && title != null && currentValue > 0) {
            int recommended = getRecommendedBitrate();
            if (currentValue < recommended) {
                return context.getString(R.string.seekbar_title_recommended, title, formatValue(recommended));
            }
        }
        return title;
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

    // Saves a value, which may be past the slider's end, and closes the dialog. Settings
    // that aren't persisted are saved elsewhere by their change listener.
    private void saveValueAndClose(int value) {
        currentValue = value;
        if (shouldPersist()) {
            persistInt(value);
        }
        callChangeListener(value);
        notifyChanged();
        if (getDialog() != null) {
            getDialog().dismiss();
        }
    }

    private void showCustomValueDialog() {
        final EditText input = new EditText(context);
        input.setInputType(InputType.TYPE_CLASS_NUMBER);

        // Start from the slider's value when it's open, otherwise the saved value,
        // or the recommendation if this is unset
        int shownValue;
        if (getDialog() != null && getDialog().isShowing()) {
            shownValue = (!sliderTouched && dialogStartValue > maxValue) ? dialogStartValue : seekBar.getProgress();
        }
        else if (currentValue == 0 && showRecommendedBitrate) {
            shownValue = getRecommendedBitrate();
        }
        else {
            shownValue = currentValue;
        }
        input.setText(String.valueOf(Math.round(shownValue / (float)divisor)));
        input.setSelectAllOnFocus(true);


        AlertDialog.Builder builder = new AlertDialog.Builder(context)
                .setMessage(suffix == null ? null : context.getString(R.string.seekbar_custom_message, suffix))
                .setView(InputDialogs.padInput(context, input))
                .setPositiveButton(android.R.string.ok, null)
                .setNegativeButton(android.R.string.cancel, null);

        if (customOnly) {
            // Without the slider's dialog, its help goes here
            if (dialogMessage != null) {
                builder.setCustomTitle(PreferenceHelp.createDialogTitle(context, getDialogTitle(), dialogMessage));
            }
            else {
                builder.setTitle(getDialogTitle());
            }

            // Without the slider's dialog, this is the way back to unset
            if (unsetName != null) {
                builder.setNeutralButton(R.string.custom_value_clear, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        saveValueAndClose(0);
                    }
                });
            }
        }
        else {
            builder.setTitle(getDialogTitle());
        }

        final AlertDialog dialog = builder.create();
        InputDialogs.showWithKeyboard(dialog, input);

        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View view) {
                int value = PreferenceHelp.parsePositiveInt(input.getText().toString()) * divisor;
                if (value <= 0) {
                    input.setError(context.getString(R.string.seekbar_custom_invalid));
                    return;
                }

                dialog.dismiss();
                saveValueAndClose(value);
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
                // Keep an out-of-range starting value unless the slider was actually moved
                boolean keepStart = !sliderTouched && dialogStartValue > maxValue;
                saveValueAndClose(keepStart ? dialogStartValue : seekBar.getProgress());
            }
        });
    }
}
