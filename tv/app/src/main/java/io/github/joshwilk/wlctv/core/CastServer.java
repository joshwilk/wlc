/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv.core;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * The WilkCast 1.1 receiver HTTP API (doc/wilkcast.md section 2):
 * GET /cast/v1/info, GET /cast/v1/status, POST /cast/v1/play, POST /cast/v1/control.
 */
public final class CastServer {
    public interface Config {
        String id();
        String name();
        String pin();
        /** "pin", "open" or "off" */
        String auth();
    }

    public interface Listener {
        /** A cast arrived (after it was handed to the player). */
        void onCast(String title, String from, String remoteIp);
    }

    public static final int DEFAULT_PORT = 48620;
    public static final String VERSION = "0.1.0";
    private static final Set<String> CMDS = new HashSet<>(Arrays.asList(
            "play", "resume", "pause", "toggle", "stop", "seek", "seekrel", "volume", "next", "prev"));
    private static final int MAX_QUEUE = 500;

    private final Config config;
    private final Player player;
    private final Listener listener;
    private ServerSocket server;
    private ExecutorService pool;
    private volatile boolean running;
    private int fails;
    private long lockedUntil;

    public CastServer(Config config, Player player, Listener listener) {
        this.config = config;
        this.player = player;
        this.listener = listener;
    }

    /** Listens on the first free port from `port` to port+9; returns the port. */
    public synchronized int start(int port) throws IOException {
        IOException last = null;
        for (int p = port; p < port + 10; p++) {
            try {
                ServerSocket s = new ServerSocket();
                s.setReuseAddress(true);
                s.bind(new InetSocketAddress((InetAddress) null, p));
                server = s;
                break;
            } catch (IOException e) {
                last = e;
            }
        }
        if (server == null) throw last;
        running = true;
        pool = Executors.newFixedThreadPool(6);
        Thread t = new Thread(this::acceptLoop, "wilkcast-http");
        t.setDaemon(true);
        t.start();
        return server.getLocalPort();
    }

    public int port() { return server == null ? -1 : server.getLocalPort(); }

    public synchronized void stop() {
        running = false;
        try { if (server != null) server.close(); } catch (IOException ignored) { }
        if (pool != null) pool.shutdownNow();
    }

    private void acceptLoop() {
        while (running) {
            try {
                final Socket c = server.accept();
                pool.execute(() -> handle(c));
            } catch (IOException e) {
                if (!running) return;
            } catch (RuntimeException e) {
                // pool rejected (shutting down)
            }
        }
    }

    /* ------------------------------------------------------------------ HTTP */

    private void handle(Socket c) {
        try (Socket sock = c) {
            sock.setSoTimeout(15000);
            InputStream in = sock.getInputStream();
            ByteArrayOutputStream head = new ByteArrayOutputStream();
            int b, n = 0;
            // read until \r\n\r\n (headers are small)
            while ((b = in.read()) != -1) {
                head.write(b);
                n = (b == '\r' || b == '\n') ? n + 1 : 0;
                if (n == 4 || head.size() > 16384) break;
            }
            String[] lines = head.toString("ISO-8859-1").split("\r\n");
            String[] rl = lines.length > 0 ? lines[0].split(" ") : new String[0];
            if (rl.length < 2) return;
            String method = rl[0], path = rl[1];
            int q = path.indexOf('?');
            if (q >= 0) path = path.substring(0, q);
            String pin = "";
            int len = 0;
            for (int k = 1; k < lines.length; k++) {
                int colon = lines[k].indexOf(':');
                if (colon < 0) continue;
                String h = lines[k].substring(0, colon).trim().toLowerCase(Locale.ROOT);
                String v = lines[k].substring(colon + 1).trim();
                if (h.equals("x-wilkcast-pin")) pin = v;
                else if (h.equals("content-length")) {
                    try { len = Integer.parseInt(v); } catch (NumberFormatException ignored) { }
                }
            }
            if (len < 0 || len > (1 << 20)) {
                reply(sock, 400, err("request too large"));
                return;
            }
            byte[] body = new byte[len];
            int got = 0;
            while (got < len) {
                int r = in.read(body, got, len - got);
                if (r < 0) break;
                got += r;
            }
            String ip = sock.getInetAddress().getHostAddress();
            reply(sock, route(method, path, pin, new String(body, 0, got, StandardCharsets.UTF_8), ip));
        } catch (IOException ignored) {
        }
    }

