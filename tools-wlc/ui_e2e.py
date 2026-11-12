#!/usr/bin/env python3
"""Drives the real Control Center page in Chrome against a running WLC.

    python3 tools-wlc/ui_e2e.py http://127.0.0.1:48620 /folder/with/media [screenshot-dir] [lan-url pin]
"""
import json, os, re, sys, time, urllib.request
from playwright.sync_api import sync_playwright

BASE, MEDIA = sys.argv[1], os.path.realpath(sys.argv[2])
SHOTS = sys.argv[3] if len(sys.argv) > 3 else None
PASS = FAIL = 0
errors = []

def check(name, cond, detail=''):
    global PASS, FAIL
    if cond: PASS += 1; print('  ok   ' + name)
    else: FAIL += 1; print('  FAIL ' + name + ('  -> ' + str(detail) if detail != '' else ''))

def api(path, body=None):
    r = urllib.request.Request(BASE + path, data=json.dumps(body).encode() if body is not None else None,
                               headers={'Content-Type': 'application/json'} if body is not None else {})
    return json.loads(urllib.request.urlopen(r, timeout=15).read())

def shot(page, name):
    if SHOTS: page.screenshot(path=os.path.join(SHOTS, name + '.png'))

def walk(page, path):
    """walk the folder/file picker to an absolute path"""
    page.wait_for_selector('.modal td.name')
    page.locator('.modal td.name', has_text=re.compile(r'^/$')).click()
    for part in [p for p in path.split('/') if p]:
        page.locator('.modal td.name', has_text=re.compile('^' + re.escape(part) + '$')).click()
        page.wait_for_timeout(120)

def pick(page, path, confirm):
    walk(page, path)
    page.get_by_role('button', name=confirm).click()

