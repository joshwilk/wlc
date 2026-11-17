/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

import io.github.joshwilk.wlctv.core.CastServer;
import io.github.joshwilk.wlctv.core.Discovery;

/**
 * Keeps this TV announced and reachable as a WilkCast receiver, even while
 * WLC TV is not on screen, and brings the player up when something is cast.
 */
public class ReceiverService extends Service {
    private static final String TAG = "WLC-TV";
    private static volatile ReceiverService running;

    private CastServer server;
    private Discovery discovery;
    private WifiManager.MulticastLock mlock;
    private WifiManager.WifiLock wlock;
    private int port = -1;
    private final Handler main = new Handler(Looper.getMainLooper());

    static ReceiverService running() { return running; }

    int port() { return port; }

    Discovery discovery() { return discovery; }

    @Override
    public void onCreate() {
        super.onCreate();
        startInForeground();
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm != null) {
            mlock = wm.createMulticastLock("wlc-tv");
            mlock.setReferenceCounted(false);
            mlock.acquire();
            wlock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "wlc-tv");
            wlock.setReferenceCounted(false);
            wlock.acquire();
        }
        App.Prefs prefs = App.prefs();
        VlcPlayer player = VlcPlayer.get(this);
        server = new CastServer(prefs, player, (title, from, ip) -> main.post(this::showPlayer));
        // network setup off the main thread
        new Thread(() -> {
            try {
                port = server.start(CastServer.DEFAULT_PORT);
                discovery = new Discovery(prefs, port);
                discovery.start();
                Log.i(TAG, "WilkCast receiver on port " + port + " as \"" + prefs.name() + "\"");
            } catch (Exception e) {
                Log.e(TAG, "could not start the receiver", e);
            }
        }, "wlc-tv-start").start();
        running = this;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    /** Settings changed: tell the network right away. */
    void announce() {
        if (discovery != null) discovery.announceSoon();
    }

    private void showPlayer() {
        Intent i = new Intent(this, PlayerActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        try {
            startActivity(i);
        } catch (RuntimeException e) {
            // Android 10+ may refuse while another app is in front; playback still runs,
            // and the player shows when WLC TV is opened.
            Log.w(TAG, "could not bring the player up", e);
        }
    }

    private void startInForeground() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(new NotificationChannel("receiver", "Cast receiver",
                    NotificationManager.IMPORTANCE_LOW));
            b = new Notification.Builder(this, "receiver");
        } else {
            b = new Notification.Builder(this);
        }
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = b.setContentTitle("WLC TV")
                .setContentText("Ready for WilkCast")
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentIntent(open)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        else startForeground(1, n);
    }

    @Override
    public void onDestroy() {
        running = null;
        new Thread(() -> {
            if (discovery != null) discovery.stop();
            if (server != null) server.stop();
        }).start();
        if (mlock != null) mlock.release();
        if (wlock != null) wlock.release();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
