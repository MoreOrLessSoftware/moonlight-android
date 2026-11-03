package com.limelight.binding.video;

import android.content.Context;
import android.content.SharedPreferences;

import com.limelight.LimeLog;

import java.util.Locale;

/**
 * How fast each host's clock runs against ours, remembered from its last stream.
 *
 * The host paces its frames on its own clock, so asked for 71.8274 fps it sends them at that
 * rate times its clock's error as we see it. Two crystals are commonly tens of ppm apart (60 ppm
 * between an iPlay 70 mini Ultra and a Windows host), and at 72 fps on 144 Hz frames then drift a
 * whole vsync against ours every few minutes, each time a frame held long or short and the
 * latency stepping by a vsync. The error is measured over each stream with clock sync
 * (MoonBridge.getHostClockRate()), and the next stream asks for a rate that undoes it.
 */
public final class HostClockRates {
    private static final String PREFS = "HostClockRate";

    // Shorter streams measure too little to replace what's known. Over 30 s the line through
    // clock sync is already good to a couple of ppm, and a short first stream still calibrates.
    private static final double MIN_SPAN_SECONDS = 30;

    // A change this large is worth telling the user about: a different PC behind the same host,
    // say. Measurements of the same host differ by a few ppm (59.2 and 56.8 in one morning).
    private static final double NOTABLE_CHANGE_PPM = 10;

    // Further off than this, something other than the crystals is wrong
    private static final double MAX_PPM = 500;

    private HostClockRates() {
    }

    // The host's clock rate against ours in ppm (positive when it runs fast), or 0 if unknown
    public static double get(Context context, String computerUuid) {
        if (computerUuid == null) {
            return 0;
        }
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getFloat(computerUuid, 0);
    }

    // Whether a rate has been measured for the host
    public static boolean isKnown(Context context, String computerUuid) {
        return computerUuid != null && context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).contains(computerUuid);
    }

    // Remembers the rate measured over the stream that's ending, if it ran long enough. Returns
    // true when it's the first for the host or differs notably from the last, so that the next
    // stream asks for a noticeably different rate.
    public static boolean save(Context context, String computerUuid, double ppm, double spanSeconds) {
        LimeLog.info(String.format(Locale.ROOT, "Host clock runs %+.1f ppm against ours, over %.0f s", ppm, spanSeconds));
        if (computerUuid == null || spanSeconds < MIN_SPAN_SECONDS || Math.abs(ppm) > MAX_PPM) {
            return false;
        }
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        boolean notable = !prefs.contains(computerUuid) ||
                Math.abs(prefs.getFloat(computerUuid, 0) - ppm) > NOTABLE_CHANGE_PPM;
        prefs.edit().putFloat(computerUuid, (float) ppm).apply();
        return notable;
    }

    // The rate to ask a host for, so that its frames come out at streamHz on our clock
    public static double hostRequestHz(double streamHz, double hostClockPpm) {
        return streamHz / (1 + hostClockPpm / 1e6);
    }
}
