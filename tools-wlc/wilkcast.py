#!/usr/bin/env python3
"""wilkcast.py - a stand-alone WilkCast 1 sender and receiver (doc/wilkcast.md).

Python 3.8+, standard library only. Works with WLC, with WLC browser screens,
and with other copies of this script.

    wilkcast.py discover [-t SECONDS]
    wilkcast.py info     TARGET
    wilkcast.py cast     TARGET FILE_OR_URL... [--from SECONDS] [--wait]
    wilkcast.py ctl      TARGET pause|resume|toggle|stop|next|prev|seek N|seekrel N|volume N
    wilkcast.py status   TARGET
    wilkcast.py receive  [--name NAME] [--pin PIN|--open] [--port N] [--player mpv|ffplay|vlc|none]

TARGET is a device name or id prefix as shown by `discover`, or HOST[:PORT].
The PIN comes from --pin or the WILKCAST_PIN environment variable.

`cast` serves the files itself over HTTP (with byte ranges, so the receiver
can seek) and keeps running while they play; press Ctrl-C to stop serving.
`receive` makes this computer a receiver; with mpv it supports every command,
with ffplay or vlc only play/stop, and with `none` it just logs what it was
asked to do (handy for testing).

WilkCast and this script are in the public domain.
"""
import argparse, http.server, json, mimetypes, os, secrets, shutil, signal, socket, socketserver
import struct, subprocess, sys, threading, time, urllib.parse, urllib.request, urllib.error

PORT, GROUP, MAGIC = 48621, '239.87.76.67', b'WILKCAST/1 '
DEFAULT_HTTP = 48620


# --------------------------------------------------------------------------- discovery
def udp_socket(bind=True):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, 'SO_REUSEPORT'):
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            pass
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    if bind:
        s.bind(('', PORT))
        for ip in ['0.0.0.0'] + local_ipv4():   # join on every interface
            try:
                s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                             struct.pack('4s4s', socket.inet_aton(GROUP), socket.inet_aton(ip)))
            except OSError:
                pass
    return s


def local_ipv4():
    """This computer's IPv4 addresses (best effort, standard library only)."""
    addrs = {'127.0.0.1'}
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    except OSError:
        pass
    for probe in ('192.0.2.1', '10.255.255.255', '172.31.255.255', '192.168.255.255'):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((probe, 9))
            addrs.add(s.getsockname()[0])
        except OSError:
            pass
        finally:
            s.close()
    addrs.discard('0.0.0.0')
    return sorted(addrs)


def send_beacon(sock, text):
    """Multicast out of every interface (macOS only uses one otherwise), plus
    subnet broadcasts for networks that drop multicast."""
    data = MAGIC + text.encode()
    for ip in local_ipv4():
        try:
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(ip))
            sock.sendto(data, (GROUP, PORT))
        except OSError:
            pass
        if not ip.startswith('127.'):
            for bc in (ip.rsplit('.', 1)[0] + '.255', '255.255.255.255'):
                try:
                    sock.sendto(data, (bc, PORT))
                except OSError:
                    pass


def discover(seconds=3.0):
    """Returns {id: device} for every receiver heard within `seconds`."""
    s = udp_socket()
    send_beacon(s, '?')
    found, end = {}, time.time() + seconds
    while time.time() < end:
        s.settimeout(max(0.05, end - time.time()))
        try:
            data, (ip, _) = s.recvfrom(1024)
        except socket.timeout:
            break
        if not data.startswith(MAGIC) or data[len(MAGIC):] == b'?':
            continue
        try:
            d = json.loads(data[len(MAGIC):])
        except ValueError:
            continue
        if not isinstance(d, dict) or len(str(d.get('id', ''))) < 8 or d.get('bye'):
            continue
        d['ip'] = ip
        d.setdefault('base', '/cast/v1/')
        d.setdefault('kind', 'wlc')
        found[d['id']] = d
    s.close()
    return found


# --------------------------------------------------------------------------- client
def call(dev, op, body=None, pin=None, timeout=10):
    url = 'http://%s:%d%s%s' % (dev['ip'], dev['port'], dev.get('base', '/cast/v1/'), op)
    h = {'X-WilkCast-From': 'wilkcast.py'}
    if pin:
        h['X-WilkCast-Pin'] = pin
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        h['Content-Type'] = 'application/json'
    req = urllib.request.Request(url, data=data, headers=h, method='POST' if data else 'GET')
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b'{}')
        except ValueError:
            return e.code, {}


