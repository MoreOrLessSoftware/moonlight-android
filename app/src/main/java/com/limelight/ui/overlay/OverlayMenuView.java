package com.limelight.ui.overlay;

import android.app.UiModeManager;
import android.content.Context;
import android.content.res.Configuration;
import android.os.Build;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;

import com.limelight.R;

import java.util.ArrayList;
import java.util.List;

public class OverlayMenuView extends LinearLayout {
    private static final int BUTTON_SPACING_DP = 8;
    private static final float ANALOG_STICK_THRESHOLD = 0.5f;
    private static final long ANALOG_NAV_THROTTLE_MS = 200;

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

    private LinearLayout verticalContainer;
    private HorizontalScrollView horizontalScrollView;
    private LinearLayout horizontalContainer;

    private List<OverlayMenuButton> verticalButtons;
    private List<Integer> verticalActions;
    private List<OverlayMenuButton> horizontalButtons;
    private List<Integer> horizontalActions;

    private enum Region { VERTICAL, HORIZONTAL }
    private Region activeRegion = Region.VERTICAL;
    private int verticalIndex = 0;
    private int horizontalIndex = 0;

    private MenuActionListener actionListener;
    private CustomCommandsManager commandsManager;

    private static final int ACTION_DISCONNECT = 0;
    private static final int ACTION_QUIT = 1;
    private static final int ACTION_TOGGLE_STATS = 2;
    private static final int ACTION_CLOSE = 3;
    private static final int ACTION_SHOW_KEYBOARD = 4;
    private static final int ACTION_TOGGLE_MOUSE_EMULATION = 5;
    private static final int ACTION_SEND_GUIDE = 6;
    private static final int ACTION_CUSTOM_BASE = 100;

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
        setOrientation(LinearLayout.HORIZONTAL);
        setGravity(Gravity.BOTTOM);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setBackgroundDrawable(null);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            setDefaultFocusHighlightEnabled(false);
        }

        verticalContainer = new LinearLayout(context);
        verticalContainer.setOrientation(LinearLayout.VERTICAL);
        verticalContainer.setBackgroundDrawable(null);
        addView(verticalContainer, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ));

        horizontalScrollView = new HorizontalScrollView(context);
        horizontalScrollView.setHorizontalScrollBarEnabled(false);
        horizontalScrollView.setFocusable(false);
        horizontalScrollView.setFocusableInTouchMode(false);
        horizontalScrollView.setBackgroundDrawable(null);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            horizontalScrollView.setDefaultFocusHighlightEnabled(false);
        }

        horizontalContainer = new LinearLayout(context);
        horizontalContainer.setOrientation(LinearLayout.HORIZONTAL);
        horizontalContainer.setBackgroundDrawable(null);
        horizontalScrollView.addView(horizontalContainer);
        addView(horizontalScrollView, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ));

        verticalButtons = new ArrayList<>();
        verticalActions = new ArrayList<>();
        horizontalButtons = new ArrayList<>();
        horizontalActions = new ArrayList<>();

        activeRegion = Region.VERTICAL;
        verticalIndex = 0;
        horizontalIndex = 0;

        commandsManager = new CustomCommandsManager(context);
        isAndroidTV = isAndroidTV();

        setVisibility(GONE);
    }

    private boolean isAndroidTV() {
        UiModeManager uiModeManager = (UiModeManager) getContext().getSystemService(Context.UI_MODE_SERVICE);
        return uiModeManager.getCurrentModeType() == Configuration.UI_MODE_TYPE_TELEVISION;
    }

    public void buildMenu() {
        activeRegion = Region.VERTICAL;
        verticalIndex = 0;
        horizontalIndex = 0;
        clearFocus();

        verticalContainer.removeAllViews();
        horizontalContainer.removeAllViews();
        verticalButtons.clear();
        verticalActions.clear();
        horizontalButtons.clear();
        horizontalActions.clear();

        float density = getContext().getResources().getDisplayMetrics().density;
        int spacing = (int) (BUTTON_SPACING_DP * density);

        // Vertical column: top → bottom
        addVerticalButton(R.drawable.ic_overlay_guide,
            getContext().getString(R.string.overlay_menu_guide), ACTION_SEND_GUIDE, spacing);
        addVerticalButton(R.drawable.ic_overlay_mouse,
                getContext().getString(R.string.overlay_menu_mouse_emulation), ACTION_TOGGLE_MOUSE_EMULATION, spacing);
        if (!isAndroidTV) {
            addVerticalButton(R.drawable.ic_overlay_keyboard_toggle,
                getContext().getString(R.string.overlay_menu_keyboard), ACTION_SHOW_KEYBOARD, spacing);
        }
        addVerticalButton(R.drawable.ic_overlay_perf,
                getContext().getString(R.string.overlay_menu_toggle_stats), ACTION_TOGGLE_STATS, spacing);
        addVerticalButton(R.drawable.ic_overlay_power,
            getContext().getString(R.string.overlay_menu_quit_session), ACTION_QUIT, spacing);
        addVerticalButton(R.drawable.ic_overlay_monitor,
            getContext().getString(R.string.overlay_menu_disconnect), ACTION_DISCONNECT, 0);

        // Add spacing between vertical column and horizontal row
        ((LinearLayout.LayoutParams) horizontalScrollView.getLayoutParams()).leftMargin = spacing;

        // Horizontal row: custom commands then Close
        List<CustomCommand> customCommands = commandsManager.getCommands();
        for (CustomCommand command : customCommands) {
            addHorizontalButton(command.getIconResId(), command.getName(),
                ACTION_CUSTOM_BASE + horizontalButtons.size(), spacing);
        }
        addHorizontalButton(0,
            getContext().getString(R.string.overlay_menu_close), ACTION_CLOSE, 0);

        verticalContainer.invalidate();
        verticalContainer.requestLayout();
        horizontalContainer.invalidate();
        horizontalContainer.requestLayout();
    }

    private void addVerticalButton(int iconResId, String label, int action, int bottomMarginPx) {
        OverlayMenuButton button = OverlayMenuButton.create(getContext(), iconResId, label);

        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.bottomMargin = bottomMarginPx;
        verticalContainer.addView(button, params);

        verticalButtons.add(button);
        verticalActions.add(action);

        final int index = verticalButtons.size() - 1;
        button.setOnClickListener(v -> selectAndActivate(Region.VERTICAL, index));
        button.setOnFocusChangeListener((v, hasFocus) -> {
            if (hasFocus) {
                activeRegion = Region.VERTICAL;
                verticalIndex = index;
                button.setSelected(true);
            } else {
                button.setSelected(false);
            }
        });
    }

    private void addHorizontalButton(int iconResId, String label, int action, int rightMarginPx) {
        OverlayMenuButton button = OverlayMenuButton.create(getContext(), iconResId, label);

        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.rightMargin = rightMarginPx;
        horizontalContainer.addView(button, params);

        horizontalButtons.add(button);
        horizontalActions.add(action);

        final int index = horizontalButtons.size() - 1;
        button.setOnClickListener(v -> selectAndActivate(Region.HORIZONTAL, index));
        button.setOnFocusChangeListener((v, hasFocus) -> {
            if (hasFocus) {
                activeRegion = Region.HORIZONTAL;
                horizontalIndex = index;
                button.setSelected(true);
            } else {
                button.setSelected(false);
            }
        });
    }

    public void setMenuActionListener(MenuActionListener listener) {
        this.actionListener = listener;
    }

    public void setFlipFaceButtons(boolean flip) {
        this.flipFaceButtons = flip;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_DOWN) {
            if (!isFullGamepadEvent(event) && event.getRepeatCount() == 0) {
                closeMenu();
            }
            return true;
        }

        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            int keyCode = event.getKeyCode();
            if (flipFaceButtons) {
                keyCode = handleFlipFaceButtons(keyCode);
            }

            switch (keyCode) {
                case KeyEvent.KEYCODE_DPAD_LEFT:
                    navigateLeft();
                    return true;

                case KeyEvent.KEYCODE_DPAD_RIGHT:
                    navigateRight();
                    return true;

                case KeyEvent.KEYCODE_DPAD_UP:
                    navigateUp();
                    return true;

                case KeyEvent.KEYCODE_DPAD_DOWN:
                    navigateDown();
                    return true;

                case KeyEvent.KEYCODE_BUTTON_A:
                case KeyEvent.KEYCODE_DPAD_CENTER:
                    activateSelected();
                    return true;

                case KeyEvent.KEYCODE_BUTTON_B:
                    closeMenu();
                    return true;

                case KeyEvent.KEYCODE_BUTTON_X:
                    if (event.getRepeatCount() == 0) {
                        activateMouseEmulation();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_Y:
                    if (event.getRepeatCount() == 0) {
                        activateKeyboard();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_START:
                case KeyEvent.KEYCODE_MENU:
                    if (event.getRepeatCount() == 0) {
                        activateGuideButton();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_R1:
                    if (event.getRepeatCount() == 0) {
                        activateToggleStats();
                    }
                    return true;
            }
        }

        if (isGamepadEvent(event)) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            if (!isFullGamepadEvent(event) && event.getRepeatCount() == 0) {
                closeMenu();
            }
            return true;
        }

        if (isGamepadEvent(event)) {
            return true;
        }

        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (isGamepadMotionEvent(event)) {
            float x = event.getAxisValue(MotionEvent.AXIS_X);
            float y = event.getAxisValue(MotionEvent.AXIS_Y);
            float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
            float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);

            float combinedX = Math.abs(x) > Math.abs(hatX) ? x : hatX;
            float combinedY = Math.abs(y) > Math.abs(hatY) ? y : hatY;

            long currentTime = System.currentTimeMillis();
            if (currentTime - lastAnalogNavTime >= ANALOG_NAV_THROTTLE_MS) {
                // Dominant axis wins to prevent cross-region jumps on diagonal inputs
                if (Math.abs(combinedX) >= Math.abs(combinedY)) {
                    if (combinedX < -ANALOG_STICK_THRESHOLD) {
                        navigateLeft();
                        lastAnalogNavTime = currentTime;
                    } else if (combinedX > ANALOG_STICK_THRESHOLD) {
                        navigateRight();
                        lastAnalogNavTime = currentTime;
                    }
                } else {
                    if (combinedY < -ANALOG_STICK_THRESHOLD) {
                        navigateUp();
                        lastAnalogNavTime = currentTime;
                    } else if (combinedY > ANALOG_STICK_THRESHOLD) {
                        navigateDown();
                        lastAnalogNavTime = currentTime;
                    }
                }
            }

            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    private boolean isGamepadEvent(KeyEvent event) {
        int source = event.getSource();
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
               (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
               (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    private boolean isFullGamepadEvent(KeyEvent event) {
        int source = event.getSource();
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
               (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private int handleFlipFaceButtons(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return KeyEvent.KEYCODE_BUTTON_B;
            case KeyEvent.KEYCODE_BUTTON_B: return KeyEvent.KEYCODE_BUTTON_A;
            case KeyEvent.KEYCODE_BUTTON_X: return KeyEvent.KEYCODE_BUTTON_Y;
            case KeyEvent.KEYCODE_BUTTON_Y: return KeyEvent.KEYCODE_BUTTON_X;
            default: return keyCode;
        }
    }

    private boolean isGamepadMotionEvent(MotionEvent event) {
        int source = event.getSource();
        return (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private void navigateUp() {
        if (activeRegion == Region.VERTICAL) {
            if (!verticalButtons.isEmpty()) {
                setVerticalIndex((verticalIndex - 1 + verticalButtons.size()) % verticalButtons.size());
            }
        } else {
            // From horizontal → Up: jump to button above Disconnect (second-to-last in vertical)
            clearHorizontalSelection();
            activeRegion = Region.VERTICAL;
            int target = verticalButtons.size() >= 2 ? verticalButtons.size() - 2 : 0;
            setVerticalIndex(target);
        }
    }

    private void navigateDown() {
        if (activeRegion == Region.VERTICAL) {
            if (!verticalButtons.isEmpty()) {
                setVerticalIndex((verticalIndex + 1) % verticalButtons.size());
            }
        } else {
            // From horizontal → Down: jump to topmost vertical button
            clearHorizontalSelection();
            activeRegion = Region.VERTICAL;
            setVerticalIndex(0);
        }
    }

    private void navigateLeft() {
        if (activeRegion == Region.HORIZONTAL) {
            if (horizontalIndex > 0) {
                setHorizontalIndex(horizontalIndex - 1);
            } else {
                // At leftmost horizontal button — cross to Disconnect (last vertical button)
                clearHorizontalSelection();
                activeRegion = Region.VERTICAL;
                setVerticalIndex(verticalButtons.size() - 1);
            }
        } else {
            // In vertical region — wrap around to rightmost horizontal button
            if (!horizontalButtons.isEmpty()) {
                clearVerticalSelection();
                activeRegion = Region.HORIZONTAL;
                setHorizontalIndex(horizontalButtons.size() - 1);
            }
        }
    }

    private void navigateRight() {
        if (activeRegion == Region.VERTICAL) {
            if (!horizontalButtons.isEmpty()) {
                clearVerticalSelection();
                activeRegion = Region.HORIZONTAL;
                setHorizontalIndex(0);
            }
        } else {
            if (horizontalIndex < horizontalButtons.size() - 1) {
                setHorizontalIndex(horizontalIndex + 1);
            } else {
                // At rightmost horizontal button — cross to Disconnect (last vertical button)
                clearHorizontalSelection();
                activeRegion = Region.VERTICAL;
                setVerticalIndex(verticalButtons.size() - 1);
            }
        }
    }

    private void setVerticalIndex(int index) {
        if (index < 0 || index >= verticalButtons.size()) return;

        for (OverlayMenuButton b : verticalButtons) {
            b.setSelected(false);
        }

        verticalIndex = index;
        verticalButtons.get(index).setSelected(true);
        verticalButtons.get(index).requestFocus();
    }

    private void setHorizontalIndex(int index) {
        if (index < 0 || index >= horizontalButtons.size()) return;

        for (OverlayMenuButton b : horizontalButtons) {
            b.setSelected(false);
        }

        horizontalIndex = index;
        horizontalButtons.get(index).setSelected(true);
        horizontalButtons.get(index).requestFocus();
    }

    private void clearVerticalSelection() {
        for (OverlayMenuButton b : verticalButtons) {
            b.setSelected(false);
        }
    }

    private void clearHorizontalSelection() {
        for (OverlayMenuButton b : horizontalButtons) {
            b.setSelected(false);
        }
    }

    private void selectAndActivate(Region region, int index) {
        activeRegion = region;
        if (region == Region.VERTICAL) {
            setVerticalIndex(index);
        } else {
            setHorizontalIndex(index);
        }
        activateSelected();
    }

    private void activateSelected() {
        int action;
        if (activeRegion == Region.VERTICAL) {
            if (verticalIndex < 0 || verticalIndex >= verticalActions.size()) return;
            action = verticalActions.get(verticalIndex);
        } else {
            if (horizontalIndex < 0 || horizontalIndex >= horizontalActions.size()) return;
            action = horizontalActions.get(horizontalIndex);
        }

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
                activateKeyboard();
                return;
            } else if (action == ACTION_SEND_GUIDE) {
                actionListener.onSendGuideButton();
                shouldCloseMenu = true;
            } else if (action == ACTION_CLOSE) {
                closeMenu();
                return;
            } else if (action >= ACTION_CUSTOM_BASE) {
                int commandIndex = action - ACTION_CUSTOM_BASE;
                List<CustomCommand> commands = commandsManager.getCommands();
                if (commandIndex >= 0 && commandIndex < commands.size()) {
                    actionListener.onCustomCommand(commands.get(commandIndex));
                }
                // Keep menu open
            }
        }

        if (shouldCloseMenu) {
            closeMenu();
        }
    }

    public void closeMenu() {
        hide(() -> {
            if (actionListener != null) {
                actionListener.onMenuClosed();
            }
        });
    }

    private void activateMouseEmulation() {
        if (actionListener != null) {
            actionListener.onToggleMouseEmulation();
        }
        closeMenu();
    }

    private void activateKeyboard() {
        hide(() -> {
            if (actionListener != null) {
                actionListener.onShowKeyboard();
            }
        });
    }

    private void activateGuideButton() {
        if (actionListener != null) {
            actionListener.onSendGuideButton();
        }
        closeMenu();
    }

    private void activateQuitSession() {
        if (actionListener != null) {
            actionListener.onQuitSession();
        }
        closeMenu();
    }

    private void activateToggleStats() {
        if (actionListener != null) {
            actionListener.onToggleStats();
        }
        closeMenu();
    }

    public void show() {
        buildMenu();
        setVisibility(VISIBLE);

        invalidate();
        requestLayout();

        requestFocus();

        post(() -> {
            // Equalize all vertical button widths to the widest one
            int maxWidth = 0;
            for (OverlayMenuButton b : verticalButtons) {
                maxWidth = Math.max(maxWidth, b.getWidth());
            }
            if (maxWidth > 0) {
                for (OverlayMenuButton b : verticalButtons) {
                    b.setMinimumWidth(maxWidth);
                }
                verticalContainer.requestLayout();
            }

            if (!verticalButtons.isEmpty()) {
                setVerticalIndex(verticalButtons.size() - 1);
            }
        });
    }

    public void hide(Runnable onComplete) {
        setVisibility(GONE);
        if (onComplete != null) {
            onComplete.run();
        }
    }
}
