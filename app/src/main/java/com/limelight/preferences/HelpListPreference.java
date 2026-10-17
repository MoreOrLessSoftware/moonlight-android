package com.limelight.preferences;

import android.app.AlertDialog;
import android.content.Context;
import android.preference.ListPreference;
import android.util.AttributeSet;

// A list setting that shows its chosen entry as the summary. The summary from
// the XML is help text, shown under the title of the dialog that picks a value.
public class HelpListPreference extends ListPreference {
    private CharSequence help;

    public HelpListPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        takeHelpFromSummary();
    }

    public HelpListPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        takeHelpFromSummary();
    }

    public HelpListPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
        takeHelpFromSummary();
    }

    public HelpListPreference(Context context) {
        super(context);
        takeHelpFromSummary();
    }

    private void takeHelpFromSummary() {
        help = super.getSummary();
    }

    @Override
    public CharSequence getSummary() {
        CharSequence entry = getEntry();
        return entry != null ? entry : "";
    }

    @Override
    public void setValue(String value) {
        super.setValue(value);

        // Older Android versions don't refresh the summary themselves
        notifyChanged();
    }

    @Override
    protected void onPrepareDialogBuilder(AlertDialog.Builder builder) {
        super.onPrepareDialogBuilder(builder);

        // A list dialog can't also show a message, so the help goes under the title
        if (PreferenceHelp.hasText(help)) {
            builder.setCustomTitle(PreferenceHelp.createDialogTitle(getContext(), getDialogTitle(), help));
        }
    }
}
