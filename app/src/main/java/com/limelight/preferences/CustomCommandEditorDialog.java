package com.limelight.preferences;

import android.app.AlertDialog;
import android.app.Dialog;
import android.app.DialogFragment;
import android.content.DialogInterface;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.GridLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.limelight.R;
import com.limelight.ui.overlay.CustomCommand;
import com.limelight.ui.overlay.OverlayIcons;

import java.util.UUID;

/**
 * Dialog for adding or editing custom overlay menu commands.
 * Supports icon selection, key combination configuration, and validation.
 */
public class CustomCommandEditorDialog extends DialogFragment {
    private static final String ARG_COMMAND = "command";
    private static final String ARG_IS_EDIT = "is_edit";

    private EditText nameInput;
    private ImageView selectedIconView;
    private CheckBox ctrlCheckbox;
    private CheckBox altCheckbox;
    private CheckBox shiftCheckbox;
    private CheckBox metaCheckbox;
    private Button keyPickerButton;

    private CustomCommand editingCommand;
    private boolean isEditMode;
    private int selectedIconResId = R.drawable.ic_overlay_key_press;
    private int selectedKeyCode = 0;
    private String selectedKeyName = "";

    private OnCommandSavedListener listener;

    public interface OnCommandSavedListener {
        void onCommandSaved(CustomCommand command);
    }

    public static CustomCommandEditorDialog newInstance(CustomCommand command) {
        CustomCommandEditorDialog dialog = new CustomCommandEditorDialog();
        Bundle args = new Bundle();
        try {
            args.putString(ARG_COMMAND, command.toJson().toString());
        } catch (org.json.JSONException e) {
            e.printStackTrace();
        }
        args.putBoolean(ARG_IS_EDIT, true);
        dialog.setArguments(args);
        return dialog;
    }

    public void setOnCommandSavedListener(OnCommandSavedListener listener) {
        this.listener = listener;
    }

    @Override
    public Dialog onCreateDialog(Bundle savedInstanceState) {
        // Check if we're editing an existing command
        if (getArguments() != null) {
            isEditMode = getArguments().getBoolean(ARG_IS_EDIT, false);
            if (isEditMode) {
                String json = getArguments().getString(ARG_COMMAND);
                if (json != null) {
                    try {
                        editingCommand = CustomCommand.fromJson(new org.json.JSONObject(json));
                        selectedIconResId = editingCommand.getIconResId();
                        selectedKeyCode = editingCommand.getKeyCombination().getKeyCode();
                    } catch (org.json.JSONException e) {
                        e.printStackTrace();
                    }
                }
            }
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
        LayoutInflater inflater = getActivity().getLayoutInflater();
        View view = inflater.inflate(R.layout.dialog_custom_command_editor, null);

        // Initialize views
        nameInput = view.findViewById(R.id.command_name_input);
        selectedIconView = view.findViewById(R.id.selected_icon);
        View iconSelector = view.findViewById(R.id.icon_selector);
        ctrlCheckbox = view.findViewById(R.id.modifier_ctrl);
        altCheckbox = view.findViewById(R.id.modifier_alt);
        shiftCheckbox = view.findViewById(R.id.modifier_shift);
        metaCheckbox = view.findViewById(R.id.modifier_meta);
        keyPickerButton = view.findViewById(R.id.key_picker_button);

        // Add listeners to modifier checkboxes to update hint
        CompoundButton.OnCheckedChangeListener modifierChangeListener = new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                updateNameHint();
            }
        };
        ctrlCheckbox.setOnCheckedChangeListener(modifierChangeListener);
        altCheckbox.setOnCheckedChangeListener(modifierChangeListener);
        shiftCheckbox.setOnCheckedChangeListener(modifierChangeListener);
        metaCheckbox.setOnCheckedChangeListener(modifierChangeListener);

