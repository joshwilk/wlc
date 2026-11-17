/*
 * Tests the WLC TV core (receiver, discovery, WLC client) against a real WLC.
 *   tv/test/run_core_test.sh /path/to/vlc
 * which starts a WLC, then runs:
 *   java CoreTest <wlc-port> <wlc-pin> <media-dir> <lan-ip> <wilkcast.py>
 */
import io.github.joshwilk.wlctv.core.*;

import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class CoreTest {
    static int pass, fail;

    static void check(String name, boolean ok, Object detail) {
        if (ok) pass++; else fail++;
        System.out.println((ok ? "  ok   " : "  FAIL ") + name + (ok || detail == null ? "" : "  -> " + detail));
    }

    /** Stand-in for libVLC: remembers what it was told, plays by the clock. */
    static class FakePlayer implements Player {
        String url, title, from, state = "stopped";
        List<Map<String, Object>> queue = new ArrayList<>();
        double offset, volume = 1;
        long started;
        int loads;

        public synchronized void load(String url, String title, double pos, List<Map<String, Object>> q, String from) {
            this.url = url; this.title = title; this.from = from; this.queue = q;
            offset = pos; started = System.currentTimeMillis(); state = "playing"; loads++;
        }
        double time() {
            return state.equals("playing") ? offset + (System.currentTimeMillis() - started) / 1000.0 : offset;
        }
        public synchronized void control(String cmd, double v) {
            switch (cmd) {
                case "pause": if (state.equals("playing")) { offset = time(); state = "paused"; } break;
                case "play": case "resume": if (state.equals("paused")) { started = System.currentTimeMillis(); state = "playing"; } break;
                case "toggle": control(state.equals("playing") ? "pause" : "resume", 0); break;
                case "stop": state = "stopped"; url = null; break;
                case "seek": offset = v; started = System.currentTimeMillis(); break;
                case "seekrel": offset = time() + v; started = System.currentTimeMillis(); break;
                case "volume": volume = v; break;
                case "next": if (!queue.isEmpty()) { Map<String, Object> m = queue.remove(0); url = Json.str(m, "url"); title = Json.str(m, "title"); offset = 0; started = System.currentTimeMillis(); } break;
            }
        }
        public synchronized Map<String, Object> status() {
            return Json.map("state", state, "time", time(), "length", 30.0, "volume", volume, "title", title == null ? "" : title);
        }
    }

    static class Cfg implements CastServer.Config {
        String id = "c0ffee00c0ffee00c0ffee00c0ffee00", name = "Test TV", pin = "4821", auth = "pin";
        public String id() { return id; }
        public String name() { return name; }
        public String pin() { return pin; }
        public String auth() { return auth; }
    }

    static int lastCode;

    static Map<String, Object> http(String method, String url, String body, String pin) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setRequestMethod(method);
        c.setConnectTimeout(5000);
        c.setReadTimeout(20000);
        if (pin != null) c.setRequestProperty("X-WilkCast-Pin", pin);
        if (body != null) {
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            try (OutputStream o = c.getOutputStream()) { o.write(body.getBytes(StandardCharsets.UTF_8)); }
        }
        lastCode = c.getResponseCode();
        InputStream in = lastCode >= 400 ? c.getErrorStream() : c.getInputStream();
        String t = in == null ? "" : new String(in.readAllBytes(), StandardCharsets.UTF_8);
        Map<String, Object> m = Json.obj(t);
        return m == null ? new HashMap<>() : m;
    }

    interface Cond { boolean ok() throws Exception; }

    static boolean waitFor(Cond c, double secs) throws Exception {
        long end = System.currentTimeMillis() + (long) (secs * 1000);
        while (System.currentTimeMillis() < end) {
            if (c.ok()) return true;
            Thread.sleep(250);
        }
        return c.ok();
    }

    @SuppressWarnings("unchecked")
    static List<Map<String, Object>> peers(String wlc) throws IOException {
        Object p = http("GET", wlc + "/api/peers", null, null).get("peers");
        return p instanceof List ? (List<Map<String, Object>>) p : new ArrayList<>();
    }

    public static void main(String[] a) throws Exception {
        int wlcPort = Integer.parseInt(a[0]);
        String wlcPin = a[1], media = a[2], ip = a[3], wilkcast = a[4];
        String wlc = "http://127.0.0.1:" + wlcPort;
        Cfg cfg = new Cfg();
        FakePlayer fp = new FakePlayer();
        List<String> casts = new ArrayList<>();
        CastServer server = new CastServer(cfg, fp, (t, from, rip) -> casts.add(t + " from " + from + " @" + rip));
        int port = server.start(48650);
        Discovery disc = new Discovery(cfg, port);
        disc.start();
        String tv = "http://127.0.0.1:" + port + "/cast/v1/";

        System.out.println("Receiver");
        Map<String, Object> info = http("GET", tv + "info", null, null);
        check("info is public and says WLC TV / kind tv", lastCode == 200 && "tv".equals(info.get("kind"))
                && "Test TV".equals(info.get("name")) && Json.num(info, "wilkcast", 0) == 1, info);
        http("GET", tv + "status", null, null);
        check("status needs the PIN", lastCode == 401, lastCode);
        http("POST", tv + "play", "{\"url\":\"file:///etc/passwd\"}", "4821");
        check("non-http casts are refused", lastCode == 400, lastCode);
        http("GET", tv + "play", null, "4821");
        check("play must be POST", lastCode == 405, lastCode);
        http("POST", tv + "control", "{\"cmd\":\"clear\"}", "4821");
        check("commands outside the cast set are refused", lastCode == 400, lastCode);

        System.out.println("Discovery");
        boolean seen = waitFor(() -> peers(wlc).stream().anyMatch(p -> "Test TV".equals(p.get("name")) && "tv".equals(p.get("kind"))), 15);
        check("WLC discovers the TV over UDP (kind tv)", seen, peers(wlc));
        Process pr = new ProcessBuilder("python3", wilkcast, "discover", "-t", "6", "--json").redirectErrorStream(true).start();
        String out = new String(pr.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        pr.waitFor();
        check("wilkcast.py discovers the TV", out.contains("\"Test TV\"") && out.contains("\"tv\""), out);
        boolean heard = waitFor(() -> !disc.wlcs().isEmpty(), 10);
        check("the TV hears the WLC (for browsing)", heard && disc.wlcs().get(0).port == wlcPort, disc.all());

        System.out.println("WLC casts to the TV");
        String clip = media + "/Video/clip.mp4";
        http("POST", wlc + "/api/cast/send", Json.write(Json.map("peer", cfg.id, "pin", "0000", "path", clip)), null);
        check("wrong TV PIN from WLC is refused", lastCode == 401, lastCode);
        Map<String, Object> r = http("POST", wlc + "/api/cast/send", Json.write(Json.map("peer", cfg.id, "pin", "4821", "path", clip)), null);
        check("WLC casts a file to the TV", lastCode == 200 && fp.url != null && fp.url.contains("/media/") && fp.url.endsWith("clip.mp4"), r + " " + fp.url);
        check("the cast says who sent it", fp.from != null && !fp.from.isEmpty() && casts.size() == 1, casts);
        if (fp.url != null) {
            HttpURLConnection c = (HttpURLConnection) new URL(fp.url).openConnection();
            c.setRequestProperty("Range", "bytes=0-99");
            int code = c.getResponseCode();
            int n = c.getInputStream().readAllBytes().length;
            check("the TV can stream the file from WLC, with byte ranges", code == 206 && n == 100
                    && new URL(fp.url).getHost().equals(ip), code + " " + n + " " + fp.url);
        }
        Map<String, Object> st = http("GET", wlc + "/api/cast/status?peer=" + cfg.id + "&pin=4821", null, null);
        check("WLC sees the TV playing", "playing".equals(st.get("state")) && "tv".equals(st.get("kind")), st);
        http("POST", wlc + "/api/cast/control", Json.write(Json.map("peer", cfg.id, "pin", "4821", "cmd", "pause")), null);
        check("pause from WLC reaches the TV", fp.state.equals("paused"), fp.state);
        http("POST", wlc + "/api/cast/control", Json.write(Json.map("peer", cfg.id, "pin", "4821", "cmd", "seek", "value", 20)), null);
        check("seek from WLC reaches the TV", Math.abs(fp.time() - 20) < 0.5, fp.time());
        fp.control("resume", 0);   // someone pressed play on the TV remote
        st = http("GET", wlc + "/api/cast/status?peer=" + cfg.id + "&pin=4821", null, null);
        check("pressing play on the TV shows up on WLC (both sides in sync)", "playing".equals(st.get("state"))
                && Json.num(st, "time", 0) >= 19.5, st);
        File[] tracks = new File(media + "/Music/Album").listFiles();
        Arrays.sort(tracks);
        List<Object> paths = new ArrayList<>();
        for (File f : tracks) paths.add(f.getPath());
        http("POST", wlc + "/api/cast/send", Json.write(Json.map("peer", cfg.id, "pin", "4821", "paths", paths)), null);
        check("WLC casts a folder as a queue", lastCode == 200 && fp.queue.size() == tracks.length - 1, fp.queue.size());
        http("POST", wlc + "/api/cast/control", Json.write(Json.map("peer", cfg.id, "pin", "4821", "cmd", "next")), null);
        check("next from WLC moves through the TV's queue", fp.title != null && fp.title.equals(tracks[1].getName()), fp.title);

        System.out.println("The TV browses WLC");
        WlcClient cl = new WlcClient(ip, wlcPort, null);
        check("not signed in to begin with", !cl.signedIn(), null);
        int code = 0;
        try { cl.login("000000"); } catch (WlcClient.HttpError e) { code = e.code; }
        check("wrong WLC PIN is refused", code == 401, code);
        cl.login(wlcPin);
        check("right WLC PIN signs the TV in", cl.signedIn() && cl.cookie() != null, cl.cookie());
        WlcClient again = new WlcClient(ip, wlcPort, cl.cookie());
        check("the sign-in is kept (cookie reused)", again.signedIn(), null);
        List<WlcClient.Item> lib = cl.library();
        boolean onlyAv = lib.stream().allMatch(i -> i.kind == 'v' || i.kind == 'a');
        check("library lists the videos and songs, no pictures", lib.size() >= 4 && onlyAv
                && lib.stream().anyMatch(i -> i.name.equals("clip.mp4")), lib.size());
        int loads = fp.loads;
        cl.castToMe(cfg.id, port, cfg.pin, Collections.singletonList(clip));
        check("picking a video on the TV makes WLC cast it to the TV", fp.loads == loads + 1 && fp.url.endsWith("clip.mp4"), fp.url);
        List<String> folder = new ArrayList<>();
        for (WlcClient.Item i : lib) if (i.folder.endsWith("/Music/Album")) folder.add(i.path);
        cl.castToMe(cfg.id, port, cfg.pin, folder);
        check("\"play folder\" on the TV queues the whole folder", fp.queue.size() == folder.size() - 1, fp.queue.size() + " of " + folder.size());
        String addr = cl.myAddressTowardsWlc();
        check("the TV knows its own address on WLC's network", addr.equals(ip), addr);

        System.out.println("Settings");
        cfg.auth = "off";
        http("POST", tv + "play", "{\"url\":\"http://x/a.mp4\"}", "4821");
        check("\"off\" refuses casts", lastCode == 403, lastCode);
        cfg.auth = "open";
        http("GET", tv + "status", null, null);
        check("\"open\" needs no PIN", lastCode == 200, lastCode);
        cfg.auth = "pin";
        for (int k = 0; k < 5; k++) http("GET", tv + "status", null, "1111");
        http("GET", tv + "status", null, "4821");
        check("5 wrong PINs lock the TV for a minute", lastCode == 401, lastCode);

        disc.stop();
        server.stop();
        Thread.sleep(500);
        boolean gone = waitFor(() -> peers(wlc).stream().noneMatch(p -> "Test TV".equals(p.get("name"))), 6);
        check("when the TV app stops, it leaves WLC's list", gone, peers(wlc));

        System.out.println("\n" + pass + " passed, " + fail + " failed");
        System.exit(fail == 0 ? 0 : 1);
    }
}
