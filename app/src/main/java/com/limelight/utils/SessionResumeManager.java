package com.limelight.utils;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Base64;
import android.util.Log;

import com.limelight.Game;

public class SessionResumeManager {
    private static final String PREFS_NAME = "SessionResume";
    private static final String KEY_PENDING       = "pending";
    private static final String KEY_HOST          = "host";
    private static final String KEY_PORT          = "port";
    private static final String KEY_HTTPS_PORT    = "httpsPort";
    private static final String KEY_APP_NAME      = "appName";
    private static final String KEY_APP_ID        = "appId";
    private static final String KEY_APP_HDR       = "appHdr";
    private static final String KEY_UNIQUE_ID     = "uniqueId";
    private static final String KEY_PC_UUID       = "pcUuid";
    private static final String KEY_PC_NAME       = "pcName";
    private static final String KEY_SERVER_CERT   = "serverCert";
    private static final String KEY_QUICK_LAUNCH  = "quickLaunchKey";
    private static final String KEY_APPLY_OVERRIDES = "applyOverrides";

    public static void save(Context ctx, Intent gameIntent) {
        Log.d("SessionResume", "save() — appId=" + gameIntent.getIntExtra(Game.EXTRA_APP_ID, -1)
                + " pcUuid=" + gameIntent.getStringExtra(Game.EXTRA_PC_UUID)
                + " quickLaunchKey=" + gameIntent.getStringExtra(Game.EXTRA_QUICK_LAUNCH_APP_KEY));
        SharedPreferences.Editor editor =
                ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit();

        editor.putString(KEY_HOST,       gameIntent.getStringExtra(Game.EXTRA_HOST));
        editor.putInt(KEY_PORT,          gameIntent.getIntExtra(Game.EXTRA_PORT, 0));
        editor.putInt(KEY_HTTPS_PORT,    gameIntent.getIntExtra(Game.EXTRA_HTTPS_PORT, 0));
        editor.putString(KEY_APP_NAME,   gameIntent.getStringExtra(Game.EXTRA_APP_NAME));
        editor.putInt(KEY_APP_ID,        gameIntent.getIntExtra(Game.EXTRA_APP_ID, 0));
        editor.putBoolean(KEY_APP_HDR,   gameIntent.getBooleanExtra(Game.EXTRA_APP_HDR, false));
        editor.putString(KEY_UNIQUE_ID,  gameIntent.getStringExtra(Game.EXTRA_UNIQUEID));
        editor.putString(KEY_PC_UUID,    gameIntent.getStringExtra(Game.EXTRA_PC_UUID));
        editor.putString(KEY_PC_NAME,    gameIntent.getStringExtra(Game.EXTRA_PC_NAME));
        editor.putBoolean(KEY_APPLY_OVERRIDES,
                gameIntent.getBooleanExtra(Game.EXTRA_APPLY_PREFERENCE_OVERRIDES, false));

        String quickLaunchKey = gameIntent.getStringExtra(Game.EXTRA_QUICK_LAUNCH_APP_KEY);
        if (quickLaunchKey != null) {
            editor.putString(KEY_QUICK_LAUNCH, quickLaunchKey);
        } else {
            editor.remove(KEY_QUICK_LAUNCH);
        }

        byte[] cert = gameIntent.getByteArrayExtra(Game.EXTRA_SERVER_CERT);
        if (cert != null) {
            editor.putString(KEY_SERVER_CERT, Base64.encodeToString(cert, Base64.DEFAULT));
        } else {
            editor.remove(KEY_SERVER_CERT);
        }

        editor.putBoolean(KEY_PENDING, true);
        editor.apply();
    }

    public static boolean hasPendingSession(Context ctx) {
        return ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getBoolean(KEY_PENDING, false);
    }

    public static String getPendingPcUuid(Context ctx) {
        return ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getString(KEY_PC_UUID, null);
    }

    public static int getPendingAppId(Context ctx) {
        return ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getInt(KEY_APP_ID, 0);
    }

    public static Intent buildResumeIntent(Context ctx) {
        SharedPreferences prefs = ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        Intent intent = new Intent(ctx, Game.class);

        intent.putExtra(Game.EXTRA_HOST,       prefs.getString(KEY_HOST, null));
        intent.putExtra(Game.EXTRA_PORT,       prefs.getInt(KEY_PORT, 0));
        intent.putExtra(Game.EXTRA_HTTPS_PORT, prefs.getInt(KEY_HTTPS_PORT, 0));
        intent.putExtra(Game.EXTRA_APP_NAME,   prefs.getString(KEY_APP_NAME, null));
        intent.putExtra(Game.EXTRA_APP_ID,     prefs.getInt(KEY_APP_ID, 0));
        intent.putExtra(Game.EXTRA_APP_HDR,    prefs.getBoolean(KEY_APP_HDR, false));
        intent.putExtra(Game.EXTRA_UNIQUEID,   prefs.getString(KEY_UNIQUE_ID, null));
        intent.putExtra(Game.EXTRA_PC_UUID,    prefs.getString(KEY_PC_UUID, null));
        intent.putExtra(Game.EXTRA_PC_NAME,    prefs.getString(KEY_PC_NAME, null));
        intent.putExtra(Game.EXTRA_APPLY_PREFERENCE_OVERRIDES,
                prefs.getBoolean(KEY_APPLY_OVERRIDES, false));

        String quickLaunchKey = prefs.getString(KEY_QUICK_LAUNCH, null);
        if (quickLaunchKey != null) {
            intent.putExtra(Game.EXTRA_QUICK_LAUNCH_APP_KEY, quickLaunchKey);
        }

        String certBase64 = prefs.getString(KEY_SERVER_CERT, null);
        if (certBase64 != null) {
            intent.putExtra(Game.EXTRA_SERVER_CERT, Base64.decode(certBase64, Base64.DEFAULT));
        }

        return intent;
    }

    public static void clear(Context ctx) {
        Log.d("SessionResume", "clear()");
        ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit()
                .clear()
                .apply();
    }
}