def resolve(target, timeout=5.0):
    """A name / id prefix from discovery, or HOST[:PORT] (asked directly)."""
    host, _, port = target.rpartition(':') if target.count(':') == 1 else (target, '', '')
    if host == "localhost" or all(c.isdigit() or c == "." for c in (host or target)) or "." in target:
        dev = {'ip': host or target, 'port': int(port or DEFAULT_HTTP), 'base': '/cast/v1/'}
        code, info = call(dev, 'info')
        if code != 200 or not info.get('wilkcast'):
            sys.exit('no WilkCast receiver at %s' % target)
        dev.update(id=info.get('id'), name=info.get('name'), auth=info.get('auth', 'pin'),
                   kind=info.get('kind', 'wlc'))
        return dev
    devs = list(discover(timeout).values())
    t = target.lower()
    hits = [d for d in devs if d['name'].lower() == t] or \
           [d for d in devs if d['name'].lower().startswith(t) or d['id'].startswith(target)]
    if len(hits) == 1:
        return hits[0]
    if not hits:
        sys.exit('no device called "%s" answered (try: wilkcast.py discover)' % target)
    sys.exit('"%s" matches several devices: %s' % (target, ', '.join(d['name'] for d in hits)))


def my_ip_towards(ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ip, 9))
        return s.getsockname()[0]
    finally:
        s.close()


