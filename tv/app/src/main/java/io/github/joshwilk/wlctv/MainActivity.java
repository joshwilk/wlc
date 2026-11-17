/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

import io.github.joshwilk.wlctv.core.Discovery;

/** Home: who this TV is (name, PIN), the WLCs it can browse, settings. */
public class MainActivity extends Activity {
    private final Handler h = new Handler(Looper.getMainLooper());
    private TextView name, pin, addr, mode;
    private LinearLayout wlcList;
    private TextView nowPlaying;
    private String lastList = "";
    private final Runnable tick = new Runnable() {
        @Override public void run() { refresh(); h.postDelayed(this, 2000); }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        applyTestExtras(getIntent());
        App.startReceiver(this);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Ui.BG);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.HORIZONTAL);
        page.setPadding(Ui.dp(this, 56), Ui.dp(this, 40), Ui.dp(this, 56), Ui.dp(this, 40));
        scroll.addView(page);

        // left: identity
        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.banner);
        logo.setAdjustViewBounds(true);
        left.addView(logo, new LinearLayout.LayoutParams(Ui.dp(this, 300), -2));
        left.addView(Ui.text(this, "Ready for WilkCast as", 18, Ui.MUTE, false), Ui.fill(this, 22));
        name = Ui.text(this, "", 34, Ui.ORANGE2, true);
        left.addView(name);
        pin = Ui.text(this, "", 26, Ui.TEXT, true);
        left.addView(pin, Ui.fill(this, 10));
        mode = Ui.text(this, "", 16, Ui.MUTE, false);
        left.addView(mode);
        addr = Ui.text(this, "", 16, Ui.MUTE, false);
        left.addView(addr, Ui.fill(this, 4));
        TextView how = Ui.text(this, "To cast from your Mac: open WLC, pick something in the Media Center and "
                + "choose Cast, then this TV. You can pause, seek and skip from either end.", 16, Ui.MUTE, false);
        left.addView(how, Ui.fill(this, 18));
        page.addView(left, new LinearLayout.LayoutParams(0, -2, 1f));

        // right: actions
        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        right.setPadding(Ui.dp(this, 48), 0, 0, 0);
        nowPlaying = Ui.button(this, "▶  Now playing", v -> startActivity(new Intent(this, PlayerActivity.class)));
        right.addView(nowPlaying, Ui.fill(this, 0));
        right.addView(Ui.text(this, "Browse a WLC on your network", 18, Ui.MUTE, false), Ui.fill(this, 24));
        wlcList = new LinearLayout(this);
        wlcList.setOrientation(LinearLayout.VERTICAL);
        right.addView(wlcList, Ui.fill(this, 0));
        right.addView(Ui.button(this, "Add a WLC by address…", v -> addByAddress()), Ui.fill(this, 12));
        right.addView(Ui.button(this, "Settings", v -> settings()), Ui.fill(this, 12));
        page.addView(right, new LinearLayout.LayoutParams(0, -2, 1f));

        setContentView(scroll);
    }

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        applyTestExtras(i);
    }

    /** Debug builds only: lets the emulator test set a known name and PIN. */
    private void applyTestExtras(Intent i) {
        if (!BuildConfig.DEBUG || i == null) return;
        if (i.getStringExtra("test_pin") != null) App.prefs().setPin(i.getStringExtra("test_pin"));
        if (i.getStringExtra("test_name") != null) App.prefs().setName(i.getStringExtra("test_name"));
    }

    @Override
    protected void onResume() {
        super.onResume();
        lastList = "";
        h.post(tick);
        ReceiverService s = ReceiverService.running();
        if (s != null && s.discovery() != null) s.discovery().probe();
        maybeAskOverlay();
    }

    @Override
    protected void onPause() {
        h.removeCallbacks(tick);
        super.onPause();
    }

    private void refresh() {
        App.Prefs p = App.prefs();
        name.setText(p.name());
        String a = p.auth();
        pin.setText(a.equals("pin") ? "PIN  " + spaced(p.pin()) : a.equals("open") ? "No PIN needed" : "Not accepting casts");
        mode.setText(a.equals("pin") ? "Senders enter this PIN once." : a.equals("open")
                ? "Anyone on your network can cast here." : "Turn casting back on in Settings.");
        ReceiverService s = ReceiverService.running();
        List<String> ips = Discovery.lanAddresses();
        addr.setText(ips.isEmpty() ? "Not connected to a network" : "This TV: " + ips.get(0)
                + (s != null && s.port() > 0 ? ":" + s.port() : ""));
        nowPlaying.setVisibility(VlcPlayer.get(this).active() ? View.VISIBLE : View.GONE);

        // WLCs heard on the network plus ones added by address
        List<String[]> wlcs = new ArrayList<>();
        if (s != null && s.discovery() != null)
            for (Discovery.Device d : s.discovery().wlcs())
                wlcs.add(new String[]{d.name, d.ip, String.valueOf(d.port), d.id});
        for (String line : p.manualWlcs().split("\n")) {
            if (line.isEmpty()) continue;
            String[] hp = line.split(":");
            boolean dup = false;
            for (String[] w : wlcs) if (w[1].equals(hp[0])) dup = true;
            if (!dup) wlcs.add(new String[]{hp[0], hp[0], hp.length > 1 ? hp[1] : "48620", ""});
        }
        StringBuilder key = new StringBuilder();
        for (String[] w : wlcs) key.append(w[0]).append(w[1]).append(w[2]).append('|');
        if (key.toString().equals(lastList)) return;
        lastList = key.toString();
        boolean hadFocus = wlcList.hasFocus();
        wlcList.removeAllViews();
        if (wlcs.isEmpty()) {
            wlcList.addView(Ui.text(this, "Looking… start WLC on your Mac (same Wi-Fi) and it shows up here.",
                    16, Ui.MUTE, false), Ui.fill(this, 6));
        }
        for (String[] w : wlcs) {
            TextView btn = Ui.button(this, "🖥  " + w[0], v -> browse(w));
            wlcList.addView(btn, Ui.fill(this, 12));
        }
        if (hadFocus && wlcList.getChildCount() > 0) wlcList.getChildAt(0).requestFocus();
        else if (getCurrentFocus() == null && !wlcs.isEmpty()) wlcList.getChildAt(0).requestFocus();
    }

    private void browse(String[] w) {
        startActivity(new Intent(this, BrowseActivity.class)
                .putExtra("name", w[0]).putExtra("host", w[1])
                .putExtra("port", Integer.parseInt(w[2])).putExtra("id", w[3]));
    }

    private static String spaced(String p) {
        StringBuilder s = new StringBuilder();
        for (char c : p.toCharArray()) s.append(c).append(' ');
        return s.toString().trim();
    }

    private void addByAddress() {
        EditText in = new EditText(this);
        in.setHint("192.168.1.20");
        in.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        new AlertDialog.Builder(this)
                .setTitle("WLC's address (shown in WLC ▸ Network & Cast)")
                .setView(in)
                .setPositiveButton("Add", (d, x) -> {
                    String t = in.getText().toString().trim().replaceFirst("^https?://", "").replaceAll("/.*$", "");
                    if (t.isEmpty()) return;
                    App.prefs().addManualWlc(t.contains(":") ? t : t + ":48620");
                    lastList = "";
                    refresh();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void settings() {
        App.Prefs p = App.prefs();
        String auth = p.auth();
        String[] items = {
                "Rename this TV (" + p.name() + ")",
                "Make a new PIN",
                "Who can cast here: " + (auth.equals("pin") ? "anyone with the PIN" : auth.equals("open") ? "anyone" : "nobody"),
                "Start with the TV: " + (p.bootStart() ? "on" : "off"),
        };
        new AlertDialog.Builder(this).setTitle("Settings").setItems(items, (d, which) -> {
            switch (which) {
                case 0: rename(); break;
                case 1:
                    p.newPinNow();
                    Toast.makeText(this, "New PIN: " + p.pin(), Toast.LENGTH_LONG).show();
                    break;
                case 2:
                    String[] modes = {"Anyone with the PIN", "Anyone on my network", "Nobody (casting off)"};
                    new AlertDialog.Builder(this).setTitle("Who can cast to this TV?").setItems(modes, (d2, m) ->
                            { p.setAuth(m == 0 ? "pin" : m == 1 ? "open" : "off"); announce(); refresh(); }).show();
                    return;
                case 3: p.setBootStart(!p.bootStart()); break;
            }
            announce();
            refresh();
        }).show();
    }

    private void rename() {
        EditText in = new EditText(this);
        in.setText(App.prefs().name());
        in.setSelectAllOnFocus(true);
        new AlertDialog.Builder(this).setTitle("Name for this TV").setView(in)
                .setPositiveButton("Save", (d, x) -> {
                    String n = in.getText().toString().trim();
                    if (!n.isEmpty()) App.prefs().setName(n.length() > 60 ? n.substring(0, 60) : n);
                    announce();
                    refresh();
                })
                .setNegativeButton("Cancel", null).show();
    }

    private void announce() {
        ReceiverService s = ReceiverService.running();
        if (s != null) s.announce();
    }

    /** Android 10+ only lets a background app open the player if it may draw over other apps. */
    private void maybeAskOverlay() {
        if (Build.VERSION.SDK_INT < 29 || Settings.canDrawOverlays(this)) return;
        if (getSharedPreferences("wlctv", MODE_PRIVATE).getBoolean("asked_overlay", false)) return;
        getSharedPreferences("wlctv", MODE_PRIVATE).edit().putBoolean("asked_overlay", true).apply();
        new AlertDialog.Builder(this)
                .setTitle("Open the player automatically?")
                .setMessage("So a cast can take over the screen while you are in another app, allow WLC TV to "
                        + "\"display over other apps\". Without it, casts still play; open WLC TV to see them.")
                .setPositiveButton("Allow", (d, x) -> {
                    try {
                        startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                                Uri.parse("package:" + getPackageName())));
                    } catch (ActivityNotFoundException e) {
                        Toast.makeText(this, "This TV has no setting for that; keep WLC TV open to see casts.",
                                Toast.LENGTH_LONG).show();
                    }
                })
                .setNegativeButton("Not now", null)
                .show();
    }
}
