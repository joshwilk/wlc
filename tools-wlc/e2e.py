#!/usr/bin/env python3
"""End-to-end checks for the WLC Control Center against a real build.

Starts two headless WLC instances with separate settings folders, then
exercises the library, player, Power EQ (measuring real audio output),
converter, bulk renamer, access control, byte-range streaming and WilkCast.

    python3 tools-wlc/e2e.py /path/to/build/vlc [lan-ip]

Must be run as a normal user (the player refuses to run as root).
"""
import json, os, shutil, struct, subprocess, sys, tempfile, time, urllib.parse, urllib.request, urllib.error, wave, math, socket

VLC = sys.argv[1]
WORK = tempfile.mkdtemp(prefix='wlc-e2e-')
PASS = FAIL = 0
procs = []


def lan_ip():
    if len(sys.argv) > 2:
        return sys.argv[2]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('192.0.2.1', 9))
        return s.getsockname()[0]
    finally:
        s.close()


def check(name, cond, detail=''):
    global PASS, FAIL
    if cond:
        PASS += 1
        print('  ok   ' + name)
    else:
        FAIL += 1
        print('  FAIL ' + name + ('  -> ' + str(detail) if detail != '' else ''))


class Client:
    def __init__(self, base):
        self.base, self.cookie = base, None

    def req(self, method, path, body=None, headers=None, raw=False):
        h = dict(headers or {})
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            h['Content-Type'] = 'application/json'
        if self.cookie:
            h['Cookie'] = self.cookie
        r = urllib.request.Request(self.base + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(r, timeout=20) as f:
                sc = f.headers.get('Set-Cookie')
                if sc:
                    self.cookie = sc.split(';')[0]
                b = f.read()
                return f.status, (b if raw else json.loads(b or b'{}')), f.headers
        except urllib.error.HTTPError as e:
            b = e.read()
            try:
                return e.code, json.loads(b or b'{}'), e.headers
            except ValueError:
                return e.code, {}, e.headers

    def get(self, path, **kw):
        return self.req('GET', path, **kw)

    def post(self, path, body=None, **kw):
        return self.req('POST', path, body or {}, **kw)


def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y'] + list(args), check=True)


def start(name, port, extra=()):
    home = os.path.join(WORK, name)
    os.makedirs(home, exist_ok=True)
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + '/.config',
               XDG_DATA_HOME=home + '/.local/share', XDG_CACHE_HOME=home + '/.cache')
    log = open(os.path.join(home, 'log.txt'), 'w')
    p = subprocess.Popen([VLC, '-I', 'dummy', '--no-video', '--vout', 'vdummy', '--no-color', '-v',
                          '--wlc-port', str(port)] + list(extra), env=env, stdout=log, stderr=log)
    procs.append(p)
    c = Client('http://127.0.0.1:%d' % port)
    for _ in range(60):
        try:
            if c.get('/api/info')[0] == 200:
                return c, home, p
        except Exception:
            time.sleep(0.2)
    print('WLC did not start. Last lines of its log:')
    try:
        print(''.join(open(os.path.join(home, 'log.txt'), errors='replace').readlines()[-60:]))
    except OSError as e:
        print(e)
    print('exit code:', p.poll())
    raise SystemExit('WLC did not start; see ' + home + '/log.txt')


def settings_file(home):
    """wlc.json lives in the player's configuration folder, which differs by system."""
    for d in ('.config/vlc', 'Library/Preferences/io.github.joshwilk.wlc', 'Library/Preferences/org.videolan.vlc'):
        f = os.path.join(home, d, 'wlc.json')
        if os.path.exists(f):
            return f
    return None


def wait(fn, secs=30, step=0.25):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return None


