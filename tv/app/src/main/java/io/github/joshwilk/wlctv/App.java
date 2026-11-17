/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv;

import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.provider.Settings;

import java.security.SecureRandom;
import java.util.Locale;

import io.github.joshwilk.wlctv.core.CastServer;

public class App extends Application {
    private static App app;
    private Prefs prefs;

    @Override
    public void onCreate() {
        super.onCreate();
        app = this;
        prefs = new Prefs(this);
    }

    public static App get() { return app; }

    public static Prefs prefs() { return app.prefs; }

    public static void startReceiver(Context c) {
        Intent i = new Intent(c, ReceiverService.class);
        if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
        else c.startService(i);
    }

    /** This TV's WilkCast identity and settings. */
    public static final class Prefs implements CastServer.Config {
        private final SharedPreferences sp;
        private final Context ctx;

        Prefs(Context c) {
            ctx = c;
            sp = c.getSharedPreferences("wlctv", MODE_PRIVATE);
            if (sp.getString("id", null) == null) {
                sp.edit().putString("id", randomHex(16)).putString("pin", newPin()).apply();
            }
        }

        @Override public String id() { return sp.getString("id", ""); }

        @Override public String name() {
            String n = sp.getString("name", null);
            if (n != null && !n.isEmpty()) return n;
            String dev = null;
            try { dev = Settings.Global.getString(ctx.getContentResolver(), "device_name"); } catch (Exception ignored) { }
            if (dev == null || dev.isEmpty()) dev = Build.MODEL.startsWith("AFT") ? "Fire TV" : Build.MODEL;
            return dev;
        }

        @Override public String pin() { return sp.getString("pin", "0000"); }

        @Override public String auth() { return sp.getString("auth", "pin"); }

        public boolean bootStart() { return sp.getBoolean("boot", true); }

        public void setName(String n) { sp.edit().putString("name", n).apply(); }

        public void setPin(String p) { sp.edit().putString("pin", p).apply(); }

        public void setAuth(String a) { sp.edit().putString("auth", a).apply(); }

        public void setBootStart(boolean b) { sp.edit().putBoolean("boot", b).apply(); }

        public String newPinNow() {
            String p = newPin();
            setPin(p);
            return p;
        }

        /** Sign-in cookie for a WLC (by its id), so the TV pairs once. */
        public String cookieFor(String wlcId) { return sp.getString("cookie." + wlcId, null); }

        public void setCookie(String wlcId, String cookie) {
            sp.edit().putString("cookie." + wlcId, cookie).apply();
        }

        /** WLCs added by address (when discovery can't see them): "host:port" lines. */
        public String manualWlcs() { return sp.getString("manual", ""); }

        public void addManualWlc(String hostPort) {
            String cur = manualWlcs();
            if (!("\n" + cur + "\n").contains("\n" + hostPort + "\n"))
                sp.edit().putString("manual", cur.isEmpty() ? hostPort : cur + "\n" + hostPort).apply();
        }

        private static String newPin() {
            return String.format(Locale.ROOT, "%04d", new SecureRandom().nextInt(10000));
        }

        private static String randomHex(int bytes) {
            byte[] b = new byte[bytes];
            new SecureRandom().nextBytes(b);
            StringBuilder s = new StringBuilder();
            for (byte x : b) s.append(String.format(Locale.ROOT, "%02x", x));
            return s.toString();
        }
    }
}
