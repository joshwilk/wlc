/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv.core;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * Talks to a WLC on the network the way its LAN page does: sign in with the
 * PIN (a session cookie), read the Media Center library, and ask WLC to cast a
 * file or a folder to this TV, so playback is ordinary WilkCast and can be
 * controlled from both ends.
 */
public final class WlcClient {
    public static final class Item {
        public final String path, name, folder;
        public final char kind;      // 'v' video, 'a' audio, 'i' image
        public final long size;
        Item(String path, char kind, long size) {
            this.path = path;
            this.kind = kind;
            this.size = size;
            int slash = path.lastIndexOf('/');
            this.name = slash >= 0 ? path.substring(slash + 1) : path;
            this.folder = slash > 0 ? path.substring(0, slash) : "/";
        }
    }

    public static final class HttpError extends IOException {
        public final int code;
        HttpError(int code, String msg) { super(msg); this.code = code; }
    }

    private final String host;
    private final int port;
    private volatile String cookie;

    public WlcClient(String host, int port, String cookie) {
        this.host = host;
        this.port = port;
        this.cookie = cookie;
    }

    public String cookie() { return cookie; }

    /** {app, name, id, authed, ...} */
    public Map<String, Object> info() throws IOException {
        return call("GET", "/api/info", null);
    }

    public boolean signedIn() throws IOException {
        return Json.bool(info(), "authed", false);
    }

    /** Signs in with WLC's PIN; throws HttpError 401 if it is wrong. */
    public void login(String pin) throws IOException {
        call("POST", "/api/login", Json.write(Json.map("pin", pin)));
        if (cookie == null) throw new HttpError(401, "WLC did not accept the PIN");
    }

    /** Video and audio files in WLC's Media Center, sorted by path. */
    public List<Item> library() throws IOException {
        Map<String, Object> r = call("GET", "/api/library", null);
        List<Item> out = new ArrayList<>();
        Object items = r.get("items");
        if (items instanceof List) {
            for (Object o : (List<?>) items) {
                if (!(o instanceof Map)) continue;
                @SuppressWarnings("unchecked") Map<String, Object> m = (Map<String, Object>) o;
                String p = Json.str(m, "p"), k = Json.str(m, "k");
                if (p == null || k == null || k.isEmpty()) continue;
                char kind = k.charAt(0);
                if (kind == 'v' || kind == 'a') out.add(new Item(p, kind, (long) Json.num(m, "s", 0)));
            }
        }
        Collections.sort(out, (a, b) -> a.path.compareToIgnoreCase(b.path));
        return out;
    }

    /**
     * Has WLC cast these files to the receiver `myId` (this TV), which answers
     * WLC at myPort with myPin. Makes sure WLC knows the TV first.
     */
    public void castToMe(String myId, int myPort, String myPin, List<String> paths) throws IOException {
        try {
            call("POST", "/api/peers/add", Json.write(Json.map("host", myAddressTowardsWlc(), "port", myPort)));
        } catch (HttpError e) {
            // WLC may already know us from discovery; the cast below says if not
        }
        Map<String, Object> body = Json.map("peer", myId, "pin", myPin);
        if (paths.size() == 1) body.put("path", paths.get(0));
        else body.put("paths", new ArrayList<Object>(paths));
        call("POST", "/api/cast/send", Json.write(body));
    }

    /** The address of this device on the network WLC is on. */
    public String myAddressTowardsWlc() throws IOException {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress(host, port), 4000);
            return s.getLocalAddress().getHostAddress();
        }
    }

    /* ------------------------------------------------------------------ HTTP */

    private Map<String, Object> call(String method, String path, String json) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL("http://" + host + ":" + port + path).openConnection();
        c.setConnectTimeout(5000);
        c.setReadTimeout(30000);
        c.setRequestMethod(method);
        c.setInstanceFollowRedirects(false);
        if (cookie != null) c.setRequestProperty("Cookie", "wlc_s=" + cookie);
        if (json != null) {
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            // WLC checks Origin on state-changing requests; an app sends none, like curl
            byte[] b = json.getBytes(StandardCharsets.UTF_8);
            c.setFixedLengthStreamingMode(b.length);
            try (OutputStream o = c.getOutputStream()) { o.write(b); }
        }
        int code = c.getResponseCode();
        List<String> sets = c.getHeaderFields().get("Set-Cookie");
        if (sets != null) {
            for (String sc : sets) {
                if (sc != null && sc.startsWith("wlc_s=")) {
                    int end = sc.indexOf(';');
                    cookie = sc.substring(6, end < 0 ? sc.length() : end);
                }
            }
        }
        InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
        String text = "";
        if (in != null) {
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            in.close();
            text = bo.toString("UTF-8");
        }
        c.disconnect();
        Map<String, Object> m = Json.obj(text);
        if (code != 200) {
            String msg = Json.str(m, "error");
            throw new HttpError(code, msg != null ? msg : "WLC answered " + code);
        }
        return m != null ? m : Json.map();
    }
}
