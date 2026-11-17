#!/usr/bin/env python3
"""Starts a WLC, builds the WLC TV core with javac, and runs CoreTest against it.

    python3 tv/test/run_core_test.py /path/to/vlc      (not as root: VLC refuses)
"""
import glob, json, os, socket, subprocess, sys, tempfile, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
VLC = sys.argv[1]
WORK = tempfile.mkdtemp(prefix='wlctv-core-')


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(('192.0.2.1', 9))
    return s.getsockname()[0]


def ffmpeg(*a):
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y'] + list(a), check=True)


def api(method, path, body=None):
    req = urllib.request.Request('http://127.0.0.1:48720' + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read() or b'{}')


media = os.path.join(WORK, 'media')
os.makedirs(media + '/Music/Album')
os.makedirs(media + '/Video')
for i in (1, 2, 3):
    ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=%d:duration=6' % (300 * i), '-c:a', 'libmp3lame',
           '%s/Music/Album/0%d_track.mp3' % (media, i))
ffmpeg('-f', 'lavfi', '-i', 'testsrc=duration=30:size=320x240:rate=25', '-f', 'lavfi', '-i',
       'sine=frequency=440:duration=30', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
       '-shortest', media + '/Video/clip.mp4')
ffmpeg('-f', 'lavfi', '-i', 'testsrc=size=64x64', '-frames:v', '1', media + '/Video/poster.png')

home = os.path.join(WORK, 'wlc')
os.makedirs(home)
env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + '/.config', XDG_DATA_HOME=home + '/.local/share')
log = open(os.path.join(home, 'log.txt'), 'w')
wlc = subprocess.Popen([VLC, '-I', 'dummy', '--no-video', '--aout', 'adummy', '-v', '--wlc-port', '48720'],
                       env=env, stdout=log, stderr=log)
java = None
try:
    for _ in range(60):
        try:
            info = api('GET', '/api/info')
            break
        except Exception:
            time.sleep(0.2)
    else:
        sys.exit('WLC did not start; see ' + home + '/log.txt')
    api('POST', '/api/roots', {'add': media})

    classes = os.path.join(WORK, 'classes')
    srcs = glob.glob(ROOT + '/tv/app/src/main/java/io/github/joshwilk/wlctv/core/*.java') + [HERE + '/CoreTest.java']
    subprocess.run(['javac', '-Xlint:-options', '-d', classes] + srcs, check=True)
    java = subprocess.run(['java', '-cp', classes, 'CoreTest', '48720', info['pin'], os.path.realpath(media),
                           lan_ip(), ROOT + '/tools-wlc/wilkcast.py'])
finally:
    wlc.terminate()
    try:
        wlc.wait(5)
    except subprocess.TimeoutExpired:
        wlc.kill()
sys.exit(java.returncode if java else 1)
