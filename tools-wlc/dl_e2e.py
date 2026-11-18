#!/usr/bin/env python3
"""End-to-end tests for WLC's Downloads tool (yt-dlp), offline.

    python3 tools-wlc/dl_e2e.py path/to/vlc       (not as root: VLC refuses)

Serves test media from a local web server (direct files, a DASH stream with
separate video and audio, an RSS playlist, a slow file) and drives WLC's
/api/dl/* endpoints. Needs ffmpeg/ffprobe and yt-dlp (Python module) here.
"""
import hashlib, http.server, json, os, shutil, socket, socketserver, stat, subprocess, sys, tempfile
import threading, time, urllib.error, urllib.request

VLC = sys.argv[1]
WORK = tempfile.mkdtemp(prefix='wlc-dl-e2e-')
PASS = FAIL = 0
procs = []


def check(name, ok, detail=''):
    global PASS, FAIL
    PASS, FAIL = PASS + bool(ok), FAIL + (not ok)
    print(('  ok   ' if ok else '  FAIL ') + name + ('' if ok or detail == '' else '  -> ' + str(detail)[:400]), flush=True)


def ffmpeg(*a):
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y'] + list(a), check=True)


def probe(path):
    r = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', path],
                       capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except ValueError:
        return {}


def codecs(path):
    return sorted(s.get('codec_type') + ':' + s.get('codec_name') for s in probe(path).get('streams', []))


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(('192.0.2.1', 9))
    return s.getsockname()[0]


# ------------------------------------------------------------------ test media + web server
site = os.path.join(WORK, 'site')
os.makedirs(site + '/dash')
os.makedirs(site + '/rel-good')
os.makedirs(site + '/rel-bad')
ffmpeg('-f', 'lavfi', '-i', 'testsrc=duration=8:size=320x240:rate=25', '-f', 'lavfi', '-i',
       'sine=frequency=440:duration=8', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-shortest',
       '-movflags', '+faststart', site + '/clip.mp4')
ffmpeg('-i', site + '/clip.mp4', '-map', '0:v', '-map', '0:a', '-c', 'copy', '-f', 'dash',
       '-adaptation_sets', 'id=0,streams=v id=1,streams=a', '-seg_duration', '2', site + '/dash/show.mpd')
for i in (1, 2, 3):
    ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=%d:duration=4' % (300 * i), '-c:a', 'libmp3lame',
           '-metadata', 'title=Episode %d' % i, site + '/ep%d.mp3' % i)
ffmpeg('-f', 'lavfi', '-i', 'testsrc=duration=60:size=640x360:rate=25', '-c:v', 'libx264', '-b:v', '2M',
       '-pix_fmt', 'yuv420p', site + '/big.mp4')

# a fake "official release" of yt-dlp: a wrapper around the yt-dlp Python module
wrapper = '#!/bin/sh\nexec %s "$@"\n' % shutil.which('yt-dlp')
asset = ('yt-dlp_macos' if sys.platform == 'darwin' else
         'yt-dlp_linux_aarch64' if os.uname().machine in ('aarch64', 'arm64') else 'yt-dlp_linux')
for rel, good in (('rel-good', True), ('rel-bad', False)):
    open('%s/%s/%s' % (site, rel, asset), 'w').write(wrapper)
    digest = hashlib.sha256(wrapper.encode()).hexdigest() if good else '0' * 64
    open('%s/%s/SHA2-256SUMS' % (site, rel), 'w').write('%s  %s\n%s  yt-dlp.exe\n' % (digest, asset, 'f' * 64))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=site, **k)

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith('/feed.xml'):
            host = self.headers.get('Host')
            items = ''.join('<item><title>Episode %d</title><guid>ep%d</guid><enclosure url="http://%s/ep%d.mp3" '
                            'type="audio/mpeg" length="1"/></item>' % (i, i, host, i) for i in (1, 2, 3))
            body = ('<?xml version="1.0"?><rss version="2.0"><channel><title>Test Show</title>'
                    '<link>http://%s/</link>%s</channel></rss>' % (host, items)).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/rss+xml')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith('/slow/'):
            # 40 KB/s, so a download can be watched and cancelled
            path = site + '/' + self.path[6:].split('?')[0]
            data = open(path, 'rb').read()
            self.send_response(200)
            self.send_header('Content-Type', 'video/mp4')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            try:
                for i in range(0, len(data), 4096):
                    self.wfile.write(data[i:i + 4096])
                    time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        return super().do_GET()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


