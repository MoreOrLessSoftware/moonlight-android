package com.limelight.preferences;

import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.util.AttributeSet;
import android.view.View;

import com.limelight.R;

import java.util.Arrays;

// A list setting with a Custom button in its dialog for entering a value that isn't
// in the list. A custom value is shown as its own entry, so it shows as the summary.
public abstract class CustomValueListPreference extends HelpListPreference {
    // The value of the entry added for a custom value, if there is one
    private String customEntryValue;

    // The name of a first entry, with an empty value, for not setting this at all
    private CharSequence defaultEntryName;

    // Whether tapping this goes straight to entering a value, without the list
    private boolean customOnly;

    public CustomValueListPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    public CustomValueListPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }

    public CustomValueListPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public CustomValueListPreference(Context context) {
        super(context);
    }

    // The standard entries' names and values
    protected abstract int getEntriesId();
    protected abstract int getEntryValuesId();

    // The name of the entry for a custom value
    protected abstract String getCustomEntryName(String value);

    // Builds the dialog's view for entering a custom value, starting from the current value
    protected abstract View createCustomInput(String currentValue);

    // Reads the value from the view, or shows an error on it and returns null
    protected abstract String readCustomInput(View input);

    protected abstract CharSequence getCustomMessage();

    @Override
    public void setValue(String value) {
        super.setValue(value);
        addEntryForCustomValue();
    }

    // Adds a first entry with an empty value, for a setting that can be left unset
    // to use a default from elsewhere, like an app's setting using the global one
    public void setDefaultEntry(CharSequence name) {
        defaultEntryName = name;
        resetEntries();
        addEntryForCustomValue();
    }

    // Skips the list, so tapping this goes straight to entering a value. With a default
    // entry, the dialog's Clear button goes back to it.
    public void setCustomOnly(boolean customOnly) {
        this.customOnly = customOnly;
    }

    @Override
    protected void onClick() {
        if (customOnly) {
            showCustomDialog();
        }
        else {
            super.onClick();
        }
    }

    // Goes back to the standard entries, before entries for this device are added
    public void resetEntries() {
        customEntryValue = null;

        CharSequence[] entries = getContext().getResources().getTextArray(getEntriesId());
        CharSequence[] values = getContext().getResources().getTextArray(getEntryValuesId());
        if (defaultEntryName != null) {
            entries = prepend(defaultEntryName, entries);
            values = prepend("", values);
        }

        // Values first, so the summary never looks up an entry past the end
        setEntryValues(values);
        setEntries(entries);
    }

    private static CharSequence[] prepend(CharSequence first, CharSequence[] array) {
        CharSequence[] result = new CharSequence[array.length + 1];
        result[0] = first;
        System.arraycopy(array, 0, result, 1, array.length);
        return result;
    }

    // Adds an entry for the current value if none of the entries has it, replacing
    // the entry for an earlier custom value. Call this after changing the entries.
    public void addEntryForCustomValue() {
        CharSequence[] entries = getEntries();
        CharSequence[] values = getEntryValues();
        String value = getValue();
        if (entries == null || values == null || value == null) {
            return;
        }

        // Drop the entry for an earlier custom value
        int oldIndex = customEntryValue != null ? findIndexOfValue(customEntryValue) : -1;
        if (oldIndex >= 0 && !customEntryValue.equals(value)) {
            entries = remove(entries, oldIndex);
            values = remove(values, oldIndex);
            customEntryValue = null;
        }

        if (indexOf(values, value) < 0) {
            entries = Arrays.copyOf(entries, entries.length + 1);
            values = Arrays.copyOf(values, values.length + 1);
            entries[entries.length - 1] = getCustomEntryName(value);
            values[values.length - 1] = value;
            customEntryValue = value;
        }

        // Values first, so the summary never looks up an entry past the end
        setEntryValues(values);
        setEntries(entries);
    }

    private static CharSequence[] remove(CharSequence[] array, int index) {
        CharSequence[] result = new CharSequence[array.length - 1];
        System.arraycopy(array, 0, result, 0, index);
        System.arraycopy(array, index + 1, result, index, array.length - index - 1);
        return result;
    }

    private static int indexOf(CharSequence[] values, String value) {
        for (int i = 0; i < values.length; i++) {
            if (value.equals(values[i].toString())) {
                return i;
            }
        }
        return -1;
    }

    @Override
    protected void onPrepareDialogBuilder(AlertDialog.Builder builder) {
        super.onPrepareDialogBuilder(builder);

        builder.setNeutralButton(R.string.seekbar_custom, new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                showCustomDialog();
            }
        });
    }

    private void showCustomDialog() {
        final View input = createCustomInput(getValue());

        AlertDialog.Builder builder = new AlertDialog.Builder(getContext())
                .setMessage(getCustomMessage())
                .setView(input)
                .setPositiveButton(android.R.string.ok, null)
                .setNegativeButton(android.R.string.cancel, null);

        if (customOnly) {
            // Without the list, its help goes here
            if (PreferenceHelp.hasText(getHelp())) {
                builder.setCustomTitle(PreferenceHelp.createDialogTitle(getContext(), getDialogTitle(), getHelp()));
            }
            else {
                builder.setTitle(getDialogTitle());
            }

            // Without the list, this is the way back to the default
            if (defaultEntryName != null) {
                builder.setNeutralButton(R.string.custom_value_clear, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        if (callChangeListener("")) {
                            setValue("");
                        }
                    }
                });
            }
        }
        else {
            builder.setTitle(getTitle());
        }

        final AlertDialog dialog = builder.create();
        PreferenceHelp.showWithKeyboard(dialog, input);

        // Replace the OK button's listener so an invalid value doesn't close the dialog
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                String value = readCustomInput(input);
                if (value == null) {
                    return;
                }

                if (callChangeListener(value)) {
                    setValue(value);
                }
                dialog.dismiss();
            }
        });
    }
}
