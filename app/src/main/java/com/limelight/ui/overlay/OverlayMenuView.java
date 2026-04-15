package com.limelight.ui.overlay;

import android.app.UiModeManager;
import android.content.Context;
import android.content.res.Configuration;
import android.os.Build;
import android.util.AttributeSet;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;

import com.limelight.R;

import java.util.ArrayList;
import java.util.List;

/**
 * Overlay menu view that displays action buttons in a horizontal layout.
 * Supports gamepad navigation and custom commands.
 */
public class OverlayMenuView extends HorizontalScrollView {
    private static final int BUTTON_SPACING_DP = 16;
    private static final float ANALOG_STICK_THRESHOLD = 0.5f;
    private static final long ANALOG_NAV_THROTTLE_MS = 200;

    /**
     * Interface for menu action callbacks
     */
    public interface MenuActionListener {
        void onDisconnect();
        void onQuitSession();
        void onToggleStats();
        void onToggleMouseEmulation();
        void onShowKeyboard();
        void onSendGuideButton();
        void onCustomCommand(CustomCommand command);
        void onMenuClosed();
    }

    private LinearLayout buttonContainer;
    private List<OverlayMenuButton> buttons;
    private int selectedIndex = 0;
    private MenuActionListener actionListener;
    private CustomCommandsManager commandsManager;

    // Action types for built-in buttons
    private static final int ACTION_DISCONNECT = 0;
    private static final int ACTION_QUIT = 1;
    private static final int ACTION_TOGGLE_STATS = 2;
    private static final int ACTION_CLOSE = 3;
    private static final int ACTION_SHOW_KEYBOARD = 4;
    private static final int ACTION_TOGGLE_MOUSE_EMULATION = 5;
    private static final int ACTION_SEND_GUIDE = 6;
    private static final int ACTION_CUSTOM_BASE = 100;

    private List<Integer> buttonActions;
    private long lastAnalogNavTime = 0;
    private boolean flipFaceButtons = false;
    private boolean isAndroidTV;

    public OverlayMenuView(Context context) {
        super(context);
        init(context);
    }