srv = Server(('127.0.0.1', 0), Handler)
threading.Thread(target=srv.serve_forever, daemon=True).start()
WEB = 'http://127.0.0.1:%d' % srv.server_address[1]


# ------------------------------------------------------------------ WLC
def start_wlc(name, port, env_extra):
    home = os.path.join(WORK, name)
    os.makedirs(home, exist_ok=True)
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + '/.config', XDG_DATA_HOME=home + '/.local/share')
    env.update(env_extra)
    log = open(home + '/log.txt', 'w')
    p = subprocess.Popen([VLC, '-I', 'dummy', '--no-video', '--aout', 'adummy', '-v', '--wlc-port', str(port)],
                         env=env, stdout=log, stderr=log)
    procs.append(p)
    base = 'http://127.0.0.1:%d' % port
    for _ in range(60):
        try:
            urllib.request.urlopen(base + '/api/info', timeout=2)
            return base, home
        except Exception:
            time.sleep(0.2)
    raise SystemExit('WLC did not start: ' + home + '/log.txt')


def call(base, method, path, body=None, timeout=200):
    req = urllib.request.Request(base + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'{}')


def wait_job(base, jid, secs=120):
    end = time.time() + secs
    while time.time() < end:
        j = next((x for x in call(base, 'GET', '/api/dl/jobs')[1]['jobs'] if x['id'] == jid), None)
        if j and j['state'] in ('done', 'error', 'cancelled'):
            return j
        time.sleep(0.4)
    return next((x for x in call(base, 'GET', '/api/dl/jobs')[1]['jobs'] if x['id'] == jid), {})


def add(base, **body):
    code, r = call(base, 'POST', '/api/dl/add', body)
    return r.get('id') if code == 200 else (code, r)


def leftovers(d):
    return [os.path.join(r, n) for r, ds, fs in os.walk(d) for n in ds + fs if n.startswith('.wlc-dl')]