def rms_db(path, skip=0.6):
    with wave.open(path, 'rb') as w:
        n, ch, sw, rate = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        w.setpos(min(n, int(skip * rate)))
        raw = w.readframes(n)
    if sw == 2:
        vals = [v / 32768.0 for v in struct.unpack('<%dh' % (len(raw) // 2), raw)]
    else:
        vals = list(struct.unpack('<%df' % (len(raw) // 4), raw))
    left = vals[0::ch]
    right = vals[1::ch] if ch > 1 else left
    f = lambda a: 20 * math.log10(max(1e-9, math.sqrt(sum(x * x for x in a) / max(1, len(a)))))
    return f(left), f(right), max(abs(x) for x in vals) if vals else 0


WILKCAST = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wilkcast.py')


def wilkcast(*args, timeout=30):
    r = subprocess.run([sys.executable, WILKCAST] + list(args), capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def cast_v11(A, B, infoA, infoB, ip, media, clip):
    print('WilkCast 1.1: queues')
    B.post('/api/settings', {'castAccept': 'pin'})
    # distinct names, so devices can be picked by name
    infoA = A.post('/api/settings', {'name': 'WLC Alpha'})[1]
    infoB = B.post('/api/settings', {'name': 'WLC Bravo'})[1]
    tracks = sorted(os.path.join(media, 'Music/Album', f) for f in os.listdir(media + '/Music/Album'))
    r = A.post('/api/cast/send', {'peer': infoB['id'], 'pin': infoB['pin'], 'paths': tracks})
    check('casting a list of 3 files is accepted', r[0] == 200, r)
    pl = wait(lambda: (lambda x: x if len(x.get('items', [])) == 3 else None)(B.get('/api/playlist')[1]), 10)
    check('receiver lines up all 3 (and nothing else)', bool(pl) and pl['items'][0]['current'], B.get('/api/playlist')[1])
    first = wait(lambda: (lambda x: x if x['state'] == 'playing' else None)(B.get('/api/status')[1]), 10)
    A.post('/api/cast/control', {'peer': infoB['id'], 'pin': infoB['pin'], 'cmd': 'next'})
    nxt = wait(lambda: (lambda x: x if x['state'] == 'playing' and x.get('title') != first.get('title') else None)(B.get('/api/status')[1]), 10)
    check('"next" moves through the cast queue', bool(first) and bool(nxt), (first, B.get('/api/status')[1]))
    r = A.post('/api/cast/send', {'peer': infoB['id'], 'pin': infoB['pin'], 'paths': [clip]})
    pl = wait(lambda: (lambda x: x if len(x.get('items', [])) == 1 else None)(B.get('/api/playlist')[1]), 10)
    check('a later single-file cast is accepted', r[0] == 200, r)
    B.post('/api/cmd', {'cmd': 'stop'})

    print('WilkCast 1.1: browser screens')
    scr = Client('http://%s:48720' % ip)          # a "TV" on the LAN, talking to A
    code, page, hdrs = scr.get('/screen', raw=True)
    check('/screen serves the screen page with media allowed from the LAN',
          code == 200 and b'WLC Screen' in page and 'media-src *' in (hdrs.get('Content-Security-Policy') or ''), hdrs)
    check('a screen cannot register without the PIN', scr.post('/api/screen/register', {'name': 'Den TV'})[0] == 401)
    # the access-control tests above locked A's PIN for 60 s on purpose: wait it out
    check('screen signs in with the PIN', bool(wait(lambda: scr.post('/api/login', {'pin': infoA['pin']})[0] == 200, 75, 3)))
    code, reg, _ = scr.post('/api/screen/register', {'name': 'Den TV'})
    sid = reg.get('id', '')
    check('screen registers and gets an id', code == 200 and len(sid) == 32 and reg['name'] == 'Den TV', reg)
    status = {'state': 'stopped'}
    inbox = []

    def sync():
        c, r, _ = scr.post('/api/screen/sync', {'id': sid, 'status': status})
        if c == 200:
            inbox.extend(r['cmds'])
        return c

    check('screen sync works', sync() == 200)
    peersA = A.get('/api/peers')[1]['peers']
    check('host lists its own screen as a browser cast target',
          any(p['id'] == sid and p['kind'] == 'browser' and p['own'] for p in peersA), peersA)
    bp = wait(lambda: [p for p in B.get('/api/peers')[1]['peers'] if p['id'] == sid], 12, 0.5)
    sync()
    check('another WLC discovers the screen over UDP, with its own base path',
          bool(bp) and bp[0]['kind'] == 'browser' and bp[0]['ip'] != '127.0.0.1', B.get('/api/peers')[1])
    v1 = Client('http://%s:48720' % ip)
    code, inf, _ = v1.get('/cast/v1/s/%s/info' % sid)
    check('screen info is public, says browser', code == 200 and inf.get('kind') == 'browser' and inf.get('name') == 'Den TV', inf)
    check('host info lists its screens', any(x['id'] == sid for x in v1.get('/cast/v1/info')[1].get('screens', [])))
    check('casting to a screen without the PIN is refused',
          v1.post('/cast/v1/s/%s/play' % sid, {'url': 'http://example.com/a.mp4'})[0] == 401)
    check('screens refuse non-http casts too',
          v1.post('/cast/v1/s/%s/play' % sid, {'url': 'file:///etc/passwd'}, headers={'X-WilkCast-Pin': infoA['pin']})[0] == 400)
    check('unknown screen is a 404',
          v1.post('/cast/v1/s/%s/play' % ('0' * 32), {'url': 'http://x/a.mp4'}, headers={'X-WilkCast-Pin': infoA['pin']})[0] == 404)
    # B (another computer) casts a queue to A's screen
    r = B.post('/api/cast/send', {'peer': sid, 'pin': infoA['pin'], 'paths': [clip, tracks[0]]})
    check('another WLC casts a queue to the screen', r[0] == 200, r)
    sync()
    load = next((c for c in inbox if c.get('cmd') == 'load'), None)
    check('screen receives a load command with the queue', bool(load) and len(load.get('queue', [])) == 1 and load['title'] == 'clip.mp4', inbox)
    if load:
        req = urllib.request.Request(load['url'], headers={'Range': 'bytes=0-99'})
        try:
            with urllib.request.urlopen(req, timeout=10) as f:
                ok = f.status == 206 and len(f.read()) == 100
        except Exception as e:
            ok = e
        check('the screen can fetch the media from the sender, with ranges', ok is True and urllib.parse.urlparse(load['url']).hostname == ip, (ok, load['url']))
    inbox.clear()
    status = {'state': 'playing', 'time': 4.2, 'length': 30, 'title': 'clip.mp4', 'volume': 1}
    sync()
    code, st, _ = B.get('/api/cast/status?' + urllib.parse.urlencode({'peer': sid, 'pin': infoA['pin']}))
    check('sender sees the status the screen reported', code == 200 and st.get('state') == 'playing' and abs(st.get('time', 0) - 4.2) < 0.01 and st.get('kind') == 'browser', st)
    B.post('/api/cast/control', {'peer': sid, 'pin': infoA['pin'], 'cmd': 'seekrel', 'value': 30})
    B.post('/api/cast/control', {'peer': sid, 'pin': infoA['pin'], 'cmd': 'pause'})
    check('remote controls reach the screen in order', sync() == 200 and [c['cmd'] for c in inbox] == ['seekrel', 'pause'] and inbox[0]['value'] == 30, inbox)
    check('screen refuses commands outside the cast set',
          B.post('/api/cast/control', {'peer': sid, 'pin': infoA['pin'], 'cmd': 'clear'})[0] == 400)
    inbox.clear()
    status = {'state': 'error', 'error': 'this browser cannot play MKV files', 'title': 'x.mkv'}
    sync()
    st = A.get('/api/cast/status?' + urllib.parse.urlencode({'peer': sid}))[1]
    check('playback errors on the screen reach the sender', st.get('state') == 'error' and 'MKV' in st.get('error', ''), st)
    # A casts to its own screen: the URL must use the address the TV reached A by
    A.post('/api/cast/send', {'peer': sid, 'path': clip})
    sync()
    load = next((c for c in inbox if c.get('cmd') == 'load'), {})
    check('casting to its own screen uses the address the screen knows',
          urllib.parse.urlparse(load.get('url', '')).hostname == ip, load)
    inbox.clear()

    print('WilkCast 1.1: wilkcast.py (stand-alone sender and receiver)')
    rc, out = wilkcast('discover', '-t', '6', '--json')
    devs = json.loads(out[:out.rindex(']') + 1]) if rc == 0 and ']' in out else []
    names = {d['name'] for d in devs}
    check('wilkcast.py discovers both WLCs and the screen', {infoA['name'], infoB['name'], 'Den TV'} <= names, out[-400:])
    sync()
    rx = subprocess.Popen([sys.executable, WILKCAST, 'receive', '--name', 'Py Receiver', '--pin', '424242',
                           '--port', '48790', '--player', 'none'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(rx)
    pr = wait(lambda: [p for p in A.get('/api/peers')[1]['peers'] if p['name'] == 'Py Receiver'], 12, 0.5)
    check('WLC discovers a wilkcast.py receiver', bool(pr) and pr[0]['kind'] == 'script', A.get('/api/peers')[1])
    if pr:
        r = A.post('/api/cast/send', {'peer': pr[0]['id'], 'pin': '424242', 'paths': tracks})
        check('WLC casts a queue to wilkcast.py', r[0] == 200, r)
        rc, out = wilkcast('status', '%s:48790' % ip, '--pin', '424242')
        check('wilkcast.py receiver reports what it plays', rc == 0 and 'playing' in out and os.path.basename(tracks[0]) in out, out)
        A.post('/api/cast/control', {'peer': pr[0]['id'], 'pin': '424242', 'cmd': 'next'})
        rc, out = wilkcast('status', 'Py Receiver', '--pin', '424242')
        check('...and follows "next" through the queue (found by name)', rc == 0 and os.path.basename(tracks[1]) in out, out)
        check('wrong PIN to wilkcast.py receiver is refused', A.post('/api/cast/send', {'peer': pr[0]['id'], 'pin': '000000', 'path': clip})[0] == 401)
    sender = subprocess.Popen([sys.executable, WILKCAST, 'cast', '%s:48740' % ip, tracks[2], clip, '--pin', infoB['pin']],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(sender)
    st = wait(lambda: (lambda x: x if x['state'] == 'playing' and x['time'] > 1 else None)(B.get('/api/status')[1]), 15)
    pl = B.get('/api/playlist')[1]
    check('wilkcast.py serves local files to WLC, which plays them (queue of 2)', bool(st) and len(pl.get('items', [])) == 2, (st, pl))
    rc, out = wilkcast('ctl', 'WLC Bravo', 'seek', '5', '--pin', infoB['pin'])
    st = wait(lambda: (lambda x: x if x['time'] >= 4.5 else None)(B.get('/api/status')[1]), 8)
    check('wilkcast.py seeks on WLC (needs byte ranges from its own server)', rc == 0 and bool(st), (out, B.get('/api/status')[1]))
    sender.terminate(); sender.wait(5)
    sender = subprocess.Popen([sys.executable, WILKCAST, 'cast', 'Den TV', clip, '--pin', infoA['pin']],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(sender)
    got = wait(lambda: (sync(), next((c for c in inbox if c.get('cmd') == 'load'), None))[1], 15, 0.5)
    check('wilkcast.py casts to a browser screen found by name', bool(got) and got['url'].startswith('http://' + ip + ':'), inbox)
    sender.terminate(); sender.wait(5)
    B.post('/api/cmd', {'cmd': 'stop'})
    rx.terminate(); rx.wait(5)
    gone = wait(lambda: not [p for p in A.get('/api/peers')[1]['peers'] if p['name'] == 'Py Receiver'], 8, 0.5)
    check('a receiver that quits says goodbye and leaves the list', bool(gone))
    t0 = time.time()
    gone = wait(lambda: not [p for p in B.get('/api/peers')[1]['peers'] if p['id'] == sid], 30, 1)
    check('a screen whose page closed disappears for other WLCs (%.0f s)' % (time.time() - t0), bool(gone))


def main():
    ip = lan_ip()
    media = os.path.join(WORK, 'media')
    os.makedirs(media + '/Music/Album')
    os.makedirs(media + '/Video')
    os.makedirs(WORK + '/private')
    for i in (1, 2, 3):
        ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=%d:duration=8' % (300 * i), '-metadata', 'title=Song %d' % i,
               '-metadata', 'artist=Test Band', '-metadata', 'album=Demo', '-metadata', 'track=%d' % i,
               '-c:a', 'libmp3lame', '-b:a', '128k', '%s/Music/Album/0%d_some.track-%d.mp3' % (media, i, i))
    ffmpeg('-f', 'lavfi', '-i', 'testsrc=duration=30:size=640x360:rate=25', '-f', 'lavfi', '-i', 'sine=frequency=440:duration=30',
           '-c:v', 'libx264', '-g', '25', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-shortest', media + '/Video/clip.mp4')
    ffmpeg('-f', 'lavfi', '-i', 'aevalsrc=0.1*sin(2*PI*1000*t):d=6:s=48000', '-ac', '2', media + '/Music/tone1k.wav')
    ffmpeg('-f', 'lavfi', '-i', 'aevalsrc=0.1*sin(2*PI*100*t):d=6:s=48000', '-ac', '2', media + '/Music/tone100.wav')
    open(WORK + '/private/secret.mp3', 'wb').write(b'x' * 4000)

    print('Starting two WLC instances in', WORK)
    A, homeA, procA = start('A', 48720, ['--aout', 'adummy'])
    B, homeB, _ = start('B', 48740, ['--aout', 'adummy'])
    infoA = A.get('/api/info')[1]
    infoB = B.get('/api/info')[1]

    print('Library and player')
    check('first run creates a PIN and id', len(infoA['pin']) == 6 and len(infoA['id']) == 32 and infoA['pin'] != '')
    cfg = settings_file(homeA)
    check('settings file is private (0600)', cfg is not None and oct(os.stat(cfg).st_mode & 0o777) == '0o600', cfg)
    check('refuses to share the whole disk', A.post('/api/roots', {'add': '/'})[0] == 400)
    check('add library folder', A.post('/api/roots', {'add': media})[0] == 200)
    lib = A.get('/api/library')[1]['items']
    check('library scan finds 6 files', len(lib) == 6, len(lib))
    t1 = media + '/Music/Album/01_some.track-1.mp3'
    st = A.post('/api/cmd', {'cmd': 'add', 'paths': [t1, media + '/Music/Album/02_some.track-2.mp3']})
    check('play two files', st[0] == 200)
    s = wait(lambda: (lambda x: x if x['state'] == 'playing' and x['time'] > 0.5 else None)(A.get('/api/status')[1]))
    check('status shows playing with tags', bool(s) and s['title'] == 'Song 1' and s['artist'] == 'Test Band', s)
    pl = A.get('/api/playlist')[1]['items']
    check('queue lists 2 items, first is current', len(pl) == 2 and pl[0]['current'], pl)
    A.post('/api/cmd', {'cmd': 'seek', 'value': 5})
    s = wait(lambda: (lambda x: x if x['time'] >= 4.5 else None)(A.get('/api/status')[1]), 5)
    check('seek to 5 s', bool(s), A.get('/api/status')[1]['time'])
    A.post('/api/cmd', {'cmd': 'pause'})
    check('pause', wait(lambda: A.get('/api/status')[1]['state'] == 'paused', 5))
    A.post('/api/cmd', {'cmd': 'next'})
    s = wait(lambda: (lambda x: x if x['title'] == 'Song 2' else None)(A.get('/api/status')[1]), 6)
    check('next track', bool(s))
    A.post('/api/cmd', {'cmd': 'stop'})
    m = A.get('/api/meta?path=' + urllib.request.quote(media + '/Music/Album/03_some.track-3.mp3'))[1]
    check('tags read (title/artist/track)', m.get('title') == 'Song 3' and m.get('track') == '3' and m.get('duration', 0) > 7, m)

    print('Access control')
    R = Client('http://%s:48720' % ip)
    check('other device sees no PIN in /api/info', 'pin' not in R.get('/api/info')[1] and not R.get('/api/info')[1]['authed'])
    check('other device blocked without login', R.get('/api/status')[0] == 401 and R.get('/api/library')[0] == 401)
    check('wrong PIN rejected', R.post('/api/login', {'pin': '000000' if infoA['pin'] != '000000' else '111111'})[0] == 401)
    check('right PIN accepted', R.post('/api/login', {'pin': infoA['pin']})[0] == 200 and R.cookie)
    check('logged-in device can read status and library', R.get('/api/status')[0] == 200 and len(R.get('/api/library')[1]['items']) == 6)
    check('logged-in device cannot rename', R.post('/api/rename', {'pairs': [{'from': t1, 'to': t1 + 'x'}]})[0] == 403)
    check('logged-in device cannot convert', R.post('/api/convert', {'files': [t1], 'profile': 'mp3'})[0] == 403)
    check('logged-in device cannot change settings', R.post('/api/settings', {'name': 'hax'})[0] == 403)
    check('logged-in device cannot add folders', R.post('/api/roots', {'add': WORK + '/private'})[0] == 403)
    check('logged-in device cannot read outside shared folders',
          R.get('/api/file?path=' + urllib.request.quote(WORK + '/private/secret.mp3'), raw=True)[0] == 404
          and R.get('/api/browse?path=' + urllib.request.quote(WORK + '/private'))[0] == 404
          and R.get('/api/browse?path=/etc')[0] == 404)
    check('path tricks do not escape a shared folder',
          R.get('/api/file?path=' + urllib.request.quote(media + '/../private/secret.mp3'), raw=True)[0] == 404)
    os.symlink(WORK + '/private/secret.mp3', media + '/Music/link.mp3')
    check('a symlink pointing outside is not served',
          R.get('/api/file?path=' + urllib.request.quote(media + '/Music/link.mp3'), raw=True)[0] == 404)
    os.unlink(media + '/Music/link.mp3')
    check('remote cannot open file:// or other schemes', R.post('/api/cmd', {'cmd': 'add', 'url': 'file:///etc/passwd'})[0] == 400)
    check('cross-site page is refused (Origin check)', A.post('/api/cmd', {'cmd': 'stop'}, headers={'Origin': 'http://evil.example'})[0] == 403)
    check('DNS-rebinding style Host is refused', A.get('/api/info', headers={'Host': 'evil.example:48720'})[0] == 403)
    check('media URL needs the key', A.get('/media/' + '0' * 32 + t1, raw=True)[0] == 403)
    X = Client('http://%s:48720' % ip)
    codes = [X.post('/api/login', {'pin': 'zzzzzz'})[0] for _ in range(6)]
    check('PIN guessing gets locked out', X.post('/api/login', {'pin': infoA['pin']})[0] == 401, codes)

    print('Byte ranges')
    clip = media + '/Video/clip.mp4'
    whole = open(clip, 'rb').read()
    c, b, hd = R.get('/api/file?path=' + urllib.request.quote(clip), raw=True)
    check('full file identical', c == 200 and b == whole and hd.get('Accept-Ranges') == 'bytes')
    c, b, hd = R.get('/api/file?path=' + urllib.request.quote(clip), raw=True, headers={'Range': 'bytes=1000-1999'})
    check('range 1000-1999', c == 206 and b == whole[1000:2000] and hd.get('Content-Range') == 'bytes 1000-1999/%d' % len(whole), hd.get('Content-Range'))
    c, b, hd = R.get('/api/file?path=' + urllib.request.quote(clip), raw=True, headers={'Range': 'bytes=-500'})
    check('suffix range', c == 206 and b == whole[-500:])
    c, b, hd = R.get('/api/file?path=' + urllib.request.quote(clip), raw=True, headers={'Range': 'bytes=%d-' % (len(whole) + 5)})
    check('range past the end gives 416', c == 416)

    print('Bulk renamer')
    d = media + '/Music/Album'
    names = sorted(os.listdir(d))
    pairs = [{'from': d + '/' + n, 'to': d + '/%02d - Song %d.mp3' % (i + 1, i + 1)} for i, n in enumerate(names)]
    r = A.post('/api/rename', {'pairs': pairs})
    check('rename 3 files', r[0] == 200 and r[1]['renamed'] == 3 and sorted(os.listdir(d)) == ['01 - Song 1.mp3', '02 - Song 2.mp3', '03 - Song 3.mp3'], os.listdir(d))
    batch = r[1].get('batch')
    cur = sorted(os.listdir(d))
    swap = [{'from': d + '/' + cur[0], 'to': d + '/' + cur[1]}, {'from': d + '/' + cur[1], 'to': d + '/' + cur[0]}]
    size0 = os.path.getsize(d + '/' + cur[0]); ino0 = os.stat(d + '/' + cur[0]).st_ino
    r = A.post('/api/rename', {'pairs': swap})
    check('swap two names in one batch', r[0] == 200 and os.stat(d + '/' + cur[1]).st_ino == ino0)
    A.post('/api/rename/undo', {'batch': r[1]['batch']})
    check('undo the swap', os.stat(d + '/' + cur[0]).st_ino == ino0)
    r = A.post('/api/rename', {'pairs': [{'from': d + '/' + cur[0], 'to': d + '/' + cur[2]}]})
    check('refuses to overwrite an existing file', r[0] == 409 and sorted(os.listdir(d)) == cur, r)
    r = A.post('/api/rename', {'pairs': [{'from': d + '/' + cur[0], 'to': d + '/same.mp3'}, {'from': d + '/' + cur[1], 'to': d + '/SAME.mp3'}]})
    check('refuses two files with one name (case-insensitive)', r[0] == 409 and sorted(os.listdir(d)) == cur)
    r = A.post('/api/rename', {'pairs': [{'from': d + '/' + cur[0], 'to': d + '/ok.mp3'}, {'from': d + '/missing.mp3', 'to': d + '/x.mp3'}]})
    check('all-or-nothing when one file is missing', r[0] == 409 and sorted(os.listdir(d)) == cur)
    r = A.post('/api/rename', {'pairs': [{'from': d + '/' + cur[0], 'to': d + '/../../escape.mp3'}]})
    check('rejects names with ../', r[0] == 409 and sorted(os.listdir(d)) == cur)
    r = A.post('/api/rename', {'pairs': [{'from': d + '/' + cur[0], 'to': d + '/' + cur[0].upper()}]})
    check('case-only rename works', r[0] == 200 and cur[0].upper() in os.listdir(d), os.listdir(d))
    A.post('/api/rename/undo', {'batch': r[1]['batch']})
    check('undo first batch restores original names', A.post('/api/rename/undo', {'batch': batch})[0] == 200 and sorted(os.listdir(d)) == names, os.listdir(d))
    check('no temp files left behind', not [n for n in os.listdir(d) if n.startswith('.wlc')])
    h = A.get('/api/rename/history')[1]['batches']
    check('history is recorded', len(h) == 3 and all(b['undone'] for b in h), h)

    print('Converter')
    A.post('/api/settings', {'parallel': 2})
    out = WORK + '/out'
    os.makedirs(out)
    for prof in ('mp4-480', 'mkv-h264', 'webm-vp8', 'ts-mpeg2'):
        A.post('/api/convert', {'files': [clip], 'profile': prof, 'out': out})
    for prof in ('mp3', 'm4a', 'ogg', 'opus', 'flac', 'wav'):
        A.post('/api/convert', {'files': [media + '/Music/tone1k.wav'], 'profile': prof, 'out': out})
    A.post('/api/convert', {'files': [clip], 'profile': 'mp3', 'out': out})          # audio pulled from a video
    A.post('/api/convert', {'files': [media + '/Music/tone1k.wav'], 'profile': 'mp3', 'out': out})  # name already taken
    A.post('/api/cmd', {'cmd': 'add', 'paths': [clip]})   # play while converting
    jobs = wait(lambda: (lambda j: j if all(x['state'] in ('done', 'error', 'cancelled') for x in j) else None)(A.get('/api/jobs')[1]['jobs']), 240, 1)
    check('all jobs finished', bool(jobs), A.get('/api/jobs')[1]['jobs'])
    for j in jobs or []:
        ok = j['state'] == 'done' and os.path.exists(j['dst'])
        probe = ''
        if ok:
            probe = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'stream=codec_name:format=duration', '-of', 'csv=p=0', j['dst']],
                                   capture_output=True, text=True).stdout.replace('\n', ' ').strip()
            dur = [float(x) for x in probe.split() if x.replace('.', '', 1).isdigit()]
            if j['dst'].endswith('.flac'):
                # a raw FLAC stream carries no length in its header (same as stock VLC); decode it to measure
                dec = subprocess.run(['ffmpeg', '-v', 'error', '-i', j['dst'], '-f', 'null', '-', '-stats'], capture_output=True, text=True).stderr
                dur = [5.0] if 'time=00:00:05' in dec or 'time=00:00:06' in dec else []
            ok = bool(dur) and dur[-1] > 4
        check('convert -> %s' % os.path.basename(j['dst']), ok, (j['state'], j['msg'], probe))
    check('name clash gets " (2)" instead of overwriting', os.path.exists(out + '/tone1k (2).mp3') or os.path.exists(out + '/clip (2).mp3'), os.listdir(out))
    check('no .part files left', not [n for n in os.listdir(out) if n.endswith('.part')], os.listdir(out))
    check('originals untouched', open(clip, 'rb').read() == whole)
    check('playback kept going during conversion', A.get('/api/status')[1]['state'] in ('playing', 'stopped'))
    A.post('/api/cmd', {'cmd': 'stop'})
    A.post('/api/convert', {'files': [clip], 'profile': 'mp4-h264', 'out': out, 'vb': 6000})
    time.sleep(1.0)
    A.post('/api/jobs/cancel', {'all': True})
    j = wait(lambda: (lambda j: j if j[-1]['state'] in ('cancelled', 'done') else None)(A.get('/api/jobs')[1]['jobs']), 20)
    check('cancel leaves no partial file', bool(j) and not [n for n in os.listdir(out) if n.endswith('.part')], j and j[-1])
    check('custom options cannot smuggle an output path', A.post('/api/convert', {'files': [clip], 'profile': 'mp3', 'custom': "acodec=mp3}:std{dst=/tmp/x"})[0] == 400)

    print('WilkCast (A casts to B)')
    peers = wait(lambda: [p for p in A.get('/api/peers')[1]['peers'] if p['id'] == infoB['id']], 15)
    found = bool(peers)
    check('B discovered automatically over UDP', found, A.get('/api/peers')[1])
    if not found:
        r = A.post('/api/peers/add', {'host': ip, 'port': 48740})
        check('B added by address', r[0] == 200, r)
    check('cast info endpoint is public but minimal', set(Client('http://%s:48740' % ip).get('/cast/v1/info')[1].keys()) == {'wilkcast', 'app', 'version', 'kind', 'id', 'name', 'auth', 'screens'})
    check('cast without PIN is refused', A.post('/api/cast/send', {'peer': infoB['id'], 'path': clip})[0] == 401)
    check('direct cast call without PIN is refused', Client('http://%s:48740' % ip).post('/cast/v1/play', {'url': 'http://example.com/a.mp4'})[0] == 401)
    r = A.post('/api/cast/send', {'peer': infoB['id'], 'path': clip, 'pin': infoB['pin']})
    check('cast with PIN accepted', r[0] == 200, r)
    s = wait(lambda: (lambda x: x if x['state'] == 'playing' and x['time'] > 1 and x['length'] > 20 else None)(B.get('/api/status')[1]), 20)
    check('B is playing the file streamed from A', bool(s) and '/media/' in s['path'] and ip in s['path'], B.get('/api/status')[1])
    A.post('/api/cast/control', {'peer': infoB['id'], 'pin': infoB['pin'], 'cmd': 'seek', 'value': 20})
    s = wait(lambda: (lambda x: x if x['time'] >= 19.5 else None)(B.get('/api/status')[1]), 10)
    check('remote seek to 20 s over HTTP ranges', bool(s), B.get('/api/status')[1]['time'])
    r = A.get('/api/cast/status?peer=%s&pin=%s' % (infoB['id'], infoB['pin']))
    check('sender can read receiver status', r[0] == 200 and r[1]['state'] == 'playing', r)
    A.post('/api/cast/control', {'peer': infoB['id'], 'pin': infoB['pin'], 'cmd': 'pause'})
    check('remote pause', wait(lambda: B.get('/api/status')[1]['state'] == 'paused', 5))
    check('cast control cannot run other commands', A.post('/api/cast/control', {'peer': infoB['id'], 'pin': infoB['pin'], 'cmd': 'clear'})[0] == 400)
    check('receiver refuses non-http casts', Client('http://%s:48740' % ip).post('/cast/v1/play', {'url': 'file:///etc/passwd'}, headers={'X-WilkCast-Pin': infoB['pin']})[0] == 400)
    A.post('/api/cast/control', {'peer': infoB['id'], 'pin': infoB['pin'], 'cmd': 'stop'})
    # move: continue on B from where A is
    A.post('/api/cmd', {'cmd': 'clear'}); A.post('/api/cmd', {'cmd': 'add', 'paths': [clip]})
    wait(lambda: A.get('/api/status')[1]['state'] == 'playing', 8)
    A.post('/api/cmd', {'cmd': 'seek', 'value': 12}); time.sleep(1)
    r = A.post('/api/cast/send', {'peer': infoB['id'], 'pin': infoB['pin'], 'move': True})
    s = wait(lambda: (lambda x: x if x['state'] == 'playing' and x['time'] >= 11 else None)(B.get('/api/status')[1]), 15)
    check('"Move" continues on B from the same spot and stops A', r[0] == 200 and bool(s) and A.get('/api/status')[1]['state'] == 'stopped', (r, B.get('/api/status')[1]))
    B.post('/api/settings', {'castAccept': 'off'})
    check('receiver set to refuse casts', A.post('/api/cast/send', {'peer': infoB['id'], 'path': clip, 'pin': infoB['pin']})[0] == 403)
    B.post('/api/cmd', {'cmd': 'stop'})
    cast_v11(A, B, infoA, infoB, ip, media, clip)

    print('Power EQ (measured on real audio output)')
    def render(tone, eq):
        C, homeC, pc = start('C', 48760, ['--aout', 'afile', '--audiofile-file', WORK + '/cap.wav', '--audiofile-format', 's16', '--audiofile-channels', '2'])
        C.post('/api/eq', eq)
        C.post('/api/cmd', {'cmd': 'add', 'paths': [media + '/Music/' + tone]})
        wait(lambda: C.get('/api/status')[1]['time'] > 4.5, 15)
        C.post('/api/cmd', {'cmd': 'stop'}); time.sleep(0.4)
        pc.terminate(); pc.wait(10); procs.remove(pc)
        shutil.rmtree(homeC, ignore_errors=True)
        return rms_db(WORK + '/cap.wav')
    flat = render('tone1k.wav', {'on': False})
    src = rms_db(media + '/Music/tone1k.wav')
    check('EQ off: 1 kHz tone passes at its own level', abs(flat[0] - src[0]) < 0.3, (flat, src))
    g = ['0'] * 10; g[5] = '12'
    v = render('tone1k.wav', {'on': True, 'graphic': ' '.join(g), 'parametric': '', 'preamp': 0})
    check('graphic +12 dB at 1 kHz raises 1 kHz by ~12 dB', abs((v[0] - flat[0]) - 12) < 0.6, v[0] - flat[0])
    v = render('tone100.wav', {'on': True, 'graphic': ' '.join(g), 'parametric': '', 'preamp': 0})
    check('...and leaves 100 Hz alone', abs(v[0] - flat[0]) < 0.6, v[0] - flat[0])
    g31 = ['0'] * 31; g31[17] = '-9'
    v = render('tone1k.wav', {'on': True, 'graphic': ' '.join(g31), 'parametric': ''})
    check('31-band layout: -9 dB at 1 kHz', abs((v[0] - flat[0]) + 9) < 0.6, v[0] - flat[0])
    v = render('tone100.wav', {'on': True, 'graphic': '', 'parametric': 'highpass,400,0,0.71'})
    check('parametric high-pass at 400 Hz cuts 100 Hz by >20 dB', v[0] - flat[0] < -20, v[0] - flat[0])
    v = render('tone1k.wav', {'on': True, 'graphic': '', 'parametric': 'peak,1000,6,2;lowshelf,120,9,0.7', 'preamp': -3})
    check('parametric peak +6 dB with -3 dB preamp nets ~+3 dB', abs((v[0] - flat[0]) - 3) < 0.6, v[0] - flat[0])
    v = render('tone1k.wav', {'on': True, 'graphic': '', 'parametric': '', 'balance': -1, 'preamp': 0})
    check('balance hard left silences the right channel', abs(v[0] - flat[0]) < 0.6 and v[1] < -80, v)
    v = render('tone1k.wav', {'on': True, 'graphic': '', 'parametric': '', 'preamp': 24, 'limiter': True, 'balance': 0})
    check('clip guard holds a +24 dB boost under full scale', v[2] <= 0.99, v)
    v = render('tone1k.wav', {'on': True, 'graphic': '', 'parametric': '', 'preamp': 24, 'limiter': False})
    check('...and without it the same boost does hit full scale (clips)', v[2] >= 0.999, v)
    check('EQ rejects junk input', A.post('/api/eq', {'graphic': '0 0 $(rm)'})[0] == 400)

    print('Restart keeps settings')
    A.post('/api/eq', {'on': True, 'graphic': '1 2 3 4 5 6 7 8 9 10', 'preamp': -2})
    procA.terminate(); procA.wait(10); procs.remove(procA)
    A2, _, _ = start('A', 48720, ['--aout', 'adummy'])
    i2 = A2.get('/api/info')[1]; e2 = A2.get('/api/eq')[1]
    check('PIN, id, folders and EQ survive a restart', i2['pin'] == infoA['pin'] and i2['id'] == infoA['id'] and i2['roots'] == [os.path.realpath(media)]
          and e2['on'] and e2['graphic'] == '1 2 3 4 5 6 7 8 9 10' and e2['preamp'] == -2, (i2, e2))
    check('rename history survives a restart', len(A2.get('/api/rename/history')[1]['batches']) == 3)
    check('clean shutdown within 5 s', (lambda p: (p.terminate(), p.wait(5)) and True)(procs[-1]))


try:
    main()
except BaseException:
    import traceback
    traceback.print_exc()
    FAIL += 1
    print('  FAIL the test script stopped early (see the traceback above)')
finally:
    for p in procs:
        try:
            p.terminate(); p.wait(5)
        except Exception:
            p.kill()
    print('\n%d passed, %d failed   (work dir: %s)' % (PASS, FAIL, WORK))
    sys.exit(1 if FAIL else 0)