    private static final class Resp {
        final int code; final Object body;
        Resp(int code, Object body) { this.code = code; this.body = body; }
    }

    private static Map<String, Object> err(String msg) { return Json.map("error", msg); }

    private void reply(Socket s, int code, Object body) throws IOException { reply(s, new Resp(code, body)); }

    private void reply(Socket s, Resp r) throws IOException {
        byte[] b = Json.write(r.body).getBytes(StandardCharsets.UTF_8);
        String status = r.code == 200 ? "OK" : r.code == 401 ? "Unauthorized" : r.code == 403 ? "Forbidden"
                : r.code == 404 ? "Not Found" : r.code == 405 ? "Method Not Allowed" : "Bad Request";
        String head = "HTTP/1.1 " + r.code + " " + status + "\r\n"
                + "Content-Type: application/json\r\nContent-Length: " + b.length
                + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        OutputStream out = s.getOutputStream();
        out.write(head.getBytes(StandardCharsets.ISO_8859_1));
        out.write(b);
        out.flush();
    }

    /* ------------------------------------------------------------------ routes */

    Resp route(String method, String path, String pin, String body, String ip) {
        if (!path.startsWith("/cast/v1/")) return new Resp(404, err("no such cast endpoint"));
        String op = path.substring(9);
        if (op.equals("info")) {
            return new Resp(200, Json.map("wilkcast", 1, "app", "WLC TV", "version", VERSION,
                    "kind", "tv", "id", config.id(), "name", config.name(), "auth", config.auth()));
        }
        if (!op.equals("status") && !op.equals("play") && !op.equals("control"))
            return new Resp(404, err("no such cast endpoint"));
        if (!op.equals("status") && !method.equals("POST")) return new Resp(405, err("use POST"));
        String auth = config.auth();
        if (auth.equals("off")) return new Resp(403, err("this TV is not accepting casts"));
        if (auth.equals("pin") && !pinOk(pin)) return new Resp(401, err("PIN required"));

        if (op.equals("status")) return new Resp(200, status());
        Map<String, Object> j = Json.obj(body);
        if (j == null) return new Resp(400, err("expected a JSON object"));

        if (op.equals("play")) {
            String url = Json.str(j, "url");
            if (!httpUrl(url)) return new Resp(400, err("url must be http or https"));
            List<Map<String, Object>> queue = new ArrayList<>();
            Object qv = j.get("queue");
            if (qv instanceof List) {
                for (Object o : (List<?>) qv) {
                    if (queue.size() >= MAX_QUEUE) break;
                    if (o instanceof Map) {
                        @SuppressWarnings("unchecked") Map<String, Object> m = (Map<String, Object>) o;
                        if (httpUrl(Json.str(m, "url"))) queue.add(m);
                    }
                }
            }
            String title = Json.str(j, "title");
            String from = Json.str(j, "from");
            player.load(url, title == null ? "" : title, Math.max(0, Json.num(j, "position", 0)), queue, from);
            if (listener != null) listener.onCast(title, from, ip);
            return new Resp(200, Json.map("ok", true));
        }
        // control
        String cmd = Json.str(j, "cmd");
        if (cmd == null || !CMDS.contains(cmd)) return new Resp(400, err("command not allowed for casting"));
        player.control(cmd, Json.num(j, "value", 0));
        return new Resp(200, status());
    }

    private Map<String, Object> status() {
        Map<String, Object> st = player.status();
        st.put("name", config.name());
        st.put("kind", "tv");
        return st;
    }

    private static boolean httpUrl(String u) {
        if (u == null || u.length() > 4096) return false;
        String l = u.toLowerCase(Locale.ROOT);
        return l.startsWith("http://") || l.startsWith("https://");
    }

    /** Constant-time compare, 5 wrong PINs lock it for a minute. */
    private synchronized boolean pinOk(String given) {
        long now = System.currentTimeMillis();
        if (now < lockedUntil) return false;
        boolean ok = MessageDigest.isEqual(given.getBytes(StandardCharsets.UTF_8),
                config.pin().getBytes(StandardCharsets.UTF_8));
        if (ok) { fails = 0; return true; }
        if (++fails >= 5) { fails = 0; lockedUntil = now + 60_000; }
        return false;
    }
}
