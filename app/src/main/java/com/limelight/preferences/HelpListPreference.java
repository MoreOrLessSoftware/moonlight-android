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

    protected CharSequence getHelp() {
        return help;
    }

    @Override
    public CharSequence getSummary() {
        // Not getEntry(), since while code changes the entries and their values
        // one after the other, the value's index may be past the end of the entries
        int index = findIndexOfValue(getValue());
        CharSequence[] entries = getEntries();
        return entries != null && index >= 0 && index < entries.length ? entries[index] : "";
    }

    @Override
    public void setValue(String value) {
        super.setValue(value);

        // Older Android versions don't refresh the summary themselves
        notifyChanged();
    }

    @Override
    public void setEntries(CharSequence[] entries) {
        super.setEntries(entries);

        // The summary shows the chosen entry's name, which may have changed
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
