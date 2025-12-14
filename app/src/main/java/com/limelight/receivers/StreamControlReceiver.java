package com.limelight.receivers;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import com.limelight.Game;

/**
 * BroadcastReceiver that allows external apps to quit the currently running stream.
 * This is equivalent to the user pressing Ctrl+Alt+Shift+Q or the gamepad quit gesture.
 *
 * Supported actions:
 * - com.limelight.QUIT_STREAM: Quit the currently running stream
 *   Optional extras:
 *   - EXTRA_QUIT_MOONLIGHT (boolean): If true, closes the entire Moonlight app after quitting the stream
 *
 * Example usage from external app:
 *   Intent intent = new Intent("com.limelight.QUIT_STREAM");
 *   intent.setPackage("com.limelight.mlsoft"); // or com.limelight.mlsoft.debug for debug builds
 *   intent.putExtra("quit_moonlight", true); // Optional: close entire app
 *   sendBroadcast(intent);
 */
public class StreamControlReceiver extends BroadcastReceiver {
    private static final String TAG = "StreamControlReceiver";

    public static final String ACTION_QUIT_STREAM = "com.limelight.QUIT_STREAM";
    public static final String EXTRA_QUIT_MOONLIGHT = "quit_moonlight";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || intent.getAction() == null) {
            return;
        }

        if (ACTION_QUIT_STREAM.equals(intent.getAction())) {
            boolean quitMoonlight = intent.getBooleanExtra(EXTRA_QUIT_MOONLIGHT, false);

            Log.i(TAG, "Quit stream requested - finishing Game activity" +
                    (quitMoonlight ? " and closing Moonlight" : ""));

            // Send broadcast to finish the Game activity (same as Ctrl+Alt+Shift+Q)
            Intent finishIntent = new Intent(Game.ACTION_QUIT_APP);
            finishIntent.setPackage(context.getPackageName());
            context.sendBroadcast(finishIntent);

            // If requested, close the entire Moonlight app after a short delay
            if (quitMoonlight) {
                new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        closeMoonlightApp(context);
                    }
                }, 1000); // 1 second delay to allow Game activity to finish
            }
        }
    }

    private void closeMoonlightApp(Context context) {
        Log.i(TAG, "Closing Moonlight app");

        // Move to home screen
        Intent homeIntent = new Intent(Intent.ACTION_MAIN);
        homeIntent.addCategory(Intent.CATEGORY_HOME);
        homeIntent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        context.startActivity(homeIntent);

        // Kill the process after a brief delay
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override
            public void run() {
                android.os.Process.killProcess(android.os.Process.myPid());
            }
        }, 500);
    }
}
