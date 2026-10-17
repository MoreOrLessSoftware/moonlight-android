package com.limelight.preferences;

import android.content.Context;
import android.widget.LinearLayout;
import android.widget.TextView;

// Settings show their current value where the help text was, and the help text
// moves into the setting's dialog. These build the pieces for that.
class PreferenceHelp {
    // A dialog title with the setting's help text below it, for dialogs like lists
    // that can't show a message alongside their content
    static LinearLayout createDialogTitle(Context context, CharSequence title, CharSequence help) {
        float density = context.getResources().getDisplayMetrics().density;
        int horizontalPadding = (int) (24 * density + 0.5f);
        int topPadding = (int) (20 * density + 0.5f);
        int bottomPadding = (int) (8 * density + 0.5f);

        LinearLayout layout = new LinearLayout(context);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(horizontalPadding, topPadding, horizontalPadding, bottomPadding);

        TextView titleView = new TextView(context);
        titleView.setTextAppearance(context, android.R.style.TextAppearance_Material_DialogWindowTitle);
        titleView.setText(title);
        layout.addView(titleView);

        TextView helpView = new TextView(context);
        helpView.setTextAppearance(context, android.R.style.TextAppearance_Material_Body1);
        helpView.setTextColor(context.getResources().getColor(android.R.color.secondary_text_dark));
        helpView.setPadding(0, (int) (6 * density + 0.5f), 0, 0);
        helpView.setText(help);
        layout.addView(helpView);

        return layout;
    }

    static boolean hasText(CharSequence text) {
        return text != null && text.length() > 0;
    }
}
