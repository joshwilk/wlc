/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv;

import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;

import org.videolan.libvlc.LibVLC;
import org.videolan.libvlc.Media;
import org.videolan.libvlc.MediaPlayer;
import org.videolan.libvlc.util.VLCVideoLayout;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import io.github.joshwilk.wlctv.core.Json;
import io.github.joshwilk.wlctv.core.Player;

/**
 * libVLC behind the WilkCast Player interface. Casts and remote commands from
 * the network and key presses on the TV all go through here, on the main
 * thread, so the sender and the TV always see the same state.
 */
final class VlcPlayer implements Player {
    private static VlcPlayer instance;

    static synchronized VlcPlayer get(Context c) {
        if (instance == null) instance = new VlcPlayer(c.getApplicationContext());
        return instance;
    }

    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Runnable> listeners = new CopyOnWriteArrayList<>();
    private LibVLC lib;
    private MediaPlayer mp;
    private final Context ctx;

    // state, written on the main thread, read from anywhere
    private volatile String state = "stopped", title = "", from = "", error = "";
    private volatile double time, length;
    private volatile int volume = 100;
    private volatile boolean hasVideo;
    private final List<Map<String, Object>> queue = new ArrayList<>();
    private volatile int index = -1;

    private VlcPlayer(Context c) {
        ctx = c;
        runOnMain(() -> {
            ArrayList<String> opts = new ArrayList<>();
            opts.add("--http-reconnect");
            opts.add("--audio-time-stretch");
            lib = new LibVLC(ctx, opts);
            mp = new MediaPlayer(lib);
            mp.setEventListener(this::onEvent);
        });
    }

    /* ------------------------------------------------------------- listeners */

    void addListener(Runnable r) { listeners.add(r); }

    void removeListener(Runnable r) { listeners.remove(r); }

    private void changed() {
        for (Runnable r : listeners) r.run();
    }

    /* ------------------------------------------------------------- video */

    void attach(VLCVideoLayout layout) {
        if (mp != null && !mp.getVLCVout().areViewsAttached()) mp.attachViews(layout, null, false, false);
    }

    void detach() {
        if (mp != null && mp.getVLCVout().areViewsAttached()) mp.detachViews();
    }

    boolean hasVideo() { return hasVideo; }
    boolean active() { return !state.equals("stopped"); }
    String state() { return state; }
    String title() { return title; }
    String from() { return from; }
    String error() { return error; }
    double time() { return time; }
    double length() { return length; }
    int index() { return index; }
    int count() { synchronized (queue) { return queue.size(); } }

    /* ------------------------------------------------------------- Player */

    @Override
    public void load(String url, String t, double position, List<Map<String, Object>> rest, String sender) {
        final List<Map<String, Object>> q = new ArrayList<>();
        q.add(Json.map("url", url, "title", t));
        q.addAll(rest);
        runOnMainAndWait(() -> {
            synchronized (queue) {
                queue.clear();
                queue.addAll(q);
            }
            from = sender == null ? "" : sender;
            playIndex(0, position);
        });
    }

    @Override
    public void control(String cmd, double v) {
        runOnMainAndWait(() -> doControl(cmd, v));
    }

    @Override
    public Map<String, Object> status() {
        return Json.map("state", state, "time", time, "length", length, "volume", volume / 100.0,
                "title", title, "index", index, "count", count(), "error", error);
    }

    /* ------------------------------------------------------------- main thread */

    private void playIndex(int i, double position) {
        Map<String, Object> item;
        synchronized (queue) {
            if (i < 0 || i >= queue.size()) { stopNow(); return; }
            item = queue.get(i);
        }
        index = i;
        String url = Json.str(item, "url");
        String t = Json.str(item, "title");
        if (t == null || t.isEmpty()) {
            String p = Uri.parse(url).getLastPathSegment();
            t = p == null ? url : p;
        }
        title = t;
        error = "";
        time = position;
        length = 0;
        hasVideo = false;
        state = "opening";
        Media m = new Media(lib, Uri.parse(url));
        m.addOption(":network-caching=1500");
        if (position > 0) m.addOption(String.format(Locale.ROOT, ":start-time=%.2f", position));
        mp.setMedia(m);
        m.release();
        mp.setVolume(volume);
        mp.play();
        changed();
    }

    private void stopNow() {
        if (mp != null) mp.stop();
        state = "stopped";
        index = -1;
        time = 0;
        length = 0;
        synchronized (queue) { queue.clear(); }
        changed();
    }

    void doControl(String cmd, double v) {
        if (mp == null) return;
        switch (cmd) {
            case "play": case "resume":
                if (state.equals("paused")) mp.play();
                break;
            case "pause":
                if (state.equals("playing") || state.equals("opening")) mp.pause();
                break;
            case "toggle":
                if (state.equals("playing")) mp.pause();
                else if (state.equals("paused")) mp.play();
                break;
            case "stop":
                stopNow();
                return;
            case "seek":
                seekTo(v);
                break;
            case "seekrel":
                seekTo(time + v);
                break;
            case "volume":
                volume = (int) Math.round(Math.max(0, Math.min(2, v)) * 100);
                mp.setVolume(volume);
                break;
            case "next":
                if (index + 1 < count()) playIndex(index + 1, 0);
                break;
            case "prev":
                if (time > 3 || index <= 0) seekTo(0);
                else playIndex(index - 1, 0);
                break;
        }
        changed();
    }

    private void seekTo(double secs) {
        double t = Math.max(0, length > 0 ? Math.min(secs, length - 1) : secs);
        mp.setTime((long) (t * 1000));
        time = t;
    }

    private void onEvent(MediaPlayer.Event e) {
        switch (e.type) {
            case MediaPlayer.Event.Opening:
                state = "opening";
                break;
            case MediaPlayer.Event.Playing:
                state = "playing";
                break;
            case MediaPlayer.Event.Paused:
                state = "paused";
                break;
            case MediaPlayer.Event.TimeChanged:
                time = e.getTimeChanged() / 1000.0;
                break;
            case MediaPlayer.Event.LengthChanged:
                length = e.getLengthChanged() / 1000.0;
                break;
            case MediaPlayer.Event.Vout:
                hasVideo = e.getVoutCount() > 0;
                break;
            case MediaPlayer.Event.EndReached:
                if (index + 1 < count()) { playIndex(index + 1, 0); return; }
                stopNow();
                return;
            case MediaPlayer.Event.EncounteredError:
                error = "could not play " + title;
                if (index + 1 < count()) { playIndex(index + 1, 0); return; }
                state = "error";
                break;
            default:
                return;
        }
        if (e.type == MediaPlayer.Event.TimeChanged && state.equals("opening")) state = "playing";
        changed();
    }

    /* ------------------------------------------------------------- threads */

    private void runOnMain(Runnable r) {
        if (Looper.myLooper() == Looper.getMainLooper()) r.run();
        else main.post(r);
    }

    /** Runs on the main thread and waits (briefly) so the reply carries the new state. */
    private void runOnMainAndWait(Runnable r) {
        if (Looper.myLooper() == Looper.getMainLooper()) { r.run(); return; }
        CountDownLatch done = new CountDownLatch(1);
        main.post(() -> { try { r.run(); } finally { done.countDown(); } });
        try { done.await(3, TimeUnit.SECONDS); } catch (InterruptedException ignored) { }
    }
}
