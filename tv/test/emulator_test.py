#!/usr/bin/env python3
"""Plays real media through WLC TV on an Android emulator, over WilkCast.

    python3 tv/test/emulator_test.py app-debug.apk

Needs adb with one emulator attached and ffmpeg. The emulator reaches this
computer at 10.0.2.2; this computer reaches the app through `adb forward`.
"""
import os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools-wlc'))
import wilkcast  # noqa: E402

APK = sys.argv[1]
PKG = 'io.github.joshwilk.wlctv'
PIN = '2468'
PORT = 48651
PASS = FAIL = 0
dev = {'ip': '127.0.0.1', 'port': PORT, 'base': '/cast/v1/'}


def check(name, ok, detail=''):
    global PASS, FAIL
    PASS, FAIL = PASS + bool(ok), FAIL + (not ok)
    print(('  ok   ' if ok else '  FAIL ') + name + ('' if ok or detail == '' else '  -> ' + str(detail)), flush=True)


def adb(*a, check_rc=True):
    r = subprocess.run(['adb'] + list(a), capture_output=True, text=True, timeout=120)
    if check_rc and r.returncode:
        print(r.stdout, r.stderr)
    return r.stdout


def status():
    try:
        code, st = wilkcast.call(dev, 'status', pin=PIN, timeout=5)
    except Exception as e:      # the app stopped answering
        return {'state': 'no answer (%s)' % type(e).__name__}
    return st if code == 200 else {'state': 'http %d' % code}


def ctl(cmd, value=0):
    return wilkcast.call(dev, 'control', {'cmd': cmd, 'value': value}, PIN)


def wait(fn, secs=20, step=0.5):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


def top_activity():
    out = adb('shell', 'dumpsys', 'activity', 'activities')
    for line in out.splitlines():
        if 'ResumedActivity' in line and PKG in line:
            return line.strip()
    return ''


def ffmpeg(*a):
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y'] + list(a), check=True)


work = tempfile.mkdtemp(prefix='wlctv-emu-')
clip, song, hevc = work + '/clip.mp4', work + '/song.mp3', work + '/movie.mkv'
ffmpeg('-f', 'lavfi', '-i', 'testsrc=duration=60:size=640x360:rate=25', '-f', 'lavfi', '-i',
       'sine=frequency=440:duration=60', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-g', '25', '-c:a', 'aac',
       '-shortest', '-movflags', '+faststart', clip)
ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=660:duration=20', '-c:a', 'libmp3lame', song)
ffmpeg('-f', 'lavfi', '-i', 'testsrc2=duration=20:size=640x360:rate=25', '-f', 'lavfi', '-i',
       'sine=frequency=330:duration=20', '-c:v', 'libx265', '-x265-params', 'log-level=error', '-pix_fmt',
       'yuv420p', '-c:a', 'ac3', '-shortest', hevc)
srv, urls = wilkcast.serve_files([clip, song, hevc], '10.0.2.2')

print('Install and start')
print(adb('install', '-r', '-g', APK).strip()[-200:])
adb('shell', "am start -n %s/.MainActivity --es test_pin %s --es test_name 'CI TV'" % (PKG, PIN))
time.sleep(4)
adb('forward', 'tcp:%d' % PORT, 'tcp:48620')
info = wait(lambda: (lambda r: r[1] if r[0] == 200 else None)(wilkcast.call(dev, 'info')), 30, 1)
check('WLC TV answers WilkCast as "CI TV" (kind tv)', bool(info) and info.get('kind') == 'tv'
      and info.get('name') == 'CI TV' and info.get('app') == 'WLC TV', info)
check('status needs the PIN', wilkcast.call(dev, 'status')[0] == 401)

print('Video')
code, r = wilkcast.call(dev, 'play', {'url': urls[0], 'title': 'clip.mp4', 'from': 'CI Mac',
                                      'queue': [{'url': urls[1], 'title': 'song.mp3'}]}, PIN)
