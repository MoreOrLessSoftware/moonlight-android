package com.limelight.preferences;

import android.content.Context;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;

import com.limelight.R;

// The resolution setting: common resolutions, plus any size entered with the Custom button
public class ResolutionListPreference extends CustomValueListPreference {
    // The same limits as the per-app custom resolution
    private static final int MAX_WIDTH = 7680;
    private static final int MAX_HEIGHT = 4320;

    public ResolutionListPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    public ResolutionListPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }

    public ResolutionListPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public ResolutionListPreference(Context context) {
        super(context);
    }

    @Override
    protected int getEntriesId() {
        return R.array.resolution_names;
    }

    @Override
    protected int getEntryValuesId() {
        return R.array.resolution_values;
    }

    @Override
    protected String getCustomEntryName(String value) {
        return getContext().getString(R.string.resolution_custom_entry, value);
    }

    @Override
    protected CharSequence getCustomMessage() {
        return getContext().getString(R.string.resolution_custom_message);
    }

    @Override
    protected View createCustomInput(String currentValue) {
        Context context = getContext();
        int padding = PreferenceHelp.getInputPadding(context);

        LinearLayout layout = new LinearLayout(context);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(padding, 0, padding, 0);

        EditText widthInput = new EditText(context);
        widthInput.setHint(R.string.resolution_custom_width);
        widthInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        widthInput.setSelectAllOnFocus(true);

        EditText heightInput = new EditText(context);
        heightInput.setHint(R.string.resolution_custom_height);
        heightInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        heightInput.setSelectAllOnFocus(true);

        if (currentValue != null) {
            String[] parts = currentValue.split("x");
            if (parts.length == 2) {
                widthInput.setText(parts[0]);
                heightInput.setText(parts[1]);
            }
        }

        layout.addView(widthInput);
        layout.addView(heightInput);
        return layout;
    }

    @Override
    protected String readCustomInput(View view) {
        LinearLayout layout = (LinearLayout) view;
        EditText widthInput = (EditText) layout.getChildAt(0);
        EditText heightInput = (EditText) layout.getChildAt(1);

        int width = PreferenceHelp.parsePositiveInt(widthInput.getText().toString());
        int height = PreferenceHelp.parsePositiveInt(heightInput.getText().toString());

        boolean valid = true;
        if (width <= 0 || width > MAX_WIDTH) {
            widthInput.setError(getContext().getString(R.string.resolution_custom_invalid, MAX_WIDTH));
            valid = false;
        }
        if (height <= 0 || height > MAX_HEIGHT) {
            heightInput.setError(getContext().getString(R.string.resolution_custom_invalid, MAX_HEIGHT));
            valid = false;
        }

        return valid ? width + "x" + height : null;
    }
}
