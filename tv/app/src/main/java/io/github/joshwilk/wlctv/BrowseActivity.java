/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.BaseAdapter;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import io.github.joshwilk.wlctv.core.Json;
import io.github.joshwilk.wlctv.core.WlcClient;

/**
 * Browse a WLC's Media Center from the couch. Picking something asks that WLC
 * to cast it here, so playback is ordinary WilkCast: the TV remote and the
 * Mac both control it.
 */
public class BrowseActivity extends Activity {
    private final Handler h = new Handler(Looper.getMainLooper());
    private final ExecutorService net = Executors.newSingleThreadExecutor();
    private WlcClient client;
    private String wlcId, wlcName;
    private LinearLayout page;
    private TextView heading, status;
    private ListView list;
    private List<WlcClient.Item> items = new ArrayList<>();
    private String root = "/", folder = "/";
    private final Rows rows = new Rows();

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        wlcName = getIntent().getStringExtra("name");
        wlcId = getIntent().getStringExtra("id");
        String host = getIntent().getStringExtra("host");
        int port = getIntent().getIntExtra("port", 48620);
        client = new WlcClient(host, port, wlcId == null || wlcId.isEmpty() ? null : App.prefs().cookieFor(wlcId));

        page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(Ui.BG);
        page.setPadding(Ui.dp(this, 56), Ui.dp(this, 32), Ui.dp(this, 56), Ui.dp(this, 24));
        heading = Ui.text(this, wlcName, 30, Ui.ORANGE2, true);
        page.addView(heading);
        status = Ui.text(this, "Connecting…", 18, Ui.MUTE, false);
        page.addView(status, Ui.fill(this, 6));
        setContentView(page);
        connect();
    }

    @Override
    protected void onDestroy() {
        net.shutdownNow();
        super.onDestroy();
    }

    private void connect() {
        net.execute(() -> {
            try {
                Map<String, Object> info = client.info();
                String id = Json.str(info, "id");
                if (id != null && (wlcId == null || wlcId.isEmpty())) {
                    wlcId = id;
                    String saved = App.prefs().cookieFor(id);
                    if (saved != null) client = new WlcClient(getIntent().getStringExtra("host"),
                            getIntent().getIntExtra("port", 48620), saved);
                }
                String n = Json.str(info, "name");
                if (n != null) h.post(() -> heading.setText(n));
                if (client.signedIn()) loadLibrary();
                else h.post(this::askPin);
            } catch (Exception e) {
                h.post(() -> status.setText("Could not reach " + wlcName + ". Is WLC running, with LAN access on? ("
                        + e.getMessage() + ")"));
            }
        });
    }

    private void askPin() {
        status.setText("Enter the PIN shown in WLC ▸ Control Center ▸ Network & Cast. You only do this once.");
        EditText in = new EditText(this);
        in.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_VARIATION_PASSWORD);
        in.setTextSize(28);
        in.setTextColor(Ui.TEXT);
        in.setHint("PIN");
        in.setHintTextColor(Ui.MUTE);
        in.setImeOptions(EditorInfo.IME_ACTION_DONE);
        in.setBackground(Ui.round(this, Ui.CARD, Ui.LINE, 10));
        in.setPadding(Ui.dp(this, 18), Ui.dp(this, 12), Ui.dp(this, 18), Ui.dp(this, 12));
        LinearLayout row = new LinearLayout(this);
        row.addView(in, new LinearLayout.LayoutParams(Ui.dp(this, 260), -2));
        TextView go = Ui.button(this, "Sign in", v -> signIn(in.getText().toString().trim(), row));
        row.addView(go, Ui.wrap(this, 0));
        ((LinearLayout.LayoutParams) go.getLayoutParams()).setMargins(Ui.dp(this, 16), 0, 0, 0);
        in.setOnEditorActionListener((v, act, ev) -> { signIn(in.getText().toString().trim(), row); return true; });
        page.addView(row, Ui.fill(this, 20));
        in.requestFocus();
    }

    private void signIn(String pin, View row) {
        if (pin.isEmpty()) return;
        status.setText("Signing in…");
        net.execute(() -> {
            try {
                client.login(pin);
                if (wlcId != null && !wlcId.isEmpty()) App.prefs().setCookie(wlcId, client.cookie());
                h.post(() -> page.removeView(row));
                loadLibrary();
            } catch (WlcClient.HttpError e) {
                h.post(() -> status.setText(e.code == 401
                        ? "That PIN didn't work (after 5 tries WLC waits a minute). Try again."
                        : "WLC said: " + e.getMessage()));
            } catch (Exception e) {
                h.post(() -> status.setText("Could not reach WLC: " + e.getMessage()));
            }
        });
    }

    private void loadLibrary() {
        h.post(() -> status.setText("Reading the library…"));
        try {
            List<WlcClient.Item> lib = client.library();
            h.post(() -> showLibrary(lib));
        } catch (WlcClient.HttpError e) {
            if (e.code == 401) {
                if (wlcId != null) App.prefs().setCookie(wlcId, null);
                h.post(this::askPin);
            } else h.post(() -> status.setText("WLC said: " + e.getMessage()));
        } catch (Exception e) {
            h.post(() -> status.setText("Could not read the library: " + e.getMessage()));
        }
    }

    private void showLibrary(List<WlcClient.Item> lib) {
        items = lib;
        if (lib.isEmpty()) {
            status.setText("Nothing to play yet: add folders in WLC ▸ Control Center ▸ Media Center.");
            return;
        }
        // start at the deepest folder that holds everything
        String common = lib.get(0).folder;
        for (WlcClient.Item i : lib) while (!(i.folder + "/").startsWith(common + "/")) {
            int s = common.lastIndexOf('/');
            common = s > 0 ? common.substring(0, s) : "/";
            if (common.equals("/")) break;
        }
        root = common;
        if (list == null) {
            list = new ListView(this);
            list.setAdapter(rows);
            list.setDivider(null);
            list.setItemsCanFocus(false);
            list.setSelector(Ui.round(this, 0xFF3A2414, Ui.ORANGE, 12));
            list.setDrawSelectorOnTop(false);
            list.setOnItemClickListener((p, v, pos, id) -> rows.click(pos));
            page.addView(list, new LinearLayout.LayoutParams(-1, 0, 1f));
        }
        open(root);
    }

    private void open(String f) {
        folder = f;
        rows.build();
        status.setText(folder.equals(root) ? items.size() + " videos and songs · OK to play, Back to go up"
                : folder.substring(root.length()).replaceFirst("^/", ""));
        list.setSelection(0);
        list.requestFocus();
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BACK && list != null && !folder.equals(root)) {
            int s = folder.lastIndexOf('/');
            open(s > root.length() ? folder.substring(0, s) : root);
            return true;
        }
        return super.onKeyDown(code, e);
    }

    private void cast(List<String> paths, String what) {
        ReceiverService s = ReceiverService.running();
        if (s == null || s.port() <= 0) {
            status.setText("The receiver is still starting; try again in a moment.");
            return;
        }
        status.setText("Asking " + heading.getText() + " to play " + what + "…");
        App.Prefs p = App.prefs();
        net.execute(() -> {
            try {
                client.castToMe(p.id(), s.port(), p.pin(), paths);
                h.post(() -> status.setText("Playing " + what));
            } catch (WlcClient.HttpError e) {
                h.post(() -> status.setText(e.code == 404
                        ? "WLC can't see this TV yet; check both are on the same network. (" + e.getMessage() + ")"
                        : "WLC said: " + e.getMessage()));
            } catch (Exception e) {
                h.post(() -> status.setText("Could not reach WLC: " + e.getMessage()));
            }
        });
    }

    /** Rows for the current folder: play-all, subfolders, then files. */
    private final class Rows extends BaseAdapter {
        final List<Object> data = new ArrayList<>();   // String = subfolder path, Item = file, Integer = play all
        final List<String> here = new ArrayList<>();

        void build() {
            data.clear();
            here.clear();
            TreeMap<String, Integer> sub = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
            List<WlcClient.Item> files = new ArrayList<>();
            String pre = folder.equals("/") ? "/" : folder + "/";
            for (WlcClient.Item i : items) {
                if (i.folder.equals(folder)) files.add(i);
                else if (i.path.startsWith(pre)) {
                    String rest = i.path.substring(pre.length());
                    String child = pre + rest.substring(0, rest.indexOf('/'));
                    Integer n = sub.get(child);
                    sub.put(child, n == null ? 1 : n + 1);
                }
            }
            for (WlcClient.Item i : files) here.add(i.path);
            if (files.size() > 1) data.add(files.size());
            for (Map.Entry<String, Integer> e : sub.entrySet()) data.add(new Object[]{e.getKey(), e.getValue()});
            Collections.sort(files, (a, b) -> a.name.compareToIgnoreCase(b.name));
            data.addAll(files);
            notifyDataSetChanged();
        }

        void click(int pos) {
            Object o = data.get(pos);
            if (o instanceof Integer) {
                String n = folder.substring(folder.lastIndexOf('/') + 1);
                cast(new ArrayList<>(here), "all of " + (n.isEmpty() ? "this folder" : n) + " (" + o + ")");
            } else if (o instanceof Object[]) {
                open((String) ((Object[]) o)[0]);
            } else {
                WlcClient.Item i = (WlcClient.Item) o;
                cast(Collections.singletonList(i.path), i.name);
            }
        }

        @Override public int getCount() { return data.size(); }
        @Override public Object getItem(int p) { return data.get(p); }
        @Override public long getItemId(int p) { return p; }

        @Override
        public View getView(int pos, View convert, ViewGroup parent) {
            TextView t = convert instanceof TextView ? (TextView) convert : Ui.text(BrowseActivity.this, "", 20, Ui.TEXT, false);
            t.setPadding(Ui.dp(BrowseActivity.this, 18), Ui.dp(BrowseActivity.this, 12),
                    Ui.dp(BrowseActivity.this, 18), Ui.dp(BrowseActivity.this, 12));
            t.setSingleLine(true);
            Object o = data.get(pos);
            if (o instanceof Integer) {
                t.setText("▶  Play all " + o + " in this folder");
                t.setTextColor(Ui.ORANGE2);
            } else if (o instanceof Object[]) {
                String p = (String) ((Object[]) o)[0];
                t.setText("📁  " + p.substring(p.lastIndexOf('/') + 1) + "   (" + ((Object[]) o)[1] + ")");
                t.setTextColor(Ui.TEXT);
            } else {
                WlcClient.Item i = (WlcClient.Item) o;
                t.setText((i.kind == 'v' ? "🎬  " : "🎵  ") + i.name + "   " + size(i.size));
                t.setTextColor(Ui.TEXT);
            }
            return t;
        }
    }

    private static String size(long b) {
        if (b >= 1L << 30) return String.format(Locale.ROOT, "%.1f GB", b / (double) (1L << 30));
        if (b >= 1L << 20) return String.format(Locale.ROOT, "%.0f MB", b / (double) (1L << 20));
        return String.format(Locale.ROOT, "%d KB", Math.max(1, b >> 10));
    }
}
