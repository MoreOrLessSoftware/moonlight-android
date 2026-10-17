package com.limelight.preferences;

import android.app.AlertDialog;
import android.content.Context;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

// Settings show their current value where the help text was, and the help text
// moves into the setting's dialog. These build the pieces for that, and for the
// dialogs that enter a custom value.
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

    // The side padding for a custom value's inputs, so they line up with the dialog's message
    static int getInputPadding(Context context) {
        float density = context.getResources().getDisplayMetrics().density;
        return (int) (20 * density + 0.5f);
    }

    // Shows a custom value's dialog with its first input focused, its text selected
    // so typing replaces it, and the keyboard open. Enter moves to the next input,
    // or on the last one presses OK, which checks the value before closing.
    static void showWithKeyboard(final AlertDialog dialog, View inputs) {
        dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        dialog.show();

        List<EditText> editTexts = new ArrayList<>();
        collectEditTexts(inputs, editTexts);
        if (editTexts.isEmpty()) {
            return;
        }

        for (int i = 0; i < editTexts.size() - 1; i++) {
            editTexts.get(i).setImeOptions(EditorInfo.IME_ACTION_NEXT);
        }
        EditText lastInput = editTexts.get(editTexts.size() - 1);
        lastInput.setImeOptions(EditorInfo.IME_ACTION_DONE);
        lastInput.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int actionId, KeyEvent event) {
                // A hardware keyboard's Enter comes as a key event rather than the Done action
                boolean enterPressed = event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER;
                if (actionId == EditorInfo.IME_ACTION_DONE || enterPressed) {
                    if (event == null || event.getAction() == KeyEvent.ACTION_DOWN) {
                        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
                    }
                    return true;
                }
                return false;
            }
        });

        final EditText input = editTexts.get(0);
        input.requestFocus();
        input.selectAll();

        // Some devices ignore the soft input mode, so ask for the keyboard
        // once the input is attached to the dialog's window too
        input.post(new Runnable() {
            @Override
            public void run() {
                InputMethodManager imm = (InputMethodManager) input.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.showSoftInput(input, InputMethodManager.SHOW_IMPLICIT);
                }
            }
        });
    }

    private static void collectEditTexts(View view, List<EditText> editTexts) {
        if (view instanceof EditText) {
            editTexts.add((EditText) view);
        }
        else if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                collectEditTexts(group.getChildAt(i), editTexts);
            }
        }
    }

    // The number typed into a custom value's input, or 0 if it isn't one
    static int parsePositiveInt(String text) {
        try {
            return Integer.parseInt(text.trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    static boolean hasText(CharSequence text) {
        return text != null && text.length() > 0;
    }
}
