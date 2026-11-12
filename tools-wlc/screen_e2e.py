#!/usr/bin/env python3
"""Drives the WilkCast browser screen (/screen) in real Chrome against a
running WLC: sign in with the PIN, receive a cast, play, pause, seek, report
an unplayable file and move on through the queue.

    python3 tools-wlc/screen_e2e.py http://<lan-ip>:48620 /folder/with/media

The folder must be in the WLC's Media Center and hold clip.mp4 (H.264/AAC)
and bad.mkv. WLC must be started separately (see e2e.py for how).
"""
import json, os, sys, time, urllib.request
from playwright.sync_api import sync_playwright

BASE, MEDIA = sys.argv[1].rstrip('/'), sys.argv[2]
LOCAL = 'http://127.0.0.1:' + BASE.rsplit(':', 1)[1]
PASS = FAIL = 0


def check(name, cond, detail=''):
    global PASS, FAIL
    PASS, FAIL = PASS + bool(cond), FAIL + (not cond)
    print(('  ok   ' if cond else '  FAIL ') + name + ('' if cond or detail == '' else '  -> ' + str(detail)), flush=True)


def api(path, body=None):
    req = urllib.request.Request(LOCAL + path, data=json.dumps(body).encode() if body is not None else None,
                                 headers={'Content-Type': 'application/json'}, method='POST' if body is not None else 'GET')
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'{}')


def wait(fn, secs=15, step=0.25):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return None


info = api('/api/info')[1]
with sync_playwright() as pw:
    br = pw.chromium.launch(executable_path='/usr/bin/google-chrome', args=['--autoplay-policy=no-user-gesture-required'])
    tv = br.new_page(viewport={'width': 1280, 'height': 720})
    errors = []
    tv.on('pageerror', lambda e: errors.append(str(e)))
    tv.goto(BASE + '/screen')
    check('screen page loads with the setup form', tv.is_visible('#in-pin') and tv.is_visible('#go'))
    tv.fill('#in-name', 'Browser Test TV')
    tv.fill('#in-pin', '000000' if info['pin'] != '000000' else '111111')
    tv.click('#go')
    check('wrong PIN is reported on the screen', bool(wait(lambda: 'Wrong PIN' in tv.inner_text('#err'), 5)))
    tv.fill('#in-pin', info['pin'])
    tv.click('#go')
    check('right PIN: screen shows its name and waits for casts',
          bool(wait(lambda: tv.is_visible('#idle') and tv.inner_text('#idle-name').lower() == 'browser test tv', 8)))
    peer = wait(lambda: [p for p in api('/api/peers')[1]['peers'] if p['name'] == 'Browser Test TV'], 8)
    check('WLC lists the screen as a cast target', bool(peer))
    pid = peer[0]['id'] if peer else ''
    tv.screenshot(path='/tmp/wlc-screen-idle.png')

    clip, bad = os.path.join(MEDIA, 'clip.mp4'), os.path.join(MEDIA, 'bad.mkv')
    code, r = api('/api/cast/send', {'peer': pid, 'paths': [clip, bad, clip]})
    check('cast a queue of 3 (mp4, mkv, mp4) to the screen', code == 200, r)
    t = wait(lambda: tv.evaluate('document.getElementById("v").currentTime') > 2, 15)
    check('Chrome actually plays the cast video', bool(t), tv.evaluate('({t: v.currentTime, e: v.error && v.error.code, src: v.currentSrc})'))
    tv.screenshot(path='/tmp/wlc-screen-playing.png')
    st = wait(lambda: (lambda s: s if s.get('state') == 'playing' and s.get('time', 0) > 1 else None)(
        api('/api/cast/status?peer=' + pid)[1]), 6)
    check('WLC sees the screen playing, with time and length', bool(st) and st.get('length', 0) > 20, st)
    api('/api/cast/control', {'peer': pid, 'cmd': 'pause'})
    check('pause from WLC pauses the video', bool(wait(lambda: tv.evaluate('v.paused'), 5)))
    api('/api/cast/control', {'peer': pid, 'cmd': 'seek', 'value': 20})
    check('seek from WLC moves the video (needs byte ranges)', bool(wait(lambda: abs(tv.evaluate('v.currentTime') - 20) < 1, 6)),
          tv.evaluate('v.currentTime'))
    api('/api/cast/control', {'peer': pid, 'cmd': 'volume', 'value': 0.5})
    check('volume from WLC', bool(wait(lambda: abs(tv.evaluate('v.volume') - 0.5) < 0.01, 5)))
    api('/api/cast/control', {'peer': pid, 'cmd': 'next'})
    err = wait(lambda: (lambda s: s if s.get('state') == 'error' or 'MKV' in s.get('error', '') else None)(
        api('/api/cast/status?peer=' + pid)[1]), 10, 0.2)
    check('unplayable MKV is reported back to WLC', bool(err), api('/api/cast/status?peer=' + pid)[1])
    third = wait(lambda: tv.evaluate('v.currentSrc.endsWith("clip.mp4") && v.currentTime > 1 && !v.paused'), 12)
    check('screen skips the bad file and goes on to the next one', bool(third))
    api('/api/cast/control', {'peer': pid, 'cmd': 'stop'})
    check('stop returns the screen to its waiting page', bool(wait(lambda: tv.is_visible('#idle'), 5)))

    # the Control Center's cast dialog offers the screen
    cc = br.new_page(viewport={'width': 1280, 'height': 800})
    cc.goto(LOCAL + '/')
    cc.wait_for_selector('#bt-cast')
    cc.click('#bt-cast')
    cc.wait_for_selector('.modal')
    txt = cc.inner_text('.modal')
    check('Control Center cast dialog offers the browser screen', 'Browser Test TV' in txt and 'browser screen' in txt, txt)
    cc.screenshot(path='/tmp/wlc-cast-dialog.png')

    tv.reload()
    check('after a reload the screen comes back without the PIN', bool(wait(lambda: tv.is_visible('#idle'), 8)))
    check('no script errors on the screen page', not errors, errors)
    br.close()

print('\n%d passed, %d failed' % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