check('cast of an MP4 with a queue is accepted', code == 200, r)
st = wait(lambda: (lambda s: s if s.get('state') == 'playing' and s.get('time', 0) > 2 else None)(status()), 40)
check('libVLC on Android really plays it (time moving)', bool(st), status())
check('length is known', bool(st) and 55 < st.get('length', 0) < 65, st)
check('the player opened full screen', 'PlayerActivity' in top_activity(), top_activity())
subprocess.run('adb exec-out screencap -p > %s/playing.png' % os.environ.get('SHOTS', '.'), shell=True)

print('Controls from both ends')
ctl('pause')
check('pause from the sender', wait(lambda: status().get('state') == 'paused', 6), status())
adb('shell', 'input', 'keyevent', 'KEYCODE_DPAD_CENTER')
check('OK on the TV remote resumes, and the sender sees it', wait(lambda: status().get('state') == 'playing', 6), status())
ctl('seek', 20)
check('seek from the sender (byte ranges)', wait(lambda: 19 <= status().get('time', 0) < 26, 10), status())
time.sleep(6)   # on-screen controls hide after 5 s; then arrows seek instead of moving focus
t0 = status().get('time', 0)
adb('shell', 'input', 'keyevent', 'KEYCODE_DPAD_RIGHT')
check('right on the TV remote jumps 30 s, and the sender sees it',
      wait(lambda: status().get('time', 0) >= t0 + 25, 10), (t0, status()))
adb('shell', 'input', 'keyevent', 'KEYCODE_MEDIA_PLAY_PAUSE')
check('play/pause key on the remote pauses', wait(lambda: status().get('state') == 'paused', 6), status())
ctl('resume')
ctl('volume', 0.5)
check('volume from the sender', abs(status().get('volume', 0) - 0.5) < 0.01, status())

print('Queue and formats')
ctl('next')
st = wait(lambda: (lambda s: s if s.get('title') == 'song.mp3' and s.get('state') == 'playing'
                   and s.get('time', 0) > 1 else None)(status()), 20)
check('next plays the queued MP3', bool(st), status())
check('the queue position is reported (2 of 2)', bool(st) and st.get('index') == 1 and st.get('count') == 2, st)
code, _ = wilkcast.call(dev, 'play', {'url': urls[2], 'title': 'movie.mkv'}, PIN)
st = wait(lambda: (lambda s: s if s.get('title') == 'movie.mkv' and s.get('state') == 'playing'
                   and s.get('time', 0) > 2 else None)(status()), 40)
check('plays HEVC + AC-3 in MKV (what browsers cannot)', bool(st), status())
subprocess.run('adb exec-out screencap -p > %s/mkv.png' % os.environ.get('SHOTS', '.'), shell=True)
time.sleep(6)
adb('shell', 'input', 'keyevent', 'KEYCODE_BACK')
check('Back on the remote stops it, and the sender sees it', wait(lambda: status().get('state') == 'stopped', 6),
      status())
check('the TV is back on the WLC TV home screen', wait(lambda: 'MainActivity' in top_activity(), 6), top_activity())

print('Settings')
check('wrong PIN is refused', wilkcast.call(dev, 'status', pin='0000')[0] == 401)
adb('shell', "am start -n %s/.MainActivity --es test_name 'Den TV'" % PKG)
check('rename shows up for senders', wait(lambda: wilkcast.call(dev, 'info')[1].get('name') == 'Den TV', 6))
check('the receiver keeps running in the background', 'pid' if adb('shell', 'pidof', PKG).strip() else '')

srv.shutdown()
print('\n%d passed, %d failed' % (PASS, FAIL))
log = adb('logcat', '-d', check_rc=False)
open(os.path.join(os.environ.get('SHOTS', '.'), 'logcat.txt'), 'w').write(log)
if FAIL:
    keep = [l for l in log.splitlines() if 'WLC-TV' in l or 'AndroidRuntime' in l or 'FATAL' in l
            or PKG in l or 'libvlc' in l.lower() or 'ActivityManager' in l]
    print('\n'.join(keep[-150:]))
sys.exit(1 if FAIL else 0)