try:
    # A: no ffmpeg, so WLC joins and converts with its own engine
    A, homeA = start_wlc('A', 48760, {'WLC_FFMPEG': 'none', 'WLC_YTDLP': shutil.which('yt-dlp')})
    out = os.path.join(WORK, 'out')
    os.makedirs(out)

    print('Tool status')
    code, t = call(A, 'GET', '/api/dl/tool')
    check('yt-dlp is found, with its version', code == 200 and t['ytdlp'] and t['version'], t)
    check('no ffmpeg (forced) is reported', t['ffmpeg'] is None, t)
    check('default folders are Movies/Music "WLC Downloads"', t['videoDir'].endswith('/WLC Downloads')
          and t['audioDir'].endswith('/WLC Downloads') and t['videoDir'] != t['audioDir'], t)

    print('Looking up links')
    code, i = call(A, 'POST', '/api/dl/probe', {'url': WEB + '/dash/show.mpd'})
    check('a DASH stream: not a playlist, heights listed', code == 200 and not i['playlist'] and 240 in i['heights'], (code, i))
    code, i = call(A, 'POST', '/api/dl/probe', {'url': WEB + '/feed.xml'})
    check('an RSS feed is a playlist of 3 with titles', code == 200 and i['playlist'] and i['count'] == 3
          and [e['title'] for e in i['entries']] == ['Episode 1', 'Episode 2', 'Episode 3'], (code, i))
    code, i = call(A, 'POST', '/api/dl/probe', {'url': WEB + '/missing.mp4'})
    check('a dead link explains itself', code == 422 and '404' in i.get('error', ''), (code, i))
    for bad in ('file:///etc/passwd', 'ftp://x/y', ' http://x', '--exec=rm -rf ~'):
        code, _ = call(A, 'POST', '/api/dl/probe', {'url': bad})
        check('refuses link %r' % bad, code == 400, code)

    print('Video')
    jid = add(A, url=WEB + '/clip.mp4', mode='video', quality='best', format='mp4', dir=out, title='Clip')
    j = wait_job(A, jid)
    f = j.get('files', [])
    check('a direct MP4 downloads', j.get('state') == 'done' and len(f) == 1 and f[0].endswith('/clip.mp4'), j)
    check('...and plays (H.264 + AAC)', f and codecs(f[0]) == ['audio:aac', 'video:h264'], f and codecs(f[0]))
    jid = add(A, url=WEB + '/clip.mp4', mode='video', quality='best', format='mp4', dir=out)
    j = wait_job(A, jid)
    check('the same file again is never overwritten: "clip (2).mp4"', j.get('state') == 'done'
          and j['files'][0].endswith('/clip (2).mp4'), j)
    jid = add(A, url=WEB + '/dash/show.mpd', mode='video', quality='720', format='mp4', dir=out)
    j = wait_job(A, jid)
    f = j.get('files', [])
    check('DASH video + separate audio is joined by WLC (no ffmpeg) into one MP4', j.get('state') == 'done'
          and len(f) == 1 and f[0].endswith('.mp4') and codecs(f[0]) == ['audio:aac', 'video:h264'], (j, f and codecs(f[0])))
    d = float(probe(f[0])['format']['duration']) if f else 0
    check('the joined file is the full length', 7 < d < 9.5, d)
    jid = add(A, url=WEB + '/dash/show.mpd', mode='video', quality='best', format='mkv', dir=out)
    j = wait_job(A, jid)
    f = j.get('files', [])
    check('...or into MKV', j.get('state') == 'done' and f and f[0].endswith('.mkv')
          and codecs(f[0]) == ['audio:aac', 'video:h264'], (j, f and codecs(f[0])))
    check('no temporary folders are left behind', not leftovers(out), leftovers(out))

    print('Audio')
    jid = add(A, url=WEB + '/clip.mp4', mode='audio', quality='192', format='mp3', dir=out)
    j = wait_job(A, jid)
    f = j.get('files', [])
    check('audio only: WLC converts to MP3 itself (no ffmpeg)', j.get('state') == 'done' and f
          and f[0].endswith('.mp3') and codecs(f[0]) == ['audio:mp3'], (j, f and codecs(f[0])))
    jid = add(A, url=WEB + '/dash/show.mpd', mode='audio', quality='best', format='m4a', dir=out)
    j = wait_job(A, jid)
    f = j.get('files', [])
    check('audio only from DASH, kept as AAC (.m4a), no video', j.get('state') == 'done' and f
          and f[0].endswith('.m4a') and codecs(f[0]) == ['audio:aac'], (j, f and codecs(f[0])))

    print('Playlists')
    jid = add(A, url=WEB + '/feed.xml', mode='audio', quality='best', format='mp3', dir=out, playlist=True, items='1,3')
    j = wait_job(A, jid)
    names = sorted(os.path.relpath(x, out) for x in j.get('files', []))
    check('only the chosen items (1 and 3), in a folder named after the playlist', j.get('state') == 'done'
          and names == ['Test Show/1 - Episode 1.mp3', 'Test Show/3 - Episode 3.mp3'], (j, names))
    check('progress said which item it was on', j.get('items') == 3 or j.get('item') in (1, 3), j)
    jid = add(A, url=WEB + '/feed.xml', mode='audio', quality='best', format='mp3', dir=out, playlist=False)
    j = wait_job(A, jid)
    check('"just this one" downloads a single item', j.get('state') == 'done' and len(j['files']) == 1, j)

    print('Progress and cancel')
    jid = add(A, url=WEB + '/slow/big.mp4', mode='video', quality='best', format='mp4', dir=out)
    seen = None
    for _ in range(80):
        jj = next(x for x in call(A, 'GET', '/api/dl/jobs')[1]['jobs'] if x['id'] == jid)
        if jj['state'] == 'downloading' and 0.02 < jj['progress'] < 0.9 and jj['speed'] > 0:
            seen = jj
            break
        time.sleep(0.25)
    check('progress and speed are reported while downloading', bool(seen), jj)
    code, _ = call(A, 'POST', '/api/dl/cancel', {'id': jid})
    j = wait_job(A, jid, 20)
    check('cancel stops it', code == 200 and j.get('state') == 'cancelled', j)
    time.sleep(1)
    check('...and leaves no partial files', not leftovers(out)
          and not [n for n in os.listdir(out) if n.startswith('big')], os.listdir(out))
    check('cancel of an unknown id is a 404', call(A, 'POST', '/api/dl/cancel', {'id': 9999})[0] == 404)
    jid = add(A, url=WEB + '/nothing-here.mp4', mode='video', quality='best', format='mp4', dir=out)
    j = wait_job(A, jid)
    check('a failed download says why', j.get('state') == 'error' and j.get('msg'), j)

    print('Checks on what is asked for')
    base = dict(url=WEB + '/clip.mp4', mode='video', quality='best', format='mp4')
    check('unknown format refused', add(A, **dict(base, format='exe'))[0] == 400)
    check('audio format with video mode refused', add(A, **dict(base, format='mp3'))[0] == 400)
    check('items must be numbers', add(A, **dict(base, items='1;rm'))[0] == 400)
    check('relative or ../ folders refused', add(A, **dict(base, dir='../etc'))[0] == 400
          and add(A, **dict(base, dir='/tmp/../etc'))[0] == 400)
    lib_before = call(A, 'GET', '/api/info')[1]
    jid = add(A, url=WEB + '/ep2.mp3', mode='audio', quality='best', format='mp3')
    j = wait_job(A, jid)
    roots = json.dumps(call(A, 'GET', '/api/info')[1].get('roots', []))
    check('with no folder given it goes to Music/WLC Downloads, added to the Media Center',
          j.get('state') == 'done' and '/Music/WLC Downloads/' in j['files'][0] and 'WLC Downloads' in roots, (j, roots))
    code, _ = call(A, 'POST', '/api/dl/clear', {})
    left = call(A, 'GET', '/api/dl/jobs')[1]['jobs']
    check('"clear finished" empties the list but keeps the files', code == 200 and not left
          and os.path.exists(j['files'][0]), left)
    lan = 'http://%s:48760' % lan_ip()
    code, _ = call(lan, 'POST', '/api/dl/add', base)
    check('other devices on the network cannot use Downloads', code in (401, 403), code)

    print('Play now')
    code, r = call(A, 'POST', '/api/dl/play', {'url': WEB + '/dash/show.mpd'})
    st = {}
    for _ in range(40):
        st = call(A, 'GET', '/api/status')[1]
        if st.get('state') == 'playing' and st.get('time', 0) > 1:
            break
        time.sleep(0.25)
    check('streams a link straight into WLC (video + separate audio)', code == 200 and st.get('state') == 'playing', (code, r, st))
    call(A, 'POST', '/api/cmd', {'cmd': 'stop'})

    # B: ffmpeg present, and yt-dlp set up by WLC itself from a (local) release
    print('With ffmpeg, and setting up yt-dlp')
    B, homeB = start_wlc('B', 48761, {'WLC_YTDLP_RELEASE': WEB + '/rel-bad'})
    code, t = call(B, 'GET', '/api/dl/tool')
    check('uses a yt-dlp already on the computer', t['ytdlp'] and not t['own'] and t['ffmpeg'], t)
    os.environ['WLC_YTDLP_RELEASE'] = WEB + '/rel-bad'
    code, r = call(B, 'POST', '/api/dl/install', {})
    check('a download that does not match its checksum is refused', code == 502 and 'checksum' in r.get('error', ''), (code, r))
    def own_copy():
        for r, ds, fs in os.walk(homeB):
            if 'yt-dlp' in fs and r.endswith('/bin'):
                return os.path.join(r, 'yt-dlp')
        return None
    check('...and nothing is installed', own_copy() is None and call(B, 'GET', '/api/dl/tool')[1]['own'] is False)
    procs[-1].terminate(); procs[-1].wait(10)
    B, homeB = start_wlc('B', 48761, {'WLC_YTDLP_RELEASE': WEB + '/rel-good'})
    code, t = call(B, 'POST', '/api/dl/install', {})
    check('a matching download is installed into WLC\'s own folder', code == 200 and t.get('own') and t.get('version'), (code, t))
    own = own_copy()
    check('...as an executable', own and os.stat(own).st_mode & stat.S_IXUSR, own)
    outB = os.path.join(WORK, 'outB')
    os.makedirs(outB)
    jid = add(B, url=WEB + '/dash/show.mpd', mode='video', quality='best', format='mp4', dir=outB)
    j = wait_job(B, jid)
    f = j.get('files', [])
    check('with ffmpeg, yt-dlp joins the video itself', j.get('state') == 'done' and f
          and codecs(f[0]) == ['audio:aac', 'video:h264'], (j, f and codecs(f[0])))
    jid = add(B, url=WEB + '/ep1.mp3', mode='audio', quality='192', format='opus', dir=outB)
    j = wait_job(B, jid)
    f = j.get('files', [])
    check('with ffmpeg, audio is extracted to Opus by yt-dlp', j.get('state') == 'done' and f
          and f[0].endswith('.opus') and codecs(f[0]) == ['audio:opus'], (j, f and codecs(f[0])))
finally:
    for p in procs:
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
    srv.shutdown()

print('\n%d passed, %d failed' % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