# --------------------------------------------------------------------------- file server
class Quiet(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve_files(paths, host_ip):
    """Serves the given files at unguessable URLs; returns (server, urls)."""
    token = secrets.token_hex(16)
    files = [os.path.abspath(p) for p in paths]

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_HEAD(self):
            self.do_GET(head=True)

        def do_GET(self, head=False):
            parts = self.path.split('/')
            # /<token>/<index>/<name>
            if len(parts) < 4 or parts[1] != token or not parts[2].isdigit() or int(parts[2]) >= len(files):
                self.send_error(404)
                return
            path = files[int(parts[2])]
            size = os.path.getsize(path)
            start, end, code = 0, size - 1, 200
            rng = self.headers.get('Range', '')
            if rng.startswith('bytes='):
                a, _, b = rng[6:].split(',')[0].partition('-')
                try:
                    if a == '':
                        start = max(0, size - int(b))
                    else:
                        start = int(a)
                        end = min(size - 1, int(b)) if b else size - 1
                except ValueError:
                    start, end = 0, size - 1
                if start >= size or start > end:
                    self.send_response(416)
                    self.send_header('Content-Range', 'bytes */%d' % size)
                    self.end_headers()
                    return
                code = 206
            self.send_response(code)
            self.send_header('Content-Type', mimetypes.guess_type(path)[0] or 'application/octet-stream')
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Content-Length', str(end - start + 1))
            if code == 206:
                self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
            self.end_headers()
            if head:
                return
            with open(path, 'rb') as f:
                f.seek(start)
                left = end - start + 1
                try:
                    while left > 0:
                        chunk = f.read(min(1 << 16, left))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        left -= len(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    pass

    srv = Quiet(('0.0.0.0', 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]
    urls = ['http://%s:%d/%s/%d/%s' % (host_ip, port, token, i, urllib.parse.quote(os.path.basename(p)))
            for i, p in enumerate(files)]
    return srv, urls


# --------------------------------------------------------------------------- receiver
class Player:
    """Drives an external player. With mpv, uses its JSON IPC for everything."""

    def __init__(self, kind):
        self.kind, self.proc, self.ipc = kind, None, None
        self.url, self.title, self.started, self.offset, self.paused_at = None, '', 0., 0., None
        self.queue, self.idx, self.volume = [], -1, 1.0

    def _mpv(self, *cmd):
        if not self.ipc or not self.proc or self.proc.poll() is not None:
            return None
        try:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(1)
            s.connect(self.ipc)
            s.sendall(json.dumps({'command': list(cmd)}).encode() + b'\n')
            buf = b''
            while b'\n' not in buf:
                buf += s.recv(4096)
            s.close()
            for line in buf.split(b'\n'):
                j = json.loads(line or b'{}')
                if 'error' in j:
                    return j.get('data')
        except (OSError, ValueError):
            return None

    def _kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    def load(self, url, title, position, queue):
        self.queue = [{'url': url, 'title': title}] + [q for q in queue if isinstance(q, dict) and q.get('url')]
        self._start(0, position)

    def _start(self, idx, position=0.):
        self._kill()
        self.idx = idx
        it = self.queue[idx]
        self.url, self.title = it['url'], it.get('title') or it['url'].rsplit('/', 1)[-1]
        self.started, self.offset, self.paused_at = time.time(), position, None
        print('▶ %s%s' % (self.title, ' from %.0f s' % position if position else ''), flush=True)
        if self.kind == 'mpv':
            self.ipc = '/tmp/wilkcast-mpv-%d.sock' % os.getpid()
            self.proc = subprocess.Popen(['mpv', '--force-window=yes', '--fs', '--really-quiet',
                                          '--input-ipc-server=' + self.ipc, '--start=%.2f' % position,
                                          '--volume=%d' % round(self.volume * 100), self.url])
        elif self.kind == 'ffplay':
            self.proc = subprocess.Popen(['ffplay', '-autoexit', '-loglevel', 'quiet', '-fs',
                                          '-ss', '%.2f' % position, self.url])
        elif self.kind == 'vlc':
            self.proc = subprocess.Popen(['vlc', '--play-and-exit', '--start-time=%.2f' % position, self.url],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def tick(self):
        """Moves to the next queued item when the player exits by itself."""
        if self.url and self.kind != 'none' and self.proc and self.proc.poll() is not None:
            if self.idx + 1 < len(self.queue):
                self._start(self.idx + 1)
            else:
                self.url = None

    def control(self, cmd, value):
        if not self.url:
            return
        mpv = self.kind == 'mpv'
        if cmd in ('pause', 'resume', 'play', 'toggle'):
            pause = {'pause': True, 'resume': False, 'play': False}.get(cmd, self.paused_at is None)
            if mpv:
                self._mpv('set_property', 'pause', pause)
            if pause and self.paused_at is None:
                self.paused_at = self.time()
            elif not pause and self.paused_at is not None:
                self.started, self.offset, self.paused_at = time.time(), self.paused_at, None
        elif cmd == 'stop':
            self._kill()
            self.url = None
        elif cmd in ('seek', 'seekrel'):
            t = max(0., value if cmd == 'seek' else self.time() + value)
            if mpv:
                self._mpv('set_property', 'time-pos', t)
            elif self.kind in ('ffplay', 'vlc'):
                self._start(self.idx, t)
            self.started, self.offset = time.time(), t
            if self.paused_at is not None:
                self.paused_at = t
        elif cmd == 'volume':
            self.volume = max(0., min(2., value))
            if mpv:
                self._mpv('set_property', 'volume', round(self.volume * 100))
        elif cmd == 'next' and self.idx + 1 < len(self.queue):
            self._start(self.idx + 1)
        elif cmd == 'prev':
            self._start(max(0, self.idx - 1) if self.time() < 3 else self.idx)

    def time(self):
        if self.kind == 'mpv':
            t = self._mpv('get_property', 'time-pos')
            if isinstance(t, (int, float)):
                return float(t)
        if self.paused_at is not None:
            return self.paused_at
        return self.offset + (time.time() - self.started)

    def status(self):
        if not self.url:
            return {'state': 'stopped', 'time': 0, 'length': 0, 'volume': self.volume, 'title': ''}
        length = self._mpv('get_property', 'duration') if self.kind == 'mpv' else None
        return {'state': 'paused' if self.paused_at is not None else 'playing',
                'time': round(self.time(), 2), 'length': float(length or 0), 'volume': self.volume,
                'title': self.title, 'index': self.idx, 'count': len(self.queue)}


def receive(args):
    dev_id = args.id or secrets.token_hex(16)
    auth = 'open' if args.open else 'pin'
    pin = args.pin or os.environ.get('WILKCAST_PIN') or ('%06d' % secrets.randbelow(10 ** 6))
    player = Player(args.player)
    lock = threading.Lock()
    fails = {'n': 0, 'until': 0.}
    allowed = {'play', 'resume', 'pause', 'toggle', 'stop', 'seek', 'seekrel', 'volume', 'next', 'prev'}

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def reply(self, code, obj):
            b = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def gate(self):
            if auth == 'open':
                return True
            now = time.time()
            with lock:
                if now < fails['until']:
                    return False
                if secrets.compare_digest(self.headers.get('X-WilkCast-Pin', ''), pin):
                    fails['n'] = 0
                    return True
                fails['n'] += 1
                if fails['n'] >= 5:
                    fails['n'], fails['until'] = 0, now + 60
                return False

        def do_GET(self):
            if self.path == '/cast/v1/info':
                return self.reply(200, {'wilkcast': 1, 'app': 'wilkcast.py', 'version': '1', 'kind': 'script',
                                        'id': dev_id, 'name': args.name, 'auth': auth})
            if self.path != '/cast/v1/status':
                return self.reply(404, {'error': 'no such cast endpoint'})
            if not self.gate():
                return self.reply(401, {'error': 'PIN required'})
            with lock:
                st = player.status()
            st['name'] = args.name
            self.reply(200, st)

        def do_POST(self):
            if self.path not in ('/cast/v1/play', '/cast/v1/control'):
                return self.reply(404, {'error': 'no such cast endpoint'})
            if not self.gate():
                return self.reply(401, {'error': 'PIN required'})
            try:
                n = int(self.headers.get('Content-Length', '0'))
                body = json.loads(self.rfile.read(min(n, 1 << 20)) or b'{}')
                assert isinstance(body, dict)
            except (ValueError, AssertionError):
                return self.reply(400, {'error': 'expected a JSON object'})
            with lock:
                if self.path.endswith('/play'):
                    url = str(body.get('url', ''))
                    if not url.lower().startswith(('http://', 'https://')):
                        return self.reply(400, {'error': 'url must be http or https'})
                    queue = body.get('queue') if isinstance(body.get('queue'), list) else []
                    queue = [q for q in queue[:500] if isinstance(q, dict)
                             and str(q.get('url', '')).lower().startswith(('http://', 'https://'))]
                    player.load(url, str(body.get('title', '')), float(body.get('position') or 0), queue)
                    return self.reply(200, {'ok': True})
                cmd = body.get('cmd')
                if cmd not in allowed:
                    return self.reply(400, {'error': 'command not allowed for casting'})
                try:
                    value = float(body.get('value') or 0)
                except (TypeError, ValueError):
                    value = 0.
                print('  %s %s' % (cmd, '' if cmd in ('pause', 'resume', 'toggle', 'stop', 'next', 'prev', 'play') else value),
                      flush=True)
                player.control(cmd, value)
                st = player.status()
            st['name'] = args.name
            self.reply(200, st)

    srv = Quiet(('0.0.0.0', args.port), H)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    sock = udp_socket()
    beacon = json.dumps({'id': dev_id, 'name': args.name, 'port': port, 'auth': auth, 'kind': 'script'})
    print('WilkCast receiver "%s" on port %d%s. Ctrl-C to quit.' %
          (args.name, port, '' if auth == 'open' else ', PIN ' + pin), flush=True)
    nxt = 0.

    def on_term(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_term)
    try:
        while True:
            if time.time() >= nxt:
                send_beacon(sock, beacon)
                nxt = time.time() + 4
            sock.settimeout(0.5)
            try:
                data, _ = sock.recvfrom(1024)
                if data == MAGIC + b'?':
                    nxt = 0.
            except socket.timeout:
                pass
            with lock:
                player.tick()
    except KeyboardInterrupt:
        pass
    finally:
        send_beacon(sock, json.dumps({'id': dev_id, 'name': args.name, 'port': port, 'auth': auth, 'bye': True}))
        with lock:
            player.control('stop', 0)
        srv.shutdown()


# --------------------------------------------------------------------------- commands
def fmt(s):
    s = int(s or 0)
    return '%d:%02d' % (s // 60, s % 60)


def main():
    ap = argparse.ArgumentParser(description='WilkCast 1 sender and receiver.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('discover', help='list receivers on this network')
    p.add_argument('-t', type=float, default=5.0, help='seconds to listen (default 5)')
    p.add_argument('--json', action='store_true')
    for name in ('info', 'status'):
        p = sub.add_parser(name)
        p.add_argument('target')
        p.add_argument('--pin')
    p = sub.add_parser('cast', help='play files or URLs on a receiver')
    p.add_argument('target')
    p.add_argument('items', nargs='+', metavar='FILE_OR_URL')
    p.add_argument('--pin')
    p.add_argument('--from', dest='start', type=float, default=0.)
    p.add_argument('--wait', action='store_true', help='serve until the receiver stops, then exit')
    p = sub.add_parser('ctl', help='control what a receiver is playing')
    p.add_argument('target')
    p.add_argument('action')
    p.add_argument('value', nargs='?', type=float, default=0.)
    p.add_argument('--pin')
    p = sub.add_parser('receive', help='be a receiver')
    p.add_argument('--name', default=socket.gethostname())
    p.add_argument('--pin')
    p.add_argument('--open', action='store_true', help='accept casts without a PIN')
    p.add_argument('--port', type=int, default=0)
    p.add_argument('--id', help=argparse.SUPPRESS)
    p.add_argument('--player', default=next((x for x in ('mpv', 'ffplay', 'vlc') if shutil.which(x)), 'none'),
                   choices=('mpv', 'ffplay', 'vlc', 'none'))
    a = ap.parse_args()
    pin = getattr(a, 'pin', None) or os.environ.get('WILKCAST_PIN')

    if a.cmd == 'discover':
        devs = discover(a.t)
        if a.json:
            print(json.dumps(list(devs.values()), indent=1))
        for d in sorted(devs.values(), key=lambda d: d['name'].lower()):
            if not a.json:
                print('%-28s %-8s %s:%d%s  %s' % (d['name'][:28], d.get('kind', 'wlc'), d['ip'], d['port'],
                                                 '' if d['base'] == '/cast/v1/' else d['base'], d['auth']))
        if not devs and not a.json:
            print('nothing heard in %.0f s' % a.t)
        return
    if a.cmd == 'receive':
        return receive(a)

    dev = resolve(a.target)
    if a.cmd == 'info':
        print(json.dumps(call(dev, 'info')[1], indent=1))
    elif a.cmd == 'status':
        code, st = call(dev, 'status', pin=pin)
        if code != 200:
            sys.exit('%d: %s' % (code, st.get('error', '')))
        print('%s: %s %s %s / %s' % (dev.get('name'), st.get('state'), st.get('title', ''),
                                     fmt(st.get('time')), fmt(st.get('length'))))
    elif a.cmd == 'ctl':
        code, st = call(dev, 'control', {'cmd': a.action, 'value': a.value}, pin)
        if code != 200:
            sys.exit('%d: %s' % (code, st.get('error', '')))
        print('%s: %s' % (dev.get('name'), st.get('state')))
    elif a.cmd == 'cast':
        files = [x for x in a.items if not x.lower().startswith(('http://', 'https://'))]
        for f in files:
            if not os.path.isfile(f):
                sys.exit('not a file: ' + f)
        srv, urls = (None, [])
        if files:
            srv, urls = serve_files(files, my_ip_towards(dev['ip']))
        it = iter(urls)
        items = [{'url': x if x.lower().startswith(('http://', 'https://')) else next(it),
                  'title': os.path.basename(urllib.parse.unquote(x.split('?')[0]))} for x in a.items]
        body = {'url': items[0]['url'], 'title': items[0]['title'], 'position': a.start, 'from': socket.gethostname()}
        if len(items) > 1:
            body['queue'] = items[1:]
        code, r = call(dev, 'play', body, pin)
        if code != 200:
            sys.exit('%d: %s' % (code, r.get('error', 'refused')))
        print('Casting %d item%s to %s' % (len(items), '' if len(items) == 1 else 's', dev.get('name')))
        if not srv:
            return
        print('Serving the file%s from this computer. Ctrl-C to stop.' % ('' if len(files) == 1 else 's'))
        seen_play = False
        try:
            while True:
                time.sleep(2)
                code, st = call(dev, 'status', pin=pin)
                if code == 200:
                    state = st.get('state')
                    seen_play |= state in ('playing', 'paused')
                    print('\r  %-8s %-40.40s %s / %s ' % (state, st.get('title', ''), fmt(st.get('time')),
                                                         fmt(st.get('length'))), end='', flush=True)
                    if a.wait and seen_play and state in ('stopped', 'error'):
                        print()
                        break
        except KeyboardInterrupt:
            print()
        srv.shutdown()


if __name__ == '__main__':
    main()