    public OverlayMenuView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context);
    }

    public OverlayMenuView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init(context);
    }

    private void init(Context context) {
        setHorizontalScrollBarEnabled(false);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setBackgroundDrawable(null);

        // Disable default focus highlight that causes white background
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            setDefaultFocusHighlightEnabled(false);
        }

        // Create button container
        buttonContainer = new LinearLayout(context);
        buttonContainer.setOrientation(LinearLayout.HORIZONTAL);
        buttonContainer.setBackgroundDrawable(null);

        addView(buttonContainer);

        buttons = new ArrayList<>();
        buttonActions = new ArrayList<>();
        commandsManager = new CustomCommandsManager(context);

        // Cache Android TV status (device type never changes at runtime)
        isAndroidTV = isAndroidTV();

        setVisibility(GONE);
    }

    /**
     * Check if the device is Android TV
     */
    private boolean isAndroidTV() {
        UiModeManager uiModeManager = (UiModeManager) getContext().getSystemService(Context.UI_MODE_SERVICE);
        return uiModeManager.getCurrentModeType() == Configuration.UI_MODE_TYPE_TELEVISION;
    }

    /**
     * Build the menu with built-in and custom buttons
     */
    public void buildMenu() {
        // Reset state to prevent issues when menu is rebuilt
        selectedIndex = 0;
        clearFocus();

        buttonContainer.removeAllViews();
        buttons.clear();
        buttonActions.clear();

        float density = getContext().getResources().getDisplayMetrics().density;
        int spacing = (int) (BUTTON_SPACING_DP * density);

        // Add built-in buttons
        addBuiltInButton(R.drawable.ic_overlay_monitor,
            getContext().getString(R.string.overlay_menu_disconnect), ACTION_DISCONNECT, spacing);
        addBuiltInButton(R.drawable.ic_overlay_power,
            getContext().getString(R.string.overlay_menu_quit_session), ACTION_QUIT, spacing);
        addBuiltInButton(R.drawable.ic_overlay_perf,
            getContext().getString(R.string.overlay_menu_toggle_stats), ACTION_TOGGLE_STATS, spacing);
        addBuiltInButton(R.drawable.ic_overlay_mouse,
            getContext().getString(R.string.overlay_menu_mouse_emulation), ACTION_TOGGLE_MOUSE_EMULATION, spacing);
        // Only show keyboard button on non-TV devices where it works reliably
        if (!isAndroidTV) {
            addBuiltInButton(R.drawable.ic_overlay_keyboard_toggle,
                getContext().getString(R.string.overlay_menu_keyboard), ACTION_SHOW_KEYBOARD, spacing);
        }
        addBuiltInButton(R.drawable.ic_overlay_guide,
            getContext().getString(R.string.overlay_menu_guide), ACTION_SEND_GUIDE, spacing);

        // Add custom command buttons
        List<CustomCommand> customCommands = commandsManager.getCommands();
        for (int i = 0; i < customCommands.size(); i++) {
            CustomCommand command = customCommands.get(i);
            OverlayMenuButton button = OverlayMenuButton.create(
                getContext(),
                command.getIconResId(),
                command.getName()
            );

            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            );
            if (i < customCommands.size() - 1 || buttonContainer.getChildCount() > 0) {
                params.rightMargin = spacing;
            }
            buttonContainer.addView(button, params);

            buttons.add(button);
            buttonActions.add(ACTION_CUSTOM_BASE + i);

            final int index = buttons.size() - 1;
            final CustomCommand cmd = command;
            button.setOnClickListener(v -> selectAndActivate(index));

            // Track focus changes to update selection state
            button.setOnFocusChangeListener((v, hasFocus) -> {
                if (hasFocus) {
                    selectedIndex = index;
                    button.setSelected(true);
                    // HorizontalScrollView automatically scrolls to keep focused items visible
                } else {
                    button.setSelected(false);
                }
            });
        }

        // Add close button at the end (no right margin since it's last)
        addBuiltInButton(R.drawable.ic_overlay_close,
            getContext().getString(R.string.overlay_menu_close), ACTION_CLOSE, 0);

        // Force layout refresh to ensure view hierarchy is properly updated
        buttonContainer.invalidate();
        buttonContainer.requestLayout();
    }

    private void addBuiltInButton(int iconResId, String label, int action, int spacing) {
        OverlayMenuButton button = OverlayMenuButton.create(getContext(), iconResId, label);

        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.rightMargin = spacing;
        buttonContainer.addView(button, params);

        buttons.add(button);
        buttonActions.add(action);

        final int index = buttons.size() - 1;
        button.setOnClickListener(v -> selectAndActivate(index));

        // Track focus changes to update selection state
        button.setOnFocusChangeListener((v, hasFocus) -> {
            if (hasFocus) {
                selectedIndex = index;
                button.setSelected(true);
                // HorizontalScrollView automatically scrolls to keep focused items visible
            } else {
                button.setSelected(false);
            }
        });
    }

    /**
     * Set the action listener for menu events
     */
    public void setMenuActionListener(MenuActionListener listener) {
        this.actionListener = listener;
    }

    /**
     * Set whether face buttons should be flipped (A/B and X/Y swapped)
     */
    public void setFlipFaceButtons(boolean flip) {
        this.flipFaceButtons = flip;
    }

    /**
     * Handle gamepad D-pad and button inputs
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // Handle Android back button (but not gamepad SELECT button which sends KEYCODE_BACK)
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_DOWN) {
            if (!isGamepadEvent(event)) {
                closeMenu();
            }
            return true;
        }

        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            // Apply face button flipping if enabled
            int keyCode = event.getKeyCode();
            if (flipFaceButtons) {
                keyCode = handleFlipFaceButtons(keyCode);
            }

            switch (keyCode) {
                case KeyEvent.KEYCODE_BUTTON_A:
                case KeyEvent.KEYCODE_DPAD_CENTER:
                    activateSelected();
                    return true;

                case KeyEvent.KEYCODE_BUTTON_B:
                    closeMenu();
                    return true;

                case KeyEvent.KEYCODE_BUTTON_X:
                    // Only activate on a new press (not a repeated/held button from menu opening)
                    if (event.getRepeatCount() == 0) {
                        activateMouseEmulation();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_Y:
                    // Only activate on a new press (not a repeated/held button from menu opening)
                    if (event.getRepeatCount() == 0) {
                        activateKeyboard();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_START:
                case KeyEvent.KEYCODE_MENU:
                    // Only activate on a new press (not a repeated/held button from menu opening)
                    if (event.getRepeatCount() == 0) {
                        activateGuideButton();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_R1:
                    // Only activate on a new press (not a repeated/held button from menu opening)
                    if (event.getRepeatCount() == 0) {
                        activateToggleStats();
                    }
                    return true;
            }
        }

        // Check if this is a gamepad event and consume it to prevent pass-through
        if (isGamepadEvent(event)) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    /**
     * Override onKeyDown as a fallback to consume gamepad events that might not reach dispatchKeyEvent
     * All button actions are handled in dispatchKeyEvent() to prevent duplicate processing
     */
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        // Handle Android back button (but not gamepad SELECT button which sends KEYCODE_BACK)
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            if (!isGamepadEvent(event)) {
                closeMenu();
            }
            return true;
        }

        // Consume all gamepad events (actual handling is in dispatchKeyEvent)
        if (isGamepadEvent(event)) {
            return true;
        }

        return super.onKeyDown(keyCode, event);
    }


    /**
     * Handle analog stick and D-pad hat input
     */
    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        // Check if this is a gamepad event
        if (isGamepadMotionEvent(event)) {
            // Get left stick X-axis value
            float x = event.getAxisValue(MotionEvent.AXIS_X);

            // Get D-pad hat axis value (some controllers send D-pad as hat axis)
            float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);

            // Throttle analog navigation to prevent too-rapid movement
            long currentTime = System.currentTimeMillis();
            if (currentTime - lastAnalogNavTime >= ANALOG_NAV_THROTTLE_MS) {
                // Check both analog stick and D-pad hat
                float combinedX = Math.abs(x) > Math.abs(hatX) ? x : hatX;

                if (combinedX < -ANALOG_STICK_THRESHOLD) {
                    navigateLeft();
                    lastAnalogNavTime = currentTime;
                } else if (combinedX > ANALOG_STICK_THRESHOLD) {
                    navigateRight();
                    lastAnalogNavTime = currentTime;
                }
            }

            // Consume all gamepad motion events
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    /**
     * Check if a key event is from a gamepad
     */
    private boolean isGamepadEvent(KeyEvent event) {
        int source = event.getSource();
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
               (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
               (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    /**
     * Handle face button flipping when the flipFaceButtons preference is enabled
     */
    private int handleFlipFaceButtons(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A:
                return KeyEvent.KEYCODE_BUTTON_B;
            case KeyEvent.KEYCODE_BUTTON_B:
                return KeyEvent.KEYCODE_BUTTON_A;
            case KeyEvent.KEYCODE_BUTTON_X:
                return KeyEvent.KEYCODE_BUTTON_Y;
            case KeyEvent.KEYCODE_BUTTON_Y:
                return KeyEvent.KEYCODE_BUTTON_X;
            default:
                return keyCode;
        }
    }

    /**
     * Check if a motion event is from a gamepad
     */
    private boolean isGamepadMotionEvent(MotionEvent event) {
        int source = event.getSource();
        return (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /**
     * Navigate to the previous button
     */
    private void navigateLeft() {
        if (!buttons.isEmpty()) {
            setSelectedIndex((selectedIndex - 1 + buttons.size()) % buttons.size());
        }
    }

    /**
     * Navigate to the next button
     */
    private void navigateRight() {
        if (!buttons.isEmpty()) {
            setSelectedIndex((selectedIndex + 1) % buttons.size());
        }
    }

    /**
     * Set the currently selected button index
     */
    private void setSelectedIndex(int index) {
        if (index < 0 || index >= buttons.size()) {
            return;
        }

        // Clear selected state from all buttons
        for (OverlayMenuButton button : buttons) {
            button.setSelected(false);
        }

        // Set the index immediately to avoid race conditions with activateSelected()
        selectedIndex = index;

        // Set selected state on the new button
        buttons.get(index).setSelected(true);

        // Request focus on the button for scrolling and accessibility
        buttons.get(index).requestFocus();
    }

    /**
     * Select and activate a button by index
     */
    private void selectAndActivate(int index) {
        setSelectedIndex(index);
        activateSelected();
    }

    /**
     * Activate the currently selected button
     */
    private void activateSelected() {
        if (selectedIndex < 0 || selectedIndex >= buttons.size() || selectedIndex >= buttonActions.size()) {
            return;
        }

        int action = buttonActions.get(selectedIndex);
        boolean shouldCloseMenu = false;

        if (actionListener != null) {
            if (action == ACTION_DISCONNECT) {
                actionListener.onDisconnect();
                shouldCloseMenu = true;
            } else if (action == ACTION_QUIT) {
                actionListener.onQuitSession();
                shouldCloseMenu = true;
            } else if (action == ACTION_TOGGLE_STATS) {
                actionListener.onToggleStats();
                // Keep menu open
            } else if (action == ACTION_TOGGLE_MOUSE_EMULATION) {
                actionListener.onToggleMouseEmulation();
                shouldCloseMenu = true;
            } else if (action == ACTION_SHOW_KEYBOARD) {
                // Use helper method which closes menu before showing keyboard
                activateKeyboard();
                return; // Early return since activateKeyboard() handles menu closing
            } else if (action == ACTION_SEND_GUIDE) {
                actionListener.onSendGuideButton();
                shouldCloseMenu = true;
            } else if (action == ACTION_CLOSE) {
                closeMenu();
                return; // Early return since we're closing
            } else if (action >= ACTION_CUSTOM_BASE) {
                int commandIndex = action - ACTION_CUSTOM_BASE;
                List<CustomCommand> commands = commandsManager.getCommands();
                if (commandIndex >= 0 && commandIndex < commands.size()) {
                    actionListener.onCustomCommand(commands.get(commandIndex));
                }
                // Keep menu open
            }
        }

        // Only close menu for disconnect/quit actions
        if (shouldCloseMenu) {
            closeMenu();
        }
    }

    /**
     * Close the menu
     */
    public void closeMenu() {
        hide(() -> {
            if (actionListener != null) {
                actionListener.onMenuClosed();
            }
        });
    }

    /**
     * Activate mouse emulation shortcut (X button)
     */
    private void activateMouseEmulation() {
        if (actionListener != null) {
            actionListener.onToggleMouseEmulation();
        }
        closeMenu();
    }

    /**
     * Activate keyboard shortcut (Y button)
     */
    private void activateKeyboard() {
        // Close menu first, then show keyboard to avoid focus conflicts
        hide(() -> {
            if (actionListener != null) {
                actionListener.onShowKeyboard();
            }
        });
    }

    /**
     * Activate guide button shortcut (Start button)
     */
    private void activateGuideButton() {
        if (actionListener != null) {
            actionListener.onSendGuideButton();
        }
        closeMenu();
    }

    /**
     * Activate quit session shortcut (Select button)
     */
    private void activateQuitSession() {
        if (actionListener != null) {
            actionListener.onQuitSession();
        }
        closeMenu();
    }

    /**
     * Activate toggle stats shortcut (LB button)
     */
    private void activateToggleStats() {
        if (actionListener != null) {
            actionListener.onToggleStats();
        }
        closeMenu();
    }

    /**
     * Show the menu
     */
    public void show() {
        buildMenu();
        setVisibility(VISIBLE);

        // Ensure the view is properly refreshed
        invalidate();
        requestLayout();

        requestFocus();

        // Request focus on first button after view is laid out
        post(() -> {
            if (!buttons.isEmpty()) {
                setSelectedIndex(0);
            }
        });
    }

    /**
     * Hide the menu
     */
    public void hide(Runnable onComplete) {
        setVisibility(GONE);
        if (onComplete != null) {
            onComplete.run();
        }
    }
}
