/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv.core;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.MulticastSocket;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * WilkCast discovery (doc/wilkcast.md section 1): announces this TV every few
 * seconds on 239.87.76.67:48621 (and subnet broadcast), answers "who is here?",
 * and keeps the list of other devices it hears, so the TV can browse WLCs.
 */
public final class Discovery {
    public static final String GROUP = "239.87.76.67";
    public static final int PORT = 48621;
    private static final String MAGIC = "WILKCAST/1 ";
    private static final long GAP_MS = 4000, TTL_MS = 15000;

    public static final class Device {
        public String id, name, ip, auth, base, kind;
        public int port;
        public long seen;
        @Override public String toString() { return name + " (" + kind + " " + ip + ":" + port + ")"; }
    }

    private final CastServer.Config config;
    private final int httpPort;
    private final Map<String, Device> devices = new LinkedHashMap<>();
    private MulticastSocket sock;
    private volatile boolean running;
    private volatile long nextBeacon;
    private Thread thread;

    public Discovery(CastServer.Config config, int httpPort) {
        this.config = config;
        this.httpPort = httpPort;
    }

    public void start() throws IOException {
        sock = new MulticastSocket(null);
        sock.setReuseAddress(true);
        sock.bind(new InetSocketAddress(PORT));
        sock.setTimeToLive(1);
        sock.setBroadcast(true);
        sock.setSoTimeout(500);
        InetAddress group = InetAddress.getByName(GROUP);
        for (NetworkInterface ni : interfaces()) {
            try { sock.joinGroup(new InetSocketAddress(group, PORT), ni); } catch (IOException ignored) { }
        }
        running = true;
        thread = new Thread(this::loop, "wilkcast-discovery");
        thread.setDaemon(true);
        thread.start();
        send(MAGIC + "?");
    }

    public void stop() {
        running = false;
        if (thread != null) {
            try { thread.join(2000); } catch (InterruptedException ignored) { }
        }
        if (sock != null) {
            announce(true);
            sock.close();
        }
    }

    /** Ask everyone to announce now (e.g. when the browse screen opens). */
    public void probe() {
        send(MAGIC + "?");
    }

    /** Announce now (e.g. after a rename). */
    public void announceSoon() { nextBeacon = 0; }

    /** WLCs (kind "wlc") heard recently, most recent first. */
    public synchronized List<Device> wlcs() {
        List<Device> out = new ArrayList<>();
        long now = System.currentTimeMillis();
        for (Device d : devices.values())
            if ("wlc".equals(d.kind) && now - d.seen < TTL_MS) out.add(d);
        return out;
    }

    public synchronized List<Device> all() {
        return new ArrayList<>(devices.values());
    }

    private void loop() {
        byte[] buf = new byte[1024];
        while (running) {
            long now = System.currentTimeMillis();
            if (now >= nextBeacon) {
                announce(false);
                nextBeacon = now + GAP_MS;
                synchronized (this) {
                    for (java.util.Iterator<Device> it = devices.values().iterator(); it.hasNext(); )
                        if (now - it.next().seen > TTL_MS) it.remove();
                }
            }
            DatagramPacket p = new DatagramPacket(buf, buf.length);
            try {
                sock.receive(p);
            } catch (SocketTimeoutException e) {
                continue;
            } catch (IOException e) {
                if (!running) return;
                continue;
            }
            String text = new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8);
            if (!text.startsWith(MAGIC)) continue;
            String payload = text.substring(MAGIC.length());
            if (payload.startsWith("?")) { nextBeacon = 0; continue; }
            Map<String, Object> j = Json.obj(payload);
            String id = Json.str(j, "id");
            int port = (int) Json.num(j, "port", 0);
            if (id == null || id.length() < 8 || id.length() >= 40 || port <= 0 || port > 65535) continue;
            if (id.equals(config.id())) continue;
            String ip = p.getAddress().getHostAddress();
            synchronized (this) {
                if (Json.bool(j, "bye", false)) { devices.remove(id); continue; }
                Device d = devices.get(id);
                if (d == null) { d = new Device(); devices.put(id, d); }
                // a device on this same machine is heard on loopback too: keep the LAN address
                if (!(ip.startsWith("127.") && d.ip != null && !d.ip.startsWith("127."))) d.ip = ip;
                d.id = id;
                String name = Json.str(j, "name");
                d.name = name == null || name.isEmpty() ? "WLC" : name;
                d.port = port;
                String auth = Json.str(j, "auth");
                d.auth = auth == null ? "pin" : auth;
                String base = Json.str(j, "base");
                d.base = base != null && base.startsWith("/cast/v1/") && base.endsWith("/") && !base.contains("..")
                        ? base : "/cast/v1/";
                String kind = Json.str(j, "kind");
                d.kind = kind == null || kind.isEmpty() ? "wlc" : kind;
                d.seen = System.currentTimeMillis();
            }
        }
    }

    private void announce(boolean bye) {
        Map<String, Object> m = Json.map("id", config.id(), "name", config.name(), "port", httpPort,
                "auth", config.auth(), "kind", "tv");
        if (bye) m.put("bye", true);
        send(MAGIC + Json.write(m));
    }

    private void send(String text) {
        if (sock == null || sock.isClosed()) return;
        byte[] data = text.getBytes(StandardCharsets.UTF_8);
        try {
            InetAddress group = InetAddress.getByName(GROUP);
            for (NetworkInterface ni : interfaces()) {
                try {
                    sock.setNetworkInterface(ni);
                    sock.send(new DatagramPacket(data, data.length, group, PORT));
                } catch (IOException ignored) { }
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress bc = ia.getBroadcast();
                    if (bc == null) continue;
                    try { sock.send(new DatagramPacket(data, data.length, bc, PORT)); } catch (IOException ignored) { }
                }
            }
        } catch (IOException ignored) {
        }
    }

    /** Up, multicast-capable interfaces that have an IPv4 address (loopback included). */
    static List<NetworkInterface> interfaces() {
        List<NetworkInterface> out = new ArrayList<>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp()) continue;
                boolean v4 = false;
                for (InterfaceAddress ia : ni.getInterfaceAddresses())
                    if (ia.getAddress() instanceof Inet4Address) v4 = true;
                if (v4 && (ni.supportsMulticast() || ni.isLoopback())) out.add(ni);
            }
        } catch (IOException ignored) {
        }
        return out;
    }

    /** This device's LAN IPv4 addresses, for showing on screen. */
    public static List<String> lanAddresses() {
        List<String> out = new ArrayList<>();
        for (NetworkInterface ni : interfaces()) {
            try { if (ni.isLoopback()) continue; } catch (IOException e) { continue; }
            for (InterfaceAddress ia : ni.getInterfaceAddresses())
                if (ia.getAddress() instanceof Inet4Address) out.add(ia.getAddress().getHostAddress());
        }
        return out;
    }
}
