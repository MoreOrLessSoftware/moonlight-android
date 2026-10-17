package com.limelight.preferences;

import android.content.Context;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;

import com.limelight.R;
import com.limelight.utils.InputDialogs;

// The frame rate setting: common rates, plus any rate entered with the Custom button
public class FpsListPreference extends CustomValueListPreference {
    public FpsListPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    public FpsListPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }

    public FpsListPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public FpsListPreference(Context context) {
        super(context);
    }

    @Override
    protected int getEntriesId() {
        return R.array.fps_names;
    }

    @Override
    protected int getEntryValuesId() {
        return R.array.fps_values;
    }

    @Override
    protected String getCustomEntryName(String value) {
        return getContext().getString(R.string.fps_custom_entry, value);
    }

    @Override
    protected CharSequence getCustomMessage() {
        return getContext().getString(R.string.seekbar_custom_message, getContext().getString(R.string.fps_suffix_fps));
    }

    @Override
    protected View createCustomInput(String currentValue) {
        EditText input = new EditText(getContext());
        input.setInputType(InputType.TYPE_CLASS_NUMBER);
        if (currentValue != null) {
            input.setText(currentValue);
        }
        input.setSelectAllOnFocus(true);

        return InputDialogs.padInput(getContext(), input);
    }

    @Override
    protected String readCustomInput(View view) {
        EditText input = (EditText) ((ViewGroup) view).getChildAt(0);
        int fps = PreferenceHelp.parsePositiveInt(input.getText().toString());
        if (fps <= 0) {
            input.setError(getContext().getString(R.string.seekbar_custom_invalid));
            return null;
        }
        return String.valueOf(fps);
    }
}
