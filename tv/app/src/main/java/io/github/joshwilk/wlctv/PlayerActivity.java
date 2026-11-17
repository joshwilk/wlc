/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.ClipDrawable;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.videolan.libvlc.util.VLCVideoLayout;

/**
 * Full-screen player. The remote works without any buttons on screen
 * (OK = play/pause, left/right = -10 s / +30 s, up/down = show controls,
 * media keys), and everything stays in step with the device that cast it.
 */
public class PlayerActivity extends Activity {
    private static final long HIDE_MS = 5000;

    private VlcPlayer player;
    private VLCVideoLayout video;
    private LinearLayout art, overlay;
    private TextView artTitle, title, sub, clock, playBtn, toast;
    private ProgressBar bar;
    private final Handler h = new Handler(Looper.getMainLooper());
    private final Runnable hide = () -> overlay.setVisibility(View.GONE);
    private final Runnable hideToast = () -> toast.setVisibility(View.GONE);
    private final Runnable update = this::refresh;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        player = VlcPlayer.get(this);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        video = new VLCVideoLayout(this);
        root.addView(video, new FrameLayout.LayoutParams(-1, -1));

        // music: barrel + title instead of a black screen
        art = new LinearLayout(this);
        art.setOrientation(LinearLayout.VERTICAL);
        art.setGravity(Gravity.CENTER);
        art.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[]{0xFF24180F, Color.BLACK}));
        ImageView barrel = new ImageView(this);
        barrel.setImageResource(R.drawable.barrel);
        art.addView(barrel, new LinearLayout.LayoutParams(Ui.dp(this, 160), Ui.dp(this, 192)));
        artTitle = Ui.text(this, "", 30, Ui.TEXT, true);
        artTitle.setGravity(Gravity.CENTER);
        artTitle.setPadding(Ui.dp(this, 40), Ui.dp(this, 24), Ui.dp(this, 40), 0);
        art.addView(artTitle);
        root.addView(art, new FrameLayout.LayoutParams(-1, -1));

        // controls along the bottom
        overlay = new LinearLayout(this);
        overlay.setOrientation(LinearLayout.VERTICAL);
        overlay.setPadding(Ui.dp(this, 48), Ui.dp(this, 40), Ui.dp(this, 48), Ui.dp(this, 32));
        overlay.setBackground(new GradientDrawable(GradientDrawable.Orientation.BOTTOM_TOP,
                new int[]{0xE6000000, 0x00000000}));
        title = Ui.text(this, "", 26, Ui.TEXT, true);
        title.setSingleLine(true);
        overlay.addView(title);
        sub = Ui.text(this, "", 16, Ui.MUTE, false);
        overlay.addView(sub);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        ClipDrawable fillPart = new ClipDrawable(new ColorDrawable(Ui.ORANGE), Gravity.START, ClipDrawable.HORIZONTAL);
        LayerDrawable track = new LayerDrawable(new android.graphics.drawable.Drawable[]{
                new ColorDrawable(0xFF3A3F48), fillPart});
        track.setId(0, android.R.id.background);
        track.setId(1, android.R.id.progress);
        bar.setProgressDrawable(track);
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(-1, Ui.dp(this, 6));
        bp.setMargins(0, Ui.dp(this, 14), 0, Ui.dp(this, 6));
        overlay.addView(bar, bp);
        clock = Ui.text(this, "", 16, Ui.MUTE, false);
        overlay.addView(clock);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(Ui.button(this, "⏮", v -> player.doControl("prev", 0)), Ui.wrap(this, 14));
        row.addView(Ui.button(this, "−10 s", v -> player.doControl("seekrel", -10)), Ui.wrap(this, 14));
        playBtn = Ui.button(this, "Pause", v -> player.doControl("toggle", 0));
        row.addView(playBtn, Ui.wrap(this, 14));
        row.addView(Ui.button(this, "+30 s", v -> player.doControl("seekrel", 30)), Ui.wrap(this, 14));
        row.addView(Ui.button(this, "⏭", v -> player.doControl("next", 0)), Ui.wrap(this, 14));
        row.addView(Ui.button(this, "Stop", v -> player.doControl("stop", 0)), Ui.wrap(this, 14));
        overlay.addView(row);
        FrameLayout.LayoutParams op = new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM);
        root.addView(overlay, op);

        // short feedback for remote presses while the controls are hidden
        toast = Ui.text(this, "", 22, Color.WHITE, true);
        toast.setBackground(Ui.round(this, 0xCC000000, 0, 14));
        toast.setPadding(Ui.dp(this, 22), Ui.dp(this, 12), Ui.dp(this, 22), Ui.dp(this, 12));
        toast.setVisibility(View.GONE);
        FrameLayout.LayoutParams tp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.END);
        tp.setMargins(0, Ui.dp(this, 36), Ui.dp(this, 48), 0);
        root.addView(toast, tp);

        setContentView(root);
        showControls();
    }

    @Override
    protected void onNewIntent(android.content.Intent i) {
        super.onNewIntent(i);
        showControls();   // a new cast arrived while the player was open
    }

    @Override
    protected void onStart() {
        super.onStart();
        player.attach(video);
        player.addListener(update);
        refresh();
    }

    @Override
    protected void onStop() {
        player.removeListener(update);
        player.detach();
        super.onStop();
    }

    private void refresh() {
        if (!player.active() && !player.state().equals("error")) {
            finish();
            return;
        }
        boolean music = !player.hasVideo() && player.state().equals("playing") || player.state().equals("error");
        art.setVisibility(music ? View.VISIBLE : View.GONE);
        artTitle.setText(player.state().equals("error") ? player.error() : player.title());
        title.setText(player.title());
        StringBuilder s = new StringBuilder();
        if (!player.from().isEmpty()) s.append("From ").append(player.from());
        if (player.count() > 1) s.append(s.length() > 0 ? " · " : "").append(player.index() + 1).append(" of ").append(player.count());
        if (player.state().equals("opening")) s.append(s.length() > 0 ? " · " : "").append("Opening…");
        sub.setText(s);
        double len = player.length(), t = player.time();
        bar.setProgress(len > 0 ? (int) (1000 * Math.min(1, t / len)) : 0);
        clock.setText(Ui.clock(t) + " / " + (len > 0 ? Ui.clock(len) : "--:--"));
        playBtn.setText(player.state().equals("paused") ? "Play" : "Pause");
    }

    private void showControls() {
        boolean wasHidden = overlay.getVisibility() != View.VISIBLE;
        overlay.setVisibility(View.VISIBLE);
        if (wasHidden || getCurrentFocus() == null) playBtn.requestFocus();
        h.removeCallbacks(hide);
        h.postDelayed(hide, HIDE_MS);
    }

    private void flash(String s) {
        toast.setText(s);
        toast.setVisibility(View.VISIBLE);
        h.removeCallbacks(hideToast);
        h.postDelayed(hideToast, 1200);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getAction() != KeyEvent.ACTION_DOWN) return super.dispatchKeyEvent(e);
        boolean shown = overlay.getVisibility() == View.VISIBLE;
        switch (e.getKeyCode()) {
            // media keys work whatever is on screen
            case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:
            case KeyEvent.KEYCODE_SPACE:
                player.doControl("toggle", 0);
                flash(player.state().equals("paused") ? "Paused" : "Playing");
                return true;
            case KeyEvent.KEYCODE_MEDIA_PLAY:
                player.doControl("resume", 0);
                return true;
            case KeyEvent.KEYCODE_MEDIA_PAUSE:
                player.doControl("pause", 0);
                return true;
            case KeyEvent.KEYCODE_MEDIA_FAST_FORWARD:
                player.doControl("seekrel", 30);
                flash("+30 s");
                return true;
            case KeyEvent.KEYCODE_MEDIA_REWIND:
                player.doControl("seekrel", -10);
                flash("−10 s");
                return true;
            case KeyEvent.KEYCODE_MEDIA_NEXT:
                player.doControl("next", 0);
                return true;
            case KeyEvent.KEYCODE_MEDIA_PREVIOUS:
                player.doControl("prev", 0);
                return true;
            case KeyEvent.KEYCODE_MEDIA_STOP:
                player.doControl("stop", 0);
                return true;
            case KeyEvent.KEYCODE_BACK:
                if (shown) { overlay.setVisibility(View.GONE); return true; }
                player.doControl("stop", 0);
                finish();
                return true;
        }
        if (!shown) {
            switch (e.getKeyCode()) {
                case KeyEvent.KEYCODE_DPAD_CENTER:
                case KeyEvent.KEYCODE_ENTER:
                    player.doControl("toggle", 0);
                    flash(player.state().equals("paused") ? "Paused" : "Playing");
                    return true;
                case KeyEvent.KEYCODE_DPAD_LEFT:
                    player.doControl("seekrel", -10);
                    flash("−10 s   " + Ui.clock(player.time()));
                    return true;
                case KeyEvent.KEYCODE_DPAD_RIGHT:
                    player.doControl("seekrel", 30);
                    flash("+30 s   " + Ui.clock(player.time()));
                    return true;
                case KeyEvent.KEYCODE_DPAD_UP:
                case KeyEvent.KEYCODE_DPAD_DOWN:
                case KeyEvent.KEYCODE_MENU:
                    showControls();
                    return true;
            }
        }
        showControls();   // any key keeps the controls up while you use them
        return super.dispatchKeyEvent(e);
    }
}
