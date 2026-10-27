#!/usr/bin/env python3
"""Phone-only fixtures and touch helpers for Superscan acceptance checks.

Use an isolated HOME/XDG_CACHE_HOME for destructive history tests. This server
binds loopback only; storage reports contain fixture values, never site data.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

OUT = Path(__file__).resolve().parent / 'evidence' / 'superscan'
OUT.mkdir(parents=True, exist_ok=True)
os.environ.update(XDG_RUNTIME_DIR=ztos_user.RUNTIME_DIR, WAYLAND_DISPLAY='wayland-0')


def ctl(command, delay=0.25):
    with open(ztos_user.runtime('ztmetro.ctl'), 'w') as stream:
        stream.write(command+'\n')
    time.sleep(delay)


def tap(x, y, delay=0.4):
    ctl(f'tap {x} {y}', delay)


def shot(name, expected='superscan'):
    ctl('state')
    states = [line for line in ztos_user.home('.local/state/ztmetro.log').read_text(errors='replace').splitlines() if 'state: front=' in line]
    assert states and f'front={expected} ' in states[-1], states[-1:]
    subprocess.run(['grim', str(OUT/(name+'.png'))], check=True)


def menu(row):
    tap(342, 645)
    tap(100, 254+52*row, 1)


def navigate(url):
    # Address hold menu before the keyboard opens; Paste selects existing URL.
    ctl('focus superscan', 0.5)
    subprocess.run(['wl-copy', '--', url], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    ctl('focus superscan', 0.5)
    ctl('down 22 160 645', 0.75)
    ctl('up 22', 0.3)
    tap(100, 558, 1)
    ctl('focus superscan', 0.5)
    ctl('state')
    states = [line for line in ztos_user.home('.local/state/ztmetro.log').read_text(errors='replace').splitlines() if 'state: front=' in line]
    if 'keyboard=0 ' in states[-1]:
        ctl('osk show 672', 0.7)
    tap(332, 676, 4)


PROFILE = ztos_user.home('swarm-superscan/test-home')
HISTORY = PROFILE/'.local/share/superscan/history'


def launch(page, private=False):
    subprocess.run(['systemctl','--user','stop','swarm-superscan'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    PROFILE.mkdir(parents=True, exist_ok=True)
    command = ['systemd-run','--user','--unit=swarm-superscan','--collect',
               '--setenv=WAYLAND_DISPLAY=wayland-0','--setenv=ZTB_TRACE=1',
               '--setenv=HOME='+str(PROFILE),'--setenv=XDG_CACHE_HOME='+str(PROFILE/'.cache'),
               '/usr/local/bin/superscan']
    if private:
        command.append('--inprivate')
    command.append('http://127.0.0.1:8779/'+page)
    subprocess.run(command, check=True)
    time.sleep(5)
    ctl('focus superscan', 1)


def type_letters(word):
    for letter in word:
        if letter in 'qwertyuiop':
            tap(18+36*'qwertyuiop'.index(letter),504,.08)
        elif letter in 'asdfghjkl':
            tap(36+36*'asdfghjkl'.index(letter),560,.08)
        elif letter in 'zxcvbnm':
            tap(72+36*'zxcvbnm'.index(letter),617,.08)
        else:
            raise ValueError(letter)


def exercise(mode):
    if mode == 'private-check':
        before = HISTORY.read_bytes() if HISTORY.exists() else b''
        for name in ('private-a','private-b'):
            launch(name, private=True)
            shot(name)
            assert (HISTORY.read_bytes() if HISTORY.exists() else b'') == before
        tap(160,645,1)
        type_letters('linux')
        time.sleep(2)
        shot('private-no-suggestions')
        log = subprocess.check_output(['journalctl','--user','-u','swarm-superscan','--since','-15 seconds','--no-pager'], text=True)
        assert 'suggestion request' not in log
        launch('public')
        shot('public-storage-restored')
        reports = [json.loads(line) for line in (OUT/'storage-reports.jsonl').read_text().splitlines()]
        for name in ('private-a','private-b'):
            last = [r for r in reports if r.get('page') == ['/'+name]][-1]
            assert not last.get('cookie') and not last.get('local'), last
        public = [r for r in reports if r.get('page') == ['/public']][-1]
        assert public.get('cookie') == ['swarm=public'] and public.get('local') == ['public'], public
        menu(1)
        shot('private-from-menu')
    elif mode == 'history-check':
        subprocess.run(['systemctl','--user','stop','swarm-superscan'],check=False)
        HISTORY.parent.mkdir(parents=True,exist_ok=True)
        text = ''
        for i,(page,title) in enumerate([('public','Public fixture'),('yesterday','Yesterday fixture'),('older','Older fixture')]):
            text += f'[{i}]\nurl=http://127.0.0.1:8779/{page}\ntitle={title}\ntime={int(time.time())-i*86400}\n\n'
        HISTORY.write_text(text)
        launch('public')
        menu(5)
        shot('history-grouped')
        tap(100,177,1)
        type_letters('public')
        shot('history-search')
        tap(332,676,1)
        tap(323,286,1)
        assert HISTORY.read_text().count('url=') == 2
        shot('history-delete')
        tap(300,650,1)
        assert HISTORY.read_text().count('url=') == 0
        shot('history-clear')
        tap(60,710,1)
        bookmarks = HISTORY.parent/'bookmarks'
        if not bookmarks.exists() or '127.0.0.1:8779/public' not in bookmarks.read_text():
            menu(2)
        assert '127.0.0.1:8779/public' in bookmarks.read_text()
        tap(160,645,1)
        shot('bookmark-suggestions')
        tap(120,380,2)
        shot('bookmark-open')
    elif mode == 'suggest-check':
        launch('public')
        tap(160,645,1)
        type_letters('linux')
        time.sleep(4)
        shot('engine-suggestions-final')
        log = subprocess.check_output(['journalctl','--user','-u','swarm-superscan','--since','-15 seconds','--no-pager'], text=True)
        assert 'suggestion request' in log and 'rows=4' in log
        tap(120,195,4)
        shot('engine-suggestion-open')
    elif mode == 'scroll-check':
        launch('heavy')
        time.sleep(3)
        shot('snapshot-page')
        ctl('down 31 180 520',.1)
        ctl('move 31 180 430',.1)
        ctl('move 31 180 320',.1)
        ctl('up 31',1)
        shot('snapshot-scrolled')
        ctl('home',1)
        ctl('launch /usr/local/bin/ztsettings',3)
        shot('settings-final', 'ztsettings')
        ctl('focus superscan',2)
        shot('scroll-resumed')
        ctl('home',1)
        ctl('focus ztpower',3)
        shot('other-app-final','ztpower')
        launch('heavy')
        time.sleep(3)
        ctl('down 32 180 180',.1)
        ctl('move 32 180 300',.1)
        ctl('move 32 180 390',.1)
        shot('rubber-band-final')
        ctl('up 32',1)
    elif mode == 'download-check':
        launch('download')
        menu(4)
        shot('download-progress-final')
        ctl('home',1)
        ctl('launch /usr/local/bin/ztsettings',4)
        ctl('focus superscan',1)
        shot('download-complete-final')
        records = json.loads((HISTORY.parent/'downloads.json').read_text())
        assert records[-1].get('state') == 1 and records[-1].get('received') == 1048576, records
    else:
        raise SystemExit('Unknown check: '+mode)
    print(mode+' passed',flush=True)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        with (OUT/'fixture-requests.jsonl').open('a') as stream:
            stream.write(json.dumps({'time':time.time(), 'request':fmt % args})+'\n')

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == '/report':
            query = parse_qs(urlsplit(self.path).query)
            with (OUT/'storage-reports.jsonl').open('a') as stream:
                stream.write(json.dumps(query)+'\n')
            data, mime = b'ok', 'text/plain'
        elif path == '/download':
            self.send_response(200)
            self.send_header('Content-Type','application/octet-stream')
            self.send_header('Content-Disposition','attachment; filename="Superscan swarm check.bin"')
            self.send_header('Content-Length','1048576')
            self.end_headers()
            try:
                for _ in range(64):
                    self.wfile.write(b'S'*16384); self.wfile.flush(); time.sleep(.12)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        else:
            mode = 'private' if path.startswith('/private') else 'public'
            data = ('''<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Superscan MODE storage check</title>
<style>body{font:20px sans-serif;background:#fff;color:#111;margin:16px}a{display:block;padding:16px}canvas{width:100%;height:160px}</style>
<h2>MODE storage check</h2><pre id="result"></pre>
<a href="/second">Second history page</a><a href="/download">Download check</a>
<a href="/heavy">Scroll and snapshot check</a>
<script>
let before={page:location.pathname,cookie:document.cookie,local:localStorage.getItem('swarm')||''};
document.getElementById('result').textContent='Before visit:\\nCookie: '+(before.cookie||'(empty)')+'\\nStorage: '+(before.local||'(empty)');
fetch('/report?'+new URLSearchParams(before));
document.cookie='swarm=MODE; max-age=86400; path=/';localStorage.setItem('swarm','MODE');
if(location.pathname==='/heavy')for(let i=0;i<8;i++){let c=document.createElement('canvas');c.width=512;c.height=512;let x=c.getContext('2d');let g=x.createLinearGradient(0,0,512,512);g.addColorStop(0,'hsl('+i*40+',80%,50%)');g.addColorStop(1,'black');x.fillStyle=g;x.fillRect(0,0,512,512);document.body.append(c);}
</script>'''.replace('MODE',mode)).encode()
            mime = 'text/html; charset=utf-8'
        self.send_response(200)
        self.send_header('Content-Type',mime)
        self.send_header('Content-Length',str(len(data)))
        self.send_header('Cache-Control','public, max-age=60' if path != '/report' else 'no-store')
        self.end_headers()
        self.wfile.write(data)


if __name__ == '__main__':
    if sys.argv[1].endswith('-check'):
        exercise(sys.argv[1])
    elif sys.argv[1] == 'server':
        ThreadingHTTPServer(('127.0.0.1',8779),Handler).serve_forever()
    elif sys.argv[1] == 'shot':
        shot(sys.argv[2])
    elif sys.argv[1] == 'menu':
        menu(int(sys.argv[2]))
    elif sys.argv[1] == 'navigate':
        navigate(sys.argv[2])
    else:
        raise SystemExit('Use server, shot NAME, menu ROW, or navigate URL')
