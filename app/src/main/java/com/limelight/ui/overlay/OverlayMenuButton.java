package com.limelight.ui.overlay;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Custom view representing a single button in the overlay menu.
 * Displays an icon on the left with an optional text label to the right.
 */
public class OverlayMenuButton extends LinearLayout {
    private static final int ICON_SIZE_DP = 24;
    private static final int PADDING_DP = 12;
    private static final int TEXT_SIZE_SP = 12;
    private static final int CORNER_RADIUS_DP = 8;

    private ImageView iconView;
    private TextView labelView;
    private GradientDrawable background;
    private boolean isSelected = false;
    private int strokeWidth;

    public OverlayMenuButton(Context context) {
        super(context);
        init(context);
    }

    public OverlayMenuButton(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context);
    }

    public OverlayMenuButton(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init(context);
    }

    private void init(Context context) {
        setOrientation(HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);

        float density = context.getResources().getDisplayMetrics().density;
        int padding = (int) (PADDING_DP * density);
        int iconSize = (int) (ICON_SIZE_DP * density);

        // Cache stroke width to avoid recalculating on every appearance update
        strokeWidth = (int) (2 * density);

        setPadding(padding, padding, padding, padding);
        setClickable(true);
        setFocusable(true);

        // Create background drawable
        background = new GradientDrawable();
        background.setShape(GradientDrawable.RECTANGLE);
        background.setCornerRadius(CORNER_RADIUS_DP * density);
        background.setColor(0xD9000000); // Semi-transparent black
        setBackground(background);

        // Create icon view
        iconView = new ImageView(context);
        LinearLayout.LayoutParams iconParams = new LinearLayout.LayoutParams(iconSize, iconSize);
        iconParams.gravity = Gravity.CENTER_VERTICAL;
        iconView.setLayoutParams(iconParams);
        iconView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        addView(iconView);

        // Create label view
        labelView = new TextView(context);
        LinearLayout.LayoutParams labelParams = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        );
        labelParams.leftMargin = (int) (8 * density);
        labelView.setLayoutParams(labelParams);
        labelView.setTextSize(TypedValue.COMPLEX_UNIT_SP, TEXT_SIZE_SP);
        labelView.setTextColor(Color.WHITE);
        labelView.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
        addView(labelView);
    }

    /**
     * Set the button icon from a drawable resource ID
     */
    public void setIcon(int iconResId) {
        Drawable icon = getContext().getResources().getDrawable(iconResId);
        iconView.setImageDrawable(icon);
    }

    /**
     * Set the button icon from a drawable
     */
    public void setIcon(Drawable icon) {
        iconView.setImageDrawable(icon);
    }

    /**
     * Set the button label text
     */
    public void setLabel(String text) {
        labelView.setText(text);
        labelView.setVisibility(text != null && !text.isEmpty() ? VISIBLE : GONE);
    }

    /**
     * Set whether this button is currently selected
     */
    public void setSelected(boolean selected) {
        // Skip redundant updates to avoid unnecessary redraws
        if (isSelected == selected) {
            return;
        }
        isSelected = selected;
        updateAppearance();
    }

    @Override
    public boolean isSelected() {
        return isSelected;
    }

    /**
     * Update the visual appearance based on selection state
     */
    private void updateAppearance() {
        if (isSelected) {
            // Highlighted state - brighter background
            background.setColor(0xC0FFFFFF); // Semi-transparent white
            background.setStroke(strokeWidth, 0xFFFFFFFF); // White border
            iconView.setImageTintList(ColorStateList.valueOf(0xFF000000)); // Black icon
            labelView.setTextColor(Color.BLACK);
        } else {
            // Normal state
            background.setColor(0xD9000000); // Semi-transparent black
            background.setStroke(0, 0); // No border
            iconView.setImageTintList(ColorStateList.valueOf(0xFFFFFFFF)); // White icon
            labelView.setTextColor(Color.WHITE);
        }
    }

    /**
     * Convenience method to create a button with icon and label
     */
    public static OverlayMenuButton create(Context context, int iconResId, String label) {
        OverlayMenuButton button = new OverlayMenuButton(context);
        button.setIcon(iconResId);
        button.setLabel(label);
        return button;
    }
}