with sync_playwright() as p:
    b = p.chromium.launch(executable_path='/usr/bin/google-chrome', args=['--no-sandbox'])
    page = b.new_context(bypass_csp=True, viewport={'width': 1280, 'height': 860}).new_page()
    page.on('console', lambda m: errors.append(m.text) if m.type == 'error' else None)
    page.on('pageerror', lambda e: errors.append(str(e)))
    page.goto(BASE + '/')
    page.wait_for_selector('nav button')
    check('page loads with 6 tabs for the local user', page.locator('nav button').count() == 6)

    print('Media Center')
    page.get_by_role('button', name='Media Center').click()
    page.get_by_text('No folders yet').wait_for()
    shot(page, '01-library-empty')
    page.get_by_role('button', name='+ Add folder').click()
    pick(page, MEDIA, 'Use this folder')
    page.wait_for_selector('.tile')
    check('folder added through the picker; folder tiles appear', page.locator('.tile').count() >= 2, page.locator('.tile').count())
    try:
        page.wait_for_function("[...document.querySelectorAll('.tile .art')].some(a=>a.style.backgroundImage.includes('/api/art'))", timeout=12000); art = True
    except Exception: art = False
    check('embedded cover art shows on the album tile', art)
    shot(page, '02-library')
    page.locator('.tile', has_text='Album').click()
    page.wait_for_selector('tr.click')
    check('album folder lists 3 tracks', page.locator('tr.click').count() == 3)
    page.get_by_role('button', name='Play all').click()
    page.wait_for_function("document.querySelector('#np-title').textContent.includes('Song 1')", timeout=8000)
    check('Play all starts Song 1 (player bar updates)', True)
    page.locator('input[type=search]').fill('clip')
    page.wait_for_selector('tr.click')
    check('search finds the video', page.locator('tr.click').count() == 1 and 'clip.mp4' in page.locator('tr.click').inner_text())
    page.locator('input[type=search]').fill('')

    print('Now Playing')
    page.get_by_role('button', name='Now Playing').click()
    page.wait_for_selector('#queue tr')
    check('queue shows 3 tracks with one current', page.locator('#queue tr').count() == 3 and page.locator('#queue tr.cur').count() == 1)
    page.wait_for_timeout(800); shot(page, '03-now-playing')
    page.locator('#queue tr').nth(2).click()
    page.wait_for_function("document.querySelector('#np-title').textContent.includes('Song 3')", timeout=8000)
    check('clicking a queue row jumps to it', True)
    page.locator('#bt-play').click()
    page.wait_for_timeout(1300)
    check('pause button pauses', api('/api/status')['state'] == 'paused', api('/api/status')['state'])

    print('Power EQ')
    page.get_by_role('button', name='Power EQ').click()
    page.wait_for_selector('.eq-band')
    check('10 bands by default', page.locator('.eq-band').count() == 10)
    page.locator('select', has_text='Presets').select_option('Rock')
    page.wait_for_timeout(500)
    eq = api('/api/eq')
    check('preset applies and switches the EQ on', eq['on'] and eq['graphic'].split()[0] == '5.0', eq)
    page.get_by_role('button', name='31 bands').click()
    page.wait_for_timeout(500)
    eq = api('/api/eq')
    check('31-band layout keeps the curve shape', page.locator('.eq-band').count() == 31 and len(eq['graphic'].split()) == 31 and float(eq['graphic'].split()[0]) == 5.0, eq['graphic'])
    page.get_by_role('button', name='+ Add parametric band').click()
    page.locator('.rule select').first.select_option('highpass')
    page.locator('.rule input[type=number]').first.fill('80'); page.locator('.rule input[type=number]').first.press('Tab')
    page.wait_for_timeout(500)
    check('parametric band is sent to the engine', api('/api/eq')['parametric'].startswith('highpass,80,'), api('/api/eq')['parametric'])
    page.wait_for_timeout(300); shot(page, '04-eq')
    px = page.evaluate("(()=>{const c=document.querySelector('canvas');const d=c.getContext('2d').getImageData(0,0,c.width,c.height).data;let n=0;for(let i=0;i<d.length;i+=4)if(d[i]>200&&d[i+1]>80&&d[i+1]<140&&d[i+2]<40)n++;return n})()")
    check('response curve is drawn', px > 300, px)
    page.get_by_role('button', name='Reset', exact=True).click(); page.wait_for_timeout(400)
    check('reset flattens everything', set(api('/api/eq')['graphic'].split()) == {'0.0'} and api('/api/eq')['parametric'] == '')

    print('Bulk Renamer')
    page.get_by_role('button', name='Bulk Renamer').click()
    page.get_by_role('button', name='Open folder…').click()
    pick(page, MEDIA + '/Music/Album', 'Use this folder')
    page.wait_for_selector('td.name')
    before = sorted(os.listdir(MEDIA + '/Music/Album'))
    add = page.locator('select', has_text='Add a rule')
    add.select_option('stripnum'); add.select_option('clean'); add.select_option('kase'); add.select_option('number')
    page.wait_for_timeout(300)
    rows = page.locator('td.chg').all_inner_texts()
    check('live preview: strip numbers + clean + Title Case + numbering', rows == ['01 - Some Track-1.mp3', '02 - Some Track-2.mp3', '03 - Some Track-3.mp3'], rows)
    add.select_option('tags')
    page.locator('.rule', has_text='Name from tags').locator('input[type=text]').fill('{track:2} {artist} - {title}')
    page.wait_for_function("[...document.querySelectorAll('td.chg')].some(t=>t.textContent.includes('Test Band - Song 2'))", timeout=15000)
    rows = page.locator('td.chg').all_inner_texts()
    check('names from tags', rows == ['01 Test Band - Song 1.mp3', '02 Test Band - Song 2.mp3', '03 Test Band - Song 3.mp3'], rows)
    shot(page, '05-renamer')
    page.get_by_role('button', name='Rename 3 files').click()
    page.wait_for_function("document.querySelectorAll('.rule b').length && document.querySelector('.toast')", timeout=8000)
    page.wait_for_timeout(500)
    after = sorted(os.listdir(MEDIA + '/Music/Album'))
    check('files really renamed on disk', after == ['01 Test Band - Song 1.mp3', '02 Test Band - Song 2.mp3', '03 Test Band - Song 3.mp3'], after)
    add = page.locator('select', has_text='Add a rule')
    add.select_option('replace')
    r = page.locator('.rule', has_text='Replace text')
    r.locator('input[type=text]').nth(0).fill('^.*$'); r.locator('input[type=text]').nth(1).fill('Same'); r.get_by_text('regex').click()
    page.wait_for_timeout(300)
    check('clashing names are flagged and Rename is disabled', page.locator('td.clash').count() == 3 and page.get_by_role('button', name='Rename 3 files').is_disabled())
    page.get_by_role('button', name='Clear rules').click()
    page.get_by_role('button', name='Undo').first.click()
    page.wait_for_timeout(1200)
    check('Undo restores the original names', sorted(os.listdir(MEDIA + '/Music/Album')) == before, os.listdir(MEDIA + '/Music/Album'))

    print('Converter')
    page.get_by_role('button', name='Converter', exact=True).click()
    page.get_by_role('button', name='+ Add files').click()
    walk(page, MEDIA + '/Music/Album')
    page.get_by_role('button', name='Select all here').click()
    page.get_by_role('button', name='Add 3 files').click()
    page.locator('select', has_text='MP4').first.select_option('ogg')
    shot(page, '06-converter')
    page.get_by_role('button', name='Start converting').click()
    out = MEDIA + '/Music/Album/Converted'
    for _ in range(80):
        if os.path.isdir(out) and len([n for n in os.listdir(out) if n.endswith('.ogg')]) == 3: break
        time.sleep(0.5)
    check('3 Ogg files written to the Converted folder', os.path.isdir(out) and sorted(n for n in os.listdir(out)) == [n[:-4] + '.ogg' for n in before], os.path.isdir(out) and os.listdir(out))
    page.wait_for_timeout(1500)
    check('job list shows them done', page.locator('.ok', has_text='done').count() == 3, page.locator('main').inner_text()[-300:])
    shot(page, '07-converter-done')

    print('Network & Cast')
    page.get_by_role('button', name='Network & Cast').click()
    page.wait_for_selector('.big-pin')
    pin = page.locator('.big-pin').inner_text()
    check('PIN and phone address are shown', len(pin) == 6 and 'http://' in page.locator('main').inner_text())
    page.wait_for_timeout(1200); shot(page, '08-network')
    if len(sys.argv) > 4:
        lan = sys.argv[4]
        print('From another device (' + lan + ')')
        ctx = b.new_context(bypass_csp=True, viewport={'width': 390, 'height': 800}, device_scale_factor=2)
        ph = ctx.new_page()
        ph.on('pageerror', lambda e: errors.append('phone: ' + str(e)))
        ph.goto(lan + '/')
        ph.wait_for_selector('input[type=password]')
        shot(ph, '09-phone-login')
        ph.locator('input[type=password]').fill('999999' if pin != '999999' else '111111'); ph.get_by_role('button', name='Connect').click()
        ph.wait_for_selector('.err:not(:empty)')
        check('wrong PIN shows an error', 'PIN' in ph.locator('.err').inner_text())
        ph.locator('input[type=password]').fill(pin); ph.get_by_role('button', name='Connect').click()
        ph.wait_for_selector('nav button')
        labels = ph.locator('nav button').all_inner_texts()
        check('remote device gets 4 tabs, no Converter or Renamer', labels == ['Now Playing', 'Media Center', 'Power EQ', 'Network & Cast'], labels)
        ph.get_by_role('button', name='Media Center').click()
        ph.wait_for_selector('.tile')
        check('remote has no Add folder button', ph.get_by_role('button', name='+ Add folder').count() == 0)
        ph.locator('.tile', has_text='Video').click()
        ph.wait_for_selector('tr.click')
        ph.get_by_role('button', name='Here', exact=True).click()
        ph.wait_for_selector('#local-player video')
        ok = ph.wait_for_function("(()=>{const v=document.querySelector('#local-player video');return v&&v.readyState>=2&&v.duration>5})()", timeout=15000)
        check('video plays in the remote browser straight from WLC', bool(ok))
        ph.evaluate("document.querySelector('#local-player video').currentTime=20"); 
        ok = ph.wait_for_function("(()=>{const v=document.querySelector('#local-player video');return v.currentTime>=19.5&&v.readyState>=2})()", timeout=15000)
        check('seeking works in the remote browser (byte ranges)', bool(ok))
        ph.wait_for_timeout(500); shot(ph, '10-phone-playing')
        ph.get_by_role('button', name='Network & Cast').click(); ph.wait_for_timeout(600)
        check('remote never sees the PIN', pin not in ph.locator('body').inner_text())
        shot(ph, '11-phone-network')
    check('no JavaScript errors', not errors, errors[:5])
    b.close()
print('\n%d passed, %d failed' % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
