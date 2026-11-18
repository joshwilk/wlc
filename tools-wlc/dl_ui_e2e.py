import json, os, subprocess, sys, tempfile, threading, time, urllib.request, http.server, socketserver
from playwright.sync_api import sync_playwright
site = tempfile.mkdtemp(); os.makedirs(site+'/dash')
subprocess.run(['ffmpeg','-loglevel','error','-y','-f','lavfi','-i','testsrc=duration=6:size=320x240','-f','lavfi','-i','sine=duration=6','-c:v','libx264','-pix_fmt','yuv420p','-c:a','aac','-shortest',site+'/Sunset Timelapse.mp4'],check=True)
for i in (1,2,3): subprocess.run(['ffmpeg','-loglevel','error','-y','-f','lavfi','-i','sine=frequency=%d:duration=3'%(300*i),'-c:a','libmp3lame',site+'/ep%d.mp3'%i],check=True)
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(s,*a,**k): super().__init__(*a,directory=site,**k)
    def log_message(s,*a): pass
    def do_GET(s):
        if s.path.startswith('/feed.xml'):
            host=s.headers.get('Host'); items=''.join('<item><title>Episode %d</title><guid>e%d</guid><enclosure url="http://%s/ep%d.mp3" type="audio/mpeg" length="1"/></item>'%(i,i,host,i) for i in (1,2,3))
            b=('<?xml version="1.0"?><rss version="2.0"><channel><title>Garage Radio</title><link>http://%s/</link>%s</channel></rss>'%(host,items)).encode()
            s.send_response(200); s.send_header('Content-Type','application/rss+xml'); s.send_header('Content-Length',str(len(b))); s.end_headers(); s.wfile.write(b); return
        return super().do_GET()
srv = socketserver.ThreadingTCPServer(('127.0.0.1',0),H); threading.Thread(target=srv.serve_forever,daemon=True).start()
web='http://127.0.0.1:%d'%srv.server_address[1]
home = tempfile.mkdtemp(); env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home+'/c', WLC_YTDLP='/usr/local/bin/yt-dlp', WLC_FFMPEG='none')
p = subprocess.Popen([sys.argv[1],'-I','dummy','--no-video','--aout','adummy','--wlc-port','48793'],env=env,stdout=open(home+'/log','w'),stderr=subprocess.STDOUT)
time.sleep(3); ok=fail=0
def check(n,c,d=''):
    global ok,fail; ok+=bool(c); fail+=not c; print(('  ok   ' if c else '  FAIL ')+n+('' if c else '  -> '+str(d)[:300]),flush=True)
try:
  with sync_playwright() as pw:
    br = pw.chromium.launch(executable_path='/usr/bin/google-chrome'); pg = br.new_page(viewport={'width':1280,'height':900})
    errs=[]; pg.on('pageerror', lambda e: errs.append(str(e)))
    pg.goto('http://127.0.0.1:48793/'); pg.wait_for_selector('#nav')
    pg.click('#nav button:has-text("Downloads")'); pg.wait_for_selector('input[type=url]')
    check('Downloads tab opens with a link box', pg.is_visible('input[type=url]'))
    pg.fill('input[type=url]', web+'/feed.xml'); pg.click('button:has-text("Look up")')
    pg.wait_for_selector('text=Garage Radio', timeout=60000)
    check('looking up a playlist shows its title and items', pg.is_visible('text=Episode 2') and pg.is_visible('text=3 items'))
    pg.click('.seg button:has-text("Audio only")')
    pg.click('.seg button:has-text("Only some")'); pg.fill('input[placeholder="e.g. 1-5,8,10"]', '2-3')
    pg.screenshot(path='/tmp/dl-tab-lookup.png', full_page=True)
    pg.click('button.primary:has-text("Download")')
    pg.wait_for_selector('td >> text=done', timeout=90000)
    row = pg.inner_text('table >> nth=-1')
    check('the queue shows it done: 2 files in Music/WLC Downloads', '2 files' in row and 'WLC Downloads' in row, row)
    pg.screenshot(path='/tmp/dl-tab-done.png', full_page=True)
    pg.fill('input[type=url]', web+'/Sunset%20Timelapse.mp4'); pg.click('.seg button:has-text("Video")')
    pg.click('button.primary:has-text("Download")')
    for _ in range(180):
        if pg.locator('td span.ok').count() >= 2: break
        time.sleep(0.5)
    print('QUEUE:', pg.inner_text('table >> nth=-1').replace('\n',' | ')[:600])
    pg.screenshot(path='/tmp/dl-tab-video.png', full_page=True)
    pg.click('td button:has-text("Play") >> nth=0'); time.sleep(3)
    st = json.loads(urllib.request.urlopen('http://127.0.0.1:48793/api/status').read())
    check('"Play" on a finished download plays it in WLC', st.get('state')=='playing' and 'Sunset' in (st.get('title') or ''), st)
    lib = json.loads(urllib.request.urlopen('http://127.0.0.1:48793/api/library').read())
    check('downloads appear in the Media Center', any('Sunset Timelapse' in i['p'] for i in lib['items'])
          and any('Episode 3' in i['p'] for i in lib['items']), [i['p'] for i in lib['items']])
    check('no script errors', not errs, errs)
    br.close()
finally:
  p.terminate(); p.wait()
print('\n%d passed, %d failed'%(ok,fail))
