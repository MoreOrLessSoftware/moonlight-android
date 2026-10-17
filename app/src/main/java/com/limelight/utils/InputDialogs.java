package com.limelight.utils;

import android.app.AlertDialog;
import android.content.Context;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

// Dialogs for typing in a value, which all look and behave the same: inputs padded
// in line with the dialog's message, the first one focused with its text selected,
// the keyboard open, and Enter moving on or pressing OK
public class InputDialogs {
    public interface TextListener {
        void onText(String text);
    }

    // The side padding for a dialog's inputs, so they line up with its message
    public static int getInputPadding(Context context) {
        float density = context.getResources().getDisplayMetrics().density;
        return (int) (20 * density + 0.5f);
    }

    // Wraps an input in the side padding, for a dialog with a single input
    public static FrameLayout padInput(Context context, View input) {
        int padding = getInputPadding(context);
        FrameLayout layout = new FrameLayout(context);
        layout.setPadding(padding, 0, padding, 0);
        layout.addView(input);
        return layout;
    }

    // Shows a dialog with its first input focused, its text selected so typing
    // replaces it, and the keyboard open. Enter moves to the next input, or on the
    // last one presses OK, which can check the value before closing.
    public static void showWithKeyboard(final AlertDialog dialog, View inputs) {
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

    // Asks for a line of text. OK with nothing typed shows emptyError and keeps the
    // dialog open; Cancel or Back calls nothing.
    public static void showTextInput(Context context, CharSequence title, CharSequence message,
                                     String initialText, final CharSequence emptyError,
                                     final TextListener onOk) {
        final EditText input = new EditText(context);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_WORDS);
        input.setSingleLine(true);
        if (initialText != null) {
            input.setText(initialText);
        }

        final AlertDialog dialog = new AlertDialog.Builder(context)
                .setTitle(title)
                .setMessage(message)
                .setView(padInput(context, input))
                .setPositiveButton(android.R.string.ok, null)
                .setNegativeButton(android.R.string.cancel, null)
                .create();
        showWithKeyboard(dialog, input);

        // Replace the OK button's listener so an empty name doesn't close the dialog
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                String text = input.getText().toString().trim();
                if (text.isEmpty()) {
                    input.setError(emptyError);
                    return;
                }

                dialog.dismiss();
                onOk.onText(text);
            }
        });
    }
}
