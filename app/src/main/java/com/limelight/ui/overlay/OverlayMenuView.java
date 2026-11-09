package com.limelight.ui.overlay;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.os.Build;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;

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
        void onChangeStats();
        void onToggleMouseEmulation();
        void onShowKeyboard();
        void onSendGuideButton();
        void onChangeJitterBuffer();
        void onShowBrightness();
        void onCustomCommand(CustomCommand command);
        // A command of the host's; the listener answers with hostCommandFinished()
        void onHostCommand(HostCommands.Command command);
        // The menu has opened, as when the host's commands could be asked for again
        void onMenuShown();
        void onMenuClosed();
    }

    private ScrollView verticalScrollView;
    private LinearLayout verticalContainer;
    // The buttons are compact, to fit a short screen (onMeasure())
    private boolean compact = false;
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
    private static final int ACTION_STATS = 2;
    private static final int ACTION_CLOSE = 3;
    private static final int ACTION_SHOW_KEYBOARD = 4;
    private static final int ACTION_TOGGLE_MOUSE_EMULATION = 5;
    private static final int ACTION_SEND_GUIDE = 6;
    private static final int ACTION_JITTER_BUFFER = 7;
    private static final int ACTION_BRIGHTNESS = 8;
    private static final int ACTION_BACK = 9;
    private static final int ACTION_DISCONNECT_PAGE = 10;
    private static final int ACTION_CUSTOM_BASE = 100;
    private static final int ACTION_OPTION_BASE = 200;
    // The host's command n, the button of the host's group n, and command n of the group showing
    private static final int ACTION_HOST_COMMAND_BASE = 1000;
    private static final int ACTION_HOST_GROUP_BASE = 2000;
    private static final int ACTION_GROUP_COMMAND_BASE = 3000;

    // How long a message (like why a host command failed) shows on its button
    private static final long MESSAGE_MS = 4000;

    /** Gets the option picked from an options page (showOptions()) */
    public interface OptionPicker {
        void onOptionPicked(int index);
    }

    // The button whose options are showing in place of the main buttons, or -1 for none. The
    // options page is asked for by the listener, for the button activated last.
    private int optionsAction = -1;
    private int requestedOptionsAction = -1;
    private String[] optionNames;
    private int optionChecked = -1;
    private OptionPicker optionPicker;

    // The host's commands and groups, and the group whose commands are showing, or -1
    private HostCommands hostCommands = HostCommands.NONE;
    private int group = -1;
    // The button of the host command running, for its result, and of the message showing
    private int runningHostAction = -1;
    private int messageAction = -1;
    private String messageLabel;
    private final Runnable messageTimeout = this::clearMessage;

    // Disconnect and Quit Session are showing in place of the main buttons. Quitting closes the
    // game on the host, losing anything unsaved, so getting to it takes this extra step.
    private boolean disconnectPage = false;

    private long lastAnalogNavTime = 0;
    private boolean flipFaceButtons = false;
    // Only with "sync to host frame timing" pacing, the one mode with a jitter buffer
    private boolean showJitterBuffer = false;
    // Not on TVs, whose screen brightness the app can't change
    private boolean showBrightness = true;

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

        // Scrolls where even compact buttons (onMeasure()) are too tall for the screen; a focused
        // button is scrolled into view
        verticalScrollView = new ScrollView(context);
        verticalScrollView.setVerticalScrollBarEnabled(false);
        verticalScrollView.setOverScrollMode(OVER_SCROLL_NEVER);
        verticalScrollView.setFocusable(false);
        verticalScrollView.setFocusableInTouchMode(false);
        verticalScrollView.setBackgroundDrawable(null);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            verticalScrollView.setDefaultFocusHighlightEnabled(false);
        }
        verticalScrollView.addView(verticalContainer);
        addView(verticalScrollView, new LinearLayout.LayoutParams(
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

        setVisibility(GONE);
    }

    public void buildMenu() {
        optionsAction = -1;
        optionPicker = null;
        group = -1;
        disconnectPage = false;
        runningHostAction = -1;
        removeCallbacks(messageTimeout);
        messageAction = -1;
        buildButtons();
    }

    private void buildButtons() {
        activeRegion = Region.VERTICAL;
        verticalIndex = 0;
        horizontalIndex = 0;
        clearFocus();
        // The new buttons start out full size; onMeasure() makes them compact if need be
        compact = false;

        verticalContainer.removeAllViews();
        horizontalContainer.removeAllViews();
        verticalButtons.clear();
        verticalActions.clear();
        horizontalButtons.clear();
        horizontalActions.clear();

        float density = getContext().getResources().getDisplayMetrics().density;
        int spacing = (int) (BUTTON_SPACING_DP * density);

        // Add spacing between vertical column and horizontal row
        ((LinearLayout.LayoutParams) horizontalScrollView.getLayoutParams()).leftMargin = spacing;

        if (optionsAction >= 0) {
            // The options, with the one in effect marked, and Back
            for (int i = 0; i < optionNames.length; i++) {
                addVerticalButton(i == optionChecked ? R.drawable.ic_overlay_radio_checked : R.drawable.ic_overlay_radio_unchecked,
                        optionNames[i], ACTION_OPTION_BASE + i, i < optionNames.length - 1 ? spacing : 0);
            }
            addHorizontalButton(R.drawable.ic_overlay_back,
                    getContext().getString(R.string.overlay_menu_back), ACTION_BACK, 0);
            return;
        }

        if (group >= 0 && group < hostCommands.groups.size()) {
            // The group's commands, and Back
            List<HostCommands.Command> commands = hostCommands.groups.get(group).commands;
            for (int i = 0; i < commands.size() && i < 1000; i++) {
                HostCommands.Command command = commands.get(i);
                addVerticalButton(MaterialIcons.drawable(getContext(), command.icon), command.label,
                        ACTION_GROUP_COMMAND_BASE + i, i < commands.size() - 1 ? spacing : 0);
            }
            addHorizontalButton(R.drawable.ic_overlay_back,
                    getContext().getString(R.string.overlay_menu_back), ACTION_BACK, 0);
            return;
        }

        if (disconnectPage) {
            // Disconnect stays last, where the button that opened this page was, so a repeated
            // press can only land on the choice that leaves the game running
            addVerticalButton(R.drawable.ic_overlay_power,
                    getContext().getString(R.string.overlay_menu_quit_session), ACTION_QUIT, spacing);
            addVerticalButton(R.drawable.ic_overlay_monitor,
                    getContext().getString(R.string.overlay_menu_disconnect), ACTION_DISCONNECT, 0);
            addHorizontalButton(R.drawable.ic_overlay_back,
                    getContext().getString(R.string.overlay_menu_back), ACTION_BACK, 0);
            return;
        }

        // Vertical column: top → bottom
        addVerticalButton(R.drawable.ic_overlay_guide,
            getContext().getString(R.string.overlay_menu_guide), ACTION_SEND_GUIDE, spacing);
        addVerticalButton(R.drawable.ic_overlay_mouse,
                getContext().getString(R.string.overlay_menu_mouse_emulation), ACTION_TOGGLE_MOUSE_EMULATION, spacing);
        addVerticalButton(R.drawable.ic_overlay_keyboard_toggle,
            getContext().getString(R.string.overlay_menu_keyboard), ACTION_SHOW_KEYBOARD, spacing);
        addVerticalButton(R.drawable.ic_overlay_perf,
                getContext().getString(R.string.overlay_menu_toggle_stats), ACTION_STATS, spacing);
        if (showBrightness) {
            addVerticalButton(R.drawable.ic_overlay_settings_brightness,
                    getContext().getString(R.string.overlay_menu_brightness), ACTION_BRIGHTNESS, spacing);
        }
        if (showJitterBuffer) {
            addVerticalButton(R.drawable.ic_overlay_jitter_buffer,
                    getContext().getString(R.string.overlay_menu_jitter_buffer), ACTION_JITTER_BUFFER, spacing);
        }
        addVerticalButton(R.drawable.ic_overlay_monitor,
            getContext().getString(R.string.overlay_menu_disconnect), ACTION_DISCONNECT_PAGE, 0);

        // Horizontal row: custom commands, the host's commands, the host's groups, then Close
        List<CustomCommand> customCommands = commandsManager.getCommands();
        for (int i = 0; i < customCommands.size(); i++) {
            CustomCommand command = customCommands.get(i);
            addHorizontalButton(command.getIconResId(), command.getName(), ACTION_CUSTOM_BASE + i, spacing);
        }
        for (int i = 0; i < hostCommands.commands.size() && i < 1000; i++) {
            HostCommands.Command command = hostCommands.commands.get(i);
            addHorizontalButton(MaterialIcons.drawable(getContext(), command.icon), command.label,
                    ACTION_HOST_COMMAND_BASE + i, spacing);
        }
        for (int i = 0; i < hostCommands.groups.size() && i < 1000; i++) {
            HostCommands.Group hostGroup = hostCommands.groups.get(i);
            addHorizontalButton(MaterialIcons.drawable(getContext(), hostGroup.iconOrDefault()), hostGroup.label,
                    ACTION_HOST_GROUP_BASE + i, spacing);
        }
        addHorizontalButton(0,
            getContext().getString(R.string.overlay_menu_close), ACTION_CLOSE, 0);

        verticalContainer.invalidate();
        verticalContainer.requestLayout();
        horizontalContainer.invalidate();
        horizontalContainer.requestLayout();
    }

    private void addVerticalButton(int iconResId, String label, int action, int bottomMarginPx) {
        addVerticalButton(OverlayMenuButton.create(getContext(), iconResId, label), action, bottomMarginPx);
    }

    private void addVerticalButton(Drawable icon, String label, int action, int bottomMarginPx) {
        addVerticalButton(OverlayMenuButton.create(getContext(), icon, label), action, bottomMarginPx);
    }

    private void addVerticalButton(OverlayMenuButton button, int action, int bottomMarginPx) {

        // As wide as the column, which is as wide as its widest button
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
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
        addHorizontalButton(OverlayMenuButton.create(getContext(), iconResId, label), action, rightMarginPx);
    }

    private void addHorizontalButton(Drawable icon, String label, int action, int rightMarginPx) {
        addHorizontalButton(OverlayMenuButton.create(getContext(), icon, label), action, rightMarginPx);
    }

    private void addHorizontalButton(OverlayMenuButton button, int action, int rightMarginPx) {

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

    // On a short screen (a phone in landscape), the column of full size buttons runs off the top
    // and bottom, cutting off Disconnect. When it won't fit, the buttons and the spacing between
    // them take half the vertical room; if that's still too tall, the column scrolls.
    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int available = MeasureSpec.getMode(heightMeasureSpec) == MeasureSpec.UNSPECIFIED
                ? Integer.MAX_VALUE : MeasureSpec.getSize(heightMeasureSpec) - getPaddingTop() - getPaddingBottom();
        boolean needCompact = columnHeightPx(false) > available;
        if (needCompact != compact) {
            setCompact(needCompact);
        }
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);
    }

    private int spacingPx(boolean compact) {
        float density = getContext().getResources().getDisplayMetrics().density;
        return (int) ((compact ? BUTTON_SPACING_DP / 2 : BUTTON_SPACING_DP) * density);
    }

    private int columnHeightPx(boolean compact) {
        int count = verticalButtons.size();
        if (count == 0) {
            return 0;
        }
        return count * OverlayMenuButton.heightPx(getContext(), compact) + (count - 1) * spacingPx(compact);
    }

    private void setCompact(boolean compact) {
        this.compact = compact;
        int spacing = spacingPx(compact);
        for (int i = 0; i < verticalButtons.size(); i++) {
            OverlayMenuButton button = verticalButtons.get(i);
            button.setCompact(compact);
            ((LinearLayout.LayoutParams) button.getLayoutParams()).bottomMargin =
                    i < verticalButtons.size() - 1 ? spacing : 0;
        }
        // The row along the bottom matches the column
        for (OverlayMenuButton button : horizontalButtons) {
            button.setCompact(compact);
        }
    }

    public void setMenuActionListener(MenuActionListener listener) {
        this.actionListener = listener;
    }

    public void setFlipFaceButtons(boolean flip) {
        this.flipFaceButtons = flip;
    }

    public void setShowJitterBuffer(boolean show) {
        this.showJitterBuffer = show;
    }

    public void setShowBrightness(boolean show) {
        this.showBrightness = show;
    }

    /**
     * The host's commands, along the row after the custom commands, and its groups, as buttons
     * after those that open a page of their commands. An open menu shows them at once, keeping
     * the selected button where it can.
     */
    public void setHostCommands(HostCommands commands) {
        hostCommands = commands != null ? commands : HostCommands.NONE;
        if (getVisibility() != VISIBLE || optionsAction >= 0 || disconnectPage) {
            return;
        }
        if (group >= hostCommands.groups.size()) {
            group = -1;
        }

        // The same button stays selected where it's still there
        Region region = activeRegion;
        int selected = selectedAction();
        int verticalBefore = verticalIndex;
        int horizontalBefore = horizontalIndex;
        clearMessage();
        runningHostAction = -1;
        buildButtons();

        List<Integer> actions = region == Region.VERTICAL ? verticalActions : horizontalActions;
        int index = actions.indexOf(selected);
        if (region == Region.VERTICAL) {
            setVerticalIndex(index >= 0 ? index : Math.min(verticalBefore, verticalButtons.size() - 1));
        } else {
            activeRegion = Region.HORIZONTAL;
            setHorizontalIndex(index >= 0 ? index : Math.min(horizontalBefore, horizontalButtons.size() - 1));
        }
    }

    /**
     * The result of the host command the listener was asked to run: the menu closes if it ran and
     * the command says to, and otherwise a failure shows on its button for a few seconds.
     */
    public void hostCommandFinished(boolean ok, boolean closeMenu, String message) {
        int action = runningHostAction;
        runningHostAction = -1;
        if (getVisibility() != VISIBLE) {
            return;
        }
        if (ok) {
            if (closeMenu) {
                closeMenu();
            }
        } else if (action >= 0) {
            showMessage(action, message);
        }
    }

    private void showMessage(int action, String message) {
        clearMessage();
        OverlayMenuButton button = buttonForAction(action);
        if (button == null) {
            return;
        }
        messageAction = action;
        messageLabel = button.getLabel();
        button.setLabel(message);
        postDelayed(messageTimeout, MESSAGE_MS);
    }

    private void clearMessage() {
        removeCallbacks(messageTimeout);
        if (messageAction >= 0) {
            OverlayMenuButton button = buttonForAction(messageAction);
            if (button != null) {
                button.setLabel(messageLabel);
            }
        }
        messageAction = -1;
        messageLabel = null;
    }

    private OverlayMenuButton buttonForAction(int action) {
        int index = verticalActions.indexOf(action);
        if (index >= 0) {
            return verticalButtons.get(index);
        }
        index = horizontalActions.indexOf(action);
        return index >= 0 ? horizontalButtons.get(index) : null;
    }

    private int selectedAction() {
        if (activeRegion == Region.VERTICAL) {
            return verticalIndex >= 0 && verticalIndex < verticalActions.size() ? verticalActions.get(verticalIndex) : -1;
        }
        return horizontalIndex >= 0 && horizontalIndex < horizontalActions.size() ? horizontalActions.get(horizontalIndex) : -1;
    }

    /** Shows a host group's commands in place of the main buttons, with the first selected */
    private void showGroup(int index) {
        if (index < 0 || index >= hostCommands.groups.size()) {
            return;
        }
        clearMessage();
        group = index;
        buildButtons();
        setVerticalIndex(0);
    }

    /** Shows Disconnect and Quit Session in place of the main buttons, on Disconnect */
    private void showDisconnectPage() {
        clearMessage();
        disconnectPage = true;
        buildButtons();
        setVerticalIndex(verticalButtons.size() - 1);
    }

    // Whether the main buttons are showing (not options, a group's commands or the disconnect page)
    private boolean isOnMainButtons() {
        return optionsAction < 0 && group < 0 && !disconnectPage;
    }

    /**
     * Shows options in place of the main buttons, for the button that asked for them (Stats or
     * Jitter Buffer). Picking one closes the menu and hands it to the picker; B goes back.
     */
    public void showOptions(String[] names, int checked, OptionPicker picker) {
        if (getVisibility() != VISIBLE || requestedOptionsAction < 0 || names.length == 0) {
            return;
        }

        optionsAction = requestedOptionsAction;
        requestedOptionsAction = -1;
        optionNames = names;
        optionChecked = checked;
        optionPicker = picker;
        buildButtons();

        // On the option in effect
        setVerticalIndex(checked >= 0 && checked < names.length ? checked : 0);
    }

    /** Back from another page to the main buttons, on the button it was for */
    private void showMainButtons() {
        int from = disconnectPage ? ACTION_DISCONNECT_PAGE
                : group >= 0 ? ACTION_HOST_GROUP_BASE + group : optionsAction;
        optionsAction = -1;
        optionPicker = null;
        group = -1;
        disconnectPage = false;
        clearMessage();
        buildButtons();

        int index = horizontalActions.indexOf(from);
        if (index >= 0) {
            activeRegion = Region.HORIZONTAL;
            setHorizontalIndex(index);
            return;
        }
        index = verticalActions.indexOf(from);
        setVerticalIndex(index >= 0 ? index : verticalButtons.size() - 1);
    }

    private void requestOptions(int action) {
        if (actionListener == null) {
            return;
        }

        requestedOptionsAction = action;
        if (action == ACTION_STATS) {
            actionListener.onChangeStats();
        } else {
            actionListener.onChangeJitterBuffer();
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_DOWN) {
            if (!isFullGamepadEvent(event) && event.getRepeatCount() == 0) {
                goBack();
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
                    // A press held on the button that opened the disconnect page doesn't go on
                    // to choose from it
                    if (!disconnectPage || event.getRepeatCount() == 0) {
                        activateSelected();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_B:
                    if (event.getRepeatCount() == 0) {
                        goBack();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_X:
                    if (event.getRepeatCount() == 0 && isOnMainButtons()) {
                        activateMouseEmulation();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_Y:
                    if (event.getRepeatCount() == 0 && isOnMainButtons()) {
                        activateKeyboard();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_START:
                case KeyEvent.KEYCODE_MENU:
                    if (event.getRepeatCount() == 0 && isOnMainButtons()) {
                        activateGuideButton();
                    }
                    return true;

                case KeyEvent.KEYCODE_BUTTON_R1:
                    if (event.getRepeatCount() == 0 && isOnMainButtons()) {
                        activateStats();
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
                goBack();
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
        clearMessage();
        if (activeRegion == Region.VERTICAL) {
            if (!verticalButtons.isEmpty()) {
                setVerticalIndex((verticalIndex - 1 + verticalButtons.size()) % verticalButtons.size());
            }
        } else {
            // From horizontal → Up: jump to button above Disconnect (second-to-last in vertical),
            // or from Back to the last option
            clearHorizontalSelection();
            activeRegion = Region.VERTICAL;
            int target = !isOnMainButtons() ? verticalButtons.size() - 1 :
                    verticalButtons.size() >= 2 ? verticalButtons.size() - 2 : 0;
            setVerticalIndex(target);
        }
    }

    private void navigateDown() {
        clearMessage();
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
        clearMessage();
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
        clearMessage();
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
        OverlayMenuButton button = horizontalButtons.get(index);
        button.setSelected(true);
        button.requestFocus();

        // Scrolled into view once it's laid out: a row built just now has no positions yet for
        // focus to scroll to
        horizontalScrollView.post(() -> {
            int left = button.getLeft();
            int right = button.getRight();
            int scroll = horizontalScrollView.getScrollX();
            int width = horizontalScrollView.getWidth();
            if (right > scroll + width) {
                horizontalScrollView.smoothScrollTo(right - width, 0);
            } else if (left < scroll) {
                horizontalScrollView.smoothScrollTo(left, 0);
            }
        });
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

        if (action == ACTION_BACK) {
            showMainButtons();
            return;
        } else if (action == ACTION_DISCONNECT_PAGE) {
            showDisconnectPage();
            return;
        } else if (action >= ACTION_GROUP_COMMAND_BASE) {
            if (group >= 0 && group < hostCommands.groups.size()) {
                List<HostCommands.Command> commands = hostCommands.groups.get(group).commands;
                int index = action - ACTION_GROUP_COMMAND_BASE;
                if (index < commands.size()) {
                    runHostCommand(commands.get(index), action);
                }
            }
            return;
        } else if (action >= ACTION_HOST_GROUP_BASE) {
            showGroup(action - ACTION_HOST_GROUP_BASE);
            return;
        } else if (action >= ACTION_HOST_COMMAND_BASE) {
            int index = action - ACTION_HOST_COMMAND_BASE;
            if (index < hostCommands.commands.size()) {
                runHostCommand(hostCommands.commands.get(index), action);
            }
            return;
        } else if (action >= ACTION_OPTION_BASE) {
            // Picking closes the menu, as a choice in a dialog did
            OptionPicker picker = optionPicker;
            closeMenu();
            if (picker != null) {
                picker.onOptionPicked(action - ACTION_OPTION_BASE);
            }
            return;
        }

        if (actionListener != null) {
            if (action == ACTION_DISCONNECT) {
                actionListener.onDisconnect();
                shouldCloseMenu = true;
            } else if (action == ACTION_QUIT) {
                closeMenu();
                actionListener.onQuitSession();
                return;
            } else if (action == ACTION_STATS) {
                activateStats();
                return;
            } else if (action == ACTION_TOGGLE_MOUSE_EMULATION) {
                actionListener.onToggleMouseEmulation();
                shouldCloseMenu = true;
            } else if (action == ACTION_SHOW_KEYBOARD) {
                activateKeyboard();
                return;
            } else if (action == ACTION_SEND_GUIDE) {
                actionListener.onSendGuideButton();
                shouldCloseMenu = true;
            } else if (action == ACTION_JITTER_BUFFER) {
                requestOptions(ACTION_JITTER_BUFFER);
                return;
            } else if (action == ACTION_BRIGHTNESS) {
                // Closed first, so the slider gets the controller and remote input
                closeMenu();
                actionListener.onShowBrightness();
                return;
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

    private void runHostCommand(HostCommands.Command command, int action) {
        clearMessage();
        if (actionListener != null) {
            // The menu stays open until the host answers, so a failure can show on the button
            runningHostAction = action;
            actionListener.onHostCommand(command);
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
        // Hide first, then post the keyboard request so the window manager has a frame
        // to process the visibility change and return focus to the game view before the
        // IME is requested. Without this, newer Android silently drops showSoftInput()
        // because the overlay view still holds focus at the moment of the call.
        setVisibility(GONE);
        post(() -> {
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

    private void activateStats() {
        requestOptions(ACTION_STATS);
    }

    // B or Back: from another page to the main buttons, or from the main buttons out of the menu
    private void goBack() {
        if (!isOnMainButtons()) {
            showMainButtons();
        } else {
            closeMenu();
        }
    }

    public void show() {
        buildMenu();
        setVisibility(VISIBLE);

        invalidate();
        requestLayout();

        requestFocus();

        post(() -> {
            if (!verticalButtons.isEmpty()) {
                setVerticalIndex(verticalButtons.size() - 1);
            }
        });

        // The host's commands can depend on what's running there now
        if (actionListener != null) {
            actionListener.onMenuShown();
        }
    }

    public void hide(Runnable onComplete) {
        setVisibility(GONE);
        optionsAction = -1;
        requestedOptionsAction = -1;
        optionPicker = null;
        group = -1;
        disconnectPage = false;
        runningHostAction = -1;
        removeCallbacks(messageTimeout);
        messageAction = -1;
        if (onComplete != null) {
            onComplete.run();
        }
    }
}