        // Setup icon selector click
        iconSelector.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showIconPicker();
            }
        });

        // Setup key picker button
        keyPickerButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showKeyPicker();
            }
        });

        // Populate fields if editing
        if (isEditMode && editingCommand != null) {
            nameInput.setText(editingCommand.getName());
            selectedIconView.setImageResource(editingCommand.getIconResId());

            CustomCommand.KeyCombination keyCombination = editingCommand.getKeyCombination();
            ctrlCheckbox.setChecked(keyCombination.isCtrl());
            altCheckbox.setChecked(keyCombination.isAlt());
            shiftCheckbox.setChecked(keyCombination.isShift());
            metaCheckbox.setChecked(keyCombination.isMeta());

            selectedKeyCode = keyCombination.getKeyCode();
            selectedKeyName = getKeyName(selectedKeyCode);
            keyPickerButton.setText(selectedKeyName);
        } else {
            selectedIconView.setImageResource(selectedIconResId);
            keyPickerButton.setText(R.string.editor_key_code_hint);
        }

        // Update the name hint to show current key combination
        updateNameHint();

        builder.setView(view)
            .setTitle(isEditMode ? R.string.editor_title_edit : R.string.editor_title_add)
            .setPositiveButton(R.string.editor_save, null) // Set to null to override later
            .setNegativeButton(R.string.editor_cancel, new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface dialog, int which) {
                    dismiss();
                }
            });

        return builder.create();
    }

    @Override
    public void onStart() {
        super.onStart();

        // Override positive button to prevent auto-dismiss on validation failure
        AlertDialog dialog = (AlertDialog) getDialog();
        if (dialog != null) {
            Button positiveButton = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            positiveButton.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    if (validateAndSave()) {
                        dismiss();
                    }
                }
            });
        }
    }

    /**
     * Validate inputs and save the command
     */
    private boolean validateAndSave() {
        String name = nameInput.getText().toString().trim();

        // Name is optional - if empty, key combo will be shown instead

        // Validate key selection
        if (selectedKeyCode == 0) {
            Toast.makeText(getActivity(), R.string.editor_error_key_empty,
                Toast.LENGTH_SHORT).show();
            return false;
        }

        // Create key combination
        CustomCommand.KeyCombination keyCombination = new CustomCommand.KeyCombination(
            ctrlCheckbox.isChecked(),
            altCheckbox.isChecked(),
            shiftCheckbox.isChecked(),
            metaCheckbox.isChecked(),
            selectedKeyCode
        );

        // Create or update command
        String id = isEditMode ? editingCommand.getId() : UUID.randomUUID().toString();
        CustomCommand command = new CustomCommand(id, name, selectedIconResId, keyCombination);

        // Notify listener
        if (listener != null) {
            listener.onCommandSaved(command);
        }

        return true;
    }

    /**
     * Show icon picker dialog
     */
    private void showIconPicker() {
        AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
        builder.setTitle(R.string.editor_icon_select);

        // Create grid of icons
        GridLayout gridLayout = new GridLayout(getActivity());
        gridLayout.setColumnCount(8);
        int padding = (int) (16 * getResources().getDisplayMetrics().density);
        gridLayout.setPadding(padding, padding, padding, padding);

        // Wrap grid in a ScrollView for Android TV compatibility
        ScrollView scrollView = new ScrollView(getActivity());
        scrollView.addView(gridLayout);

        // Set max height to 60% of screen height to prevent overflow on TV
        int maxHeight = (int) (getResources().getDisplayMetrics().heightPixels * 0.6);
        scrollView.setLayoutParams(new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            Math.min(LinearLayout.LayoutParams.WRAP_CONTENT, maxHeight)
        ));

        for (final OverlayIcons.IconOption icon : OverlayIcons.AVAILABLE_ICONS) {
            ImageView iconView = new ImageView(getActivity());
            iconView.setImageResource(icon.resourceId);
            iconView.setPadding(padding / 2, padding / 2, padding / 2, padding / 2);

            // Enable focus for Android TV D-pad navigation
            iconView.setFocusable(true);
            iconView.setFocusableInTouchMode(true);
            iconView.setBackgroundResource(R.drawable.icon_picker_selector);

            GridLayout.LayoutParams params = new GridLayout.LayoutParams();
            params.width = (int) (56 * getResources().getDisplayMetrics().density);
            params.height = (int) (56 * getResources().getDisplayMetrics().density);
            iconView.setLayoutParams(params);

            iconView.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    selectedIconResId = icon.resourceId;
                    selectedIconView.setImageResource(selectedIconResId);
                    // Dismiss the icon picker dialog
                    ((AlertDialog) v.getTag()).dismiss();
                }
            });

            gridLayout.addView(iconView);
        }

        AlertDialog iconDialog = builder.setView(scrollView)
            .setNegativeButton(R.string.editor_cancel, null)
            .create();

        // Store dialog reference in each icon view's tag
        for (int i = 0; i < gridLayout.getChildCount(); i++) {
            gridLayout.getChildAt(i).setTag(iconDialog);
        }

        iconDialog.show();
    }

    /**
     * Show key picker dialog
     */
    private void showKeyPicker() {
        AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
        builder.setTitle("Select Key");

        // Common keys
        final String[] keyNames = {
            "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
            "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
            "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
            "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
            "Win/Cmd", "Space", "Enter", "Tab", "Escape", "Backspace", "Delete",
            "Left", "Right", "Up", "Down", "Home", "End", "Page Up", "Page Down"
        };

        final int[] keyCodes = {
            KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_B, KeyEvent.KEYCODE_C, KeyEvent.KEYCODE_D,
            KeyEvent.KEYCODE_E, KeyEvent.KEYCODE_F, KeyEvent.KEYCODE_G, KeyEvent.KEYCODE_H,
            KeyEvent.KEYCODE_I, KeyEvent.KEYCODE_J, KeyEvent.KEYCODE_K, KeyEvent.KEYCODE_L,
            KeyEvent.KEYCODE_M, KeyEvent.KEYCODE_N, KeyEvent.KEYCODE_O, KeyEvent.KEYCODE_P,
            KeyEvent.KEYCODE_Q, KeyEvent.KEYCODE_R, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_T,
            KeyEvent.KEYCODE_U, KeyEvent.KEYCODE_V, KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_X,
            KeyEvent.KEYCODE_Y, KeyEvent.KEYCODE_Z,
            KeyEvent.KEYCODE_0, KeyEvent.KEYCODE_1, KeyEvent.KEYCODE_2, KeyEvent.KEYCODE_3,
            KeyEvent.KEYCODE_4, KeyEvent.KEYCODE_5, KeyEvent.KEYCODE_6, KeyEvent.KEYCODE_7,
            KeyEvent.KEYCODE_8, KeyEvent.KEYCODE_9,
            KeyEvent.KEYCODE_F1, KeyEvent.KEYCODE_F2, KeyEvent.KEYCODE_F3, KeyEvent.KEYCODE_F4,
            KeyEvent.KEYCODE_F5, KeyEvent.KEYCODE_F6, KeyEvent.KEYCODE_F7, KeyEvent.KEYCODE_F8,
            KeyEvent.KEYCODE_F9, KeyEvent.KEYCODE_F10, KeyEvent.KEYCODE_F11, KeyEvent.KEYCODE_F12,
            KeyEvent.KEYCODE_META_LEFT, KeyEvent.KEYCODE_SPACE, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_TAB,
            KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_DEL, KeyEvent.KEYCODE_FORWARD_DEL,
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_MOVE_HOME, KeyEvent.KEYCODE_MOVE_END,
            KeyEvent.KEYCODE_PAGE_UP, KeyEvent.KEYCODE_PAGE_DOWN
        };

        builder.setItems(keyNames, new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                selectedKeyCode = keyCodes[which];
                selectedKeyName = keyNames[which];
                keyPickerButton.setText(selectedKeyName);
                updateNameHint();
            }
        });

        builder.setNegativeButton(R.string.editor_cancel, null);
        builder.show();
    }

    /**
     * Get display name for a key code
     */
    private String getKeyName(int keyCode) {
        // Map some common keys to friendly names
        switch (keyCode) {
            case KeyEvent.KEYCODE_META_LEFT: return "Win/Cmd";
            case KeyEvent.KEYCODE_SPACE: return "Space";
            case KeyEvent.KEYCODE_ENTER: return "Enter";
            case KeyEvent.KEYCODE_TAB: return "Tab";
            case KeyEvent.KEYCODE_ESCAPE: return "Escape";
            case KeyEvent.KEYCODE_DEL: return "Backspace";
            case KeyEvent.KEYCODE_FORWARD_DEL: return "Delete";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "Left";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return "Right";
            case KeyEvent.KEYCODE_DPAD_UP: return "Up";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "Down";
            case KeyEvent.KEYCODE_MOVE_HOME: return "Home";
            case KeyEvent.KEYCODE_MOVE_END: return "End";
            case KeyEvent.KEYCODE_PAGE_UP: return "Page Up";
            case KeyEvent.KEYCODE_PAGE_DOWN: return "Page Down";
            default:
                // For letter/number keys, use KeyEvent.keyCodeToString
                String keyName = KeyEvent.keyCodeToString(keyCode);
                if (keyName.startsWith("KEYCODE_")) {
                    return keyName.substring(8); // Remove "KEYCODE_" prefix
                }
                return keyName;
        }
    }

    /**
     * Update the name input hint to show the current key combination
     */
    private void updateNameHint() {
        if (selectedKeyCode == 0) {
            // No key selected, use default hint
            nameInput.setHint(R.string.editor_command_name_hint);
            return;
        }

        // Build the key combination display string
        StringBuilder hintBuilder = new StringBuilder();

        if (ctrlCheckbox.isChecked()) hintBuilder.append("Ctrl+");
        if (altCheckbox.isChecked()) hintBuilder.append("Alt+");
        if (shiftCheckbox.isChecked()) hintBuilder.append("Shift+");
        if (metaCheckbox.isChecked()) hintBuilder.append("Win+");

        // Add the key name
        hintBuilder.append(selectedKeyName);

        nameInput.setHint(hintBuilder.toString());
    }
}
