package com.limelight.ui.overlay;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.PorterDuffColorFilter;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;

import com.limelight.LimeLog;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * Icons from the Material Symbols font bundled in the app's assets (the set Sunshine's client
 * commands and the Qt client use), named as "material:<name>".
 */
public final class MaterialIcons {
    private static final String PREFIX = "material:";
    private static final int ICON_SIZE_DP = 24;

    private static Typeface typeface;
    private static Map<String, String> glyphs;

    private MaterialIcons() {
    }

    private static synchronized boolean load(Context context) {
        if (glyphs != null) {
            return typeface != null;
        }

        glyphs = new HashMap<>();
        try {
            typeface = Typeface.createFromAsset(context.getAssets(), "material-symbols/MaterialSymbolsOutlined.ttf");
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    context.getAssets().open("material-symbols/codepoints.txt"), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    String[] parts = line.trim().split("\\s+");
                    if (parts.length == 2) {
                        glyphs.put(parts[0], new String(Character.toChars(Integer.parseInt(parts[1], 16))));
                    }
                }
            }
        } catch (Exception e) {
            LimeLog.warning("Couldn't load the Material Symbols icons: " + e);
            typeface = null;
        }
        return typeface != null;
    }

    /**
     * Returns a drawable for an icon such as "material:pause_circle", or null for anything else
     * or a name the font doesn't have. It takes the tint of the ImageView showing it.
     */
    public static Drawable drawable(Context context, String icon) {
        if (icon == null || !icon.startsWith(PREFIX) || !load(context)) {
            return null;
        }
        String glyph = glyphs.get(icon.substring(PREFIX.length()));
        if (glyph == null) {
            return null;
        }
        int size = (int) (ICON_SIZE_DP * context.getResources().getDisplayMetrics().density);
        return new GlyphDrawable(typeface, glyph, size);
    }

    /** One glyph of the font, drawn to fill its bounds */
    private static final class GlyphDrawable extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final String glyph;
        private final int size;
        private ColorStateList tint;
        private ColorFilter colorFilter;

        GlyphDrawable(Typeface typeface, String glyph, int size) {
            this.glyph = glyph;
            this.size = size;
            paint.setTypeface(typeface);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setColor(0xFFFFFFFF);
        }

        @Override
        public void draw(Canvas canvas) {
            Rect bounds = getBounds();
            if (bounds.isEmpty()) {
                return;
            }

            paint.setTextSize(Math.min(bounds.width(), bounds.height()));
            paint.setColor(tint != null ? tint.getColorForState(getState(), tint.getDefaultColor()) : 0xFFFFFFFF);
            paint.setColorFilter(colorFilter);

            Paint.FontMetrics metrics = paint.getFontMetrics();
            float baseline = bounds.exactCenterY() - (metrics.ascent + metrics.descent) / 2;
            canvas.drawText(glyph, bounds.exactCenterX(), baseline, paint);
        }

        @Override
        public void setTintList(ColorStateList tint) {
            this.tint = tint;
            invalidateSelf();
        }

        @Override
        public void setTintMode(PorterDuff.Mode tintMode) {
            // The tint is the glyph's color
        }

        @Override
        public boolean isStateful() {
            return tint != null && tint.isStateful();
        }

        @Override
        protected boolean onStateChange(int[] state) {
            invalidateSelf();
            return true;
        }

        @Override
        public void setAlpha(int alpha) {
            paint.setAlpha(alpha);
            invalidateSelf();
        }

        @Override
        public void setColorFilter(ColorFilter colorFilter) {
            this.colorFilter = colorFilter;
            invalidateSelf();
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }

        @Override
        public int getIntrinsicWidth() {
            return size;
        }

        @Override
        public int getIntrinsicHeight() {
            return size;
        }
    }
}
