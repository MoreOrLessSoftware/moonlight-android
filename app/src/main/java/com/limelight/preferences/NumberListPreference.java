package com.limelight.preferences;

import android.content.Context;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;

import com.limelight.R;
import com.limelight.utils.InputDialogs;

// A whole-number setting picked from common values, or any value in a range entered with
// the Custom button. Values are stored as strings, as for any list setting.
public abstract class NumberListPreference extends CustomValueListPreference {
    public NumberListPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    public NumberListPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }

    public NumberListPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public NumberListPreference(Context context) {
        super(context);
    }

    // The range a custom value may be in
    protected abstract int getMinCustomValue();
    protected abstract int getMaxCustomValue();

    // The unit the value is entered in, for the custom value's dialog
    protected abstract String getSuffix();

    @Override
    protected CharSequence getCustomMessage() {
        return getContext().getString(R.string.seekbar_custom_message, getSuffix());
    }

    @Override
    protected View createCustomInput(String currentValue) {
        EditText input = new EditText(getContext());
        input.setInputType(InputType.TYPE_CLASS_NUMBER);
        // Start from the current value only when it's one that can be entered here
        int current = currentValue != null ? PreferenceHelp.parsePositiveInt(currentValue) : 0;
        if (current >= getMinCustomValue() && current <= getMaxCustomValue()) {
            input.setText(String.valueOf(current));
        }
        input.setSelectAllOnFocus(true);

        return InputDialogs.padInput(getContext(), input);
    }

    @Override
    protected String readCustomInput(View view) {
        EditText input = (EditText) ((ViewGroup) view).getChildAt(0);
        int value = PreferenceHelp.parsePositiveInt(input.getText().toString());
        if (value < getMinCustomValue() || value > getMaxCustomValue()) {
            input.setError(getContext().getString(R.string.custom_value_out_of_range,
                    getMinCustomValue(), getMaxCustomValue()));
            return null;
        }
        return String.valueOf(value);
    }
}
