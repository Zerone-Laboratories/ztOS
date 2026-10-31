#!/usr/bin/env python3
"""End to end on a private session bus, with invented data: a scripted engine
socket, the real ztnotifyd writing to a stand-in for the shell's control pipe,
and a stand-in for ztOSIAL's Sleep1.

  e2e.py                       builds nothing; re-runs itself in dbus-run-session
  ZTNOTIFYD=path e2e.py        the ztnotifyd to run (default: compile the tree's,
                               with shell-actions.patch when it is not yet applied)
"""
import json, os, shutil, socket, stat, subprocess, sys, tempfile, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
if 'ZWN_IN_BUS' not in os.environ:
	os.environ['ZWN_IN_BUS'] = '1'
	# no service directories: the desktop's own notification daemon must not
	# be activated in place of the one under test
	with tempfile.NamedTemporaryFile('w', suffix='.conf') as conf:
		conf.write('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>'
			   '<policy context="default"><allow send_destination="*" eavesdrop="true"/>'
			   '<allow eavesdrop="true"/><allow own="*"/></policy></busconfig>')
		conf.flush()
		sys.exit(subprocess.call(['dbus-run-session', '--config-file', conf.name, '--',
					  sys.executable, '-I', __file__]))

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

failed = 0
def check(ok, what):
	global failed
	print(('ok   ' if ok else 'FAIL ') + what)
	failed += 0 if ok else 1

tmp = Path(tempfile.mkdtemp(prefix='zwn-e2e-'))
run = tmp / 'run'; run.mkdir(mode=0o700)
os.environ['XDG_RUNTIME_DIR'] = str(run)
state = tmp / 'state'
sleep_dir = tmp / 'sleep'; sleep_dir.mkdir()
status = sleep_dir / 'status'

def build_ztnotifyd():
	if os.environ.get('ZTNOTIFYD'):
		return os.environ['ZTNOTIFYD']
	source = HERE.parent.parent / 'notifyd/ztnotifyd.c'
	text = source.read_text()
	work = tmp / 'nd'; work.mkdir()
	shutil.copy(source, work / 'ztnotifyd.c')
	if 'notifyx' not in text:
		patch = (HERE / 'shell-actions.patch').read_text()
		patch = patch[:patch.index('diff --git a/ztmetro/ztmetro.c')]
		subprocess.run(['patch', '-s', '-p3', '-d', str(work)], input=patch, text=True, check=True)
	flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libsystemd'], text=True).split()
	subprocess.run(['gcc', '-O1', '-w', '-o', str(work / 'ztnotifyd'), str(work / 'ztnotifyd.c')] + flags, check=True)
	return str(work / 'ztnotifyd')

# ---- the shell's control pipe
ctl = run / 'ztmetro.ctl'
os.mkfifo(ctl)
ctl_fd = os.open(ctl, os.O_RDONLY | os.O_NONBLOCK)
ctl_keep = os.open(ctl, os.O_WRONLY)
buffered = b''
def expect(pred, what, wait=4.0):
	"""The next shell line that meets pred, or None."""
	global buffered
	end = time.time() + wait
	while time.time() < end:
		try:
			buffered += os.read(ctl_fd, 65536)
		except BlockingIOError:
			pass
		lines = buffered.split(b'\n')
		buffered = lines.pop()
		found = None
		rest = []
		for line in lines:
			text = line.decode()
			if text.startswith('media '):
				continue	# ztnotifyd's own business
			if found is None and pred(text):
				found = text
			else:
				rest.append(line)
		buffered = b''.join(l + b'\n' for l in rest) + buffered
		if found is not None:
			check(True, what)
			return found
		time.sleep(0.03)
	check(False, what + f' (left: {buffered!r})')
	return None
def silent(what, wait=1.2):
	global buffered
	time.sleep(wait)
	try:
		buffered += os.read(ctl_fd, 65536)
	except BlockingIOError:
		pass
	buffered = b''.join(l + b'\n' for l in buffered.split(b'\n') if l and not l.startswith(b'media '))
	check(not buffered, what + ('' if not buffered else f' (got {buffered!r})'))
	buffered = b''

# ---- Sleep1
calls = []
SLEEP_XML = '''<node><interface name="org.ztos.ial.Sleep1">
<method name="Inhibit"><arg direction="in" type="s"/><arg direction="in" type="s"/><arg direction="out" type="u"/></method>
<method name="Uninhibit"><arg direction="in" type="u"/></method></interface></node>'''
def on_call(conn, sender, path, iface, method, args, inv):
	calls.append((method, time.time()))
	inv.return_value(GLib.Variant('(u)', (5,)) if method == 'Inhibit' else None)
loop = GLib.MainLoop()
def on_bus(conn, name):
	conn.register_object('/org/ztos/ial/Sleep', Gio.DBusNodeInfo.new_for_xml(SLEEP_XML).interfaces[0], on_call)
Gio.bus_own_name(Gio.BusType.SESSION, 'org.ztos.ial', 0, on_bus, None, None)
threading.Thread(target=loop.run, daemon=True).start()

# ---- the engine
listener = socket.socket(socket.AF_UNIX)
listener.bind(str(run / 'ztwhatsapp.sock'))
listener.listen(2)
def send(client, **event):
	client.sendall((json.dumps(event) + '\n').encode())
NOW = int(time.time())
serial = iter(range(100, 10000))
def message(client, ts=None, live=True, chat='c1@test', **more):
	n = next(serial)
	event = dict(event='message', chat=chat, chat_name='Invented Chat', sender='s@test', sender_name='Inventor',
		     text=more.pop('text', f'test message {n}'), timestamp=ts or NOW + n, id=f'id{n}', from_me=False,
		     is_group=False, muted=False, live=live, message=dict(kind=more.pop('kind', 'text')))
	event.update(more)
	send(client, **event)

nd_path = build_ztnotifyd()
notifier = subprocess.Popen([str(HERE / 'ztwhatsapp-notify'), '--state-dir', str(state), '--sleep-status', str(status),
			     '--ial-on-user-bus'], stderr=open(tmp / 'notify.log', 'w'))
listener.settimeout(5)
client, _ = listener.accept()
send(client, event='hello', ipc_version=1, daemon_pid=1)
send(client, event='pairing', state='paired', self='x@test')
send(client, event='connection', state='connected', detail='')
time.sleep(0.2)

# the service is not up yet (the shell starts it): the message must wait for it
message(client, text='sent before the service')
time.sleep(1.0)
nd = subprocess.Popen([nd_path], stderr=open(tmp / 'nd.log', 'w'))
line = expect(lambda l: l.startswith('notifyx '), 'a message before ztnotifyd arrives once it is up', 6)
nid = line.split()[1].split('|')[0] if line else '0'
if line:
	check(line.endswith('|ztwhatsapp|WhatsApp|Invented Chat|sent before the service'), 'identity, chat and text: ' + line)

time.sleep(0.2)
message(client, text='second')
line = expect(lambda l: l.startswith('notifyx'), 'a second message updates', 3)
if line:
	check(line.startswith(f'notifyxq {nid}|') and '2 new messages. second' in line, 'same id, quiet, counted: ' + line)

message(client, from_me=True)
message(client, muted=True)
message(client, kind='system')
message(client, ts=1000, live=False)
silent('own, muted, system and old backlog stay silent')

(run / 'ztwhatsapp.focus').write_text(f'{os.getpid()} c2@test\n')
message(client, chat='c2@test')
silent('the chat on screen stays silent')
(run / 'ztwhatsapp.focus').write_text('999999 c2@test\n')
message(client, chat='c2@test', text='focus file of a dead UI')
expect(lambda l: 'focus file of a dead UI' in l, 'a dead UI does not silence')
(run / 'ztwhatsapp.focus').unlink()

message(client, chat='c3@test', live=False, text='came in during the sleep')
line = expect(lambda l: 'came in during the sleep' in l, 'backlog newer than the watermark notifies')
send(client, event='message', chat='c6@test', id='bare', from_me=False, live=True, muted=False,
     message=dict(kind='text', text='only in the record', timestamp=NOW + 50000))
expect(lambda l: 'only in the record' in l, 'an event with only the Message object')
send(client, event='history_sync', state='running', progress=1)
message(client, chat='c4@test', live=False, text='history')
silent('nothing during a history sync')
send(client, event='history_sync', state='done')

subprocess.run([nd_path, '--invoke', nid], check=True)
expect(lambda l: l == 'launch ztwhatsapp', 'a tap asks the shell for the app')
opened = run / 'ztwhatsapp.open'
check(opened.exists() and opened.read_text() == 'c1@test\n', 'a tap names the chat')
check(stat.S_IMODE(opened.stat().st_mode) == 0o600, 'the open file is private')

message(client, chat='c5@test', text='to be read')
line = expect(lambda l: 'to be read' in l, 'a new chat notifies')
read_id = line.split()[1].split('|')[0] if line else '0'
send(client, event='chat', chat=dict(id='c5@test', unread=0))
expect(lambda l: l == f'notifyclose {read_id}', 'read elsewhere closes it')

# ---- sleep
def sleepd(**fields):
	(sleep_dir / 'status.new').write_text(''.join(f'{k}={v}\n' for k, v in fields.items()))
	os.rename(sleep_dir / 'status.new', status)
sleepd(state='awake', wakes=3, last='rtc', slept=300)
time.sleep(0.3)
check(not calls, 'the first status read holds nothing')
sleepd(state='awake', wakes=4, last='hub', slept=1)
sleepd(state='awake', wakes=5, last='rtc', slept=5)
sleepd(state='awake', wakes=6, last='key', slept=300)
time.sleep(0.5)
check(not calls, 'hub wakes, short sleeps and the power key hold nothing')
send(client, event='connection', state='offline', detail='')
time.sleep(0.1)
sleepd(state='awake', wakes=7, last='rtc', slept=300)
time.sleep(0.5)
check([c[0] for c in calls] == ['Inhibit'], 'a doze wake takes the inhibit')
send(client, event='connection', state='connected', detail='')
t0 = time.time()
while len(calls) < 2 and time.time() - t0 < 8:
	time.sleep(0.1)
held = time.time() - t0
check([c[0] for c in calls] == ['Inhibit', 'Uninhibit'] and 3.5 <= held <= 5.5, f'released {held:.1f}s after live')

# ---- idle means no wakeups
def switches():
	for line in Path(f'/proc/{notifier.pid}/status').read_text().splitlines():
		if line.startswith('voluntary_ctxt_switches'):
			return int(line.split()[1])
time.sleep(1.5)
before = switches()
time.sleep(5)
check(switches() == before, f'no wakeups in 5 s idle ({switches() - before})')
fds = [os.readlink(f'/proc/{notifier.pid}/fd/{f}') for f in os.listdir(f'/proc/{notifier.pid}/fd')]
check(not any('timerfd' in f for f in fds), 'no timerfd, so no alarm clock')

# ---- nothing private in its log
log = (tmp / 'notify.log').read_text()
check(not any(w in log for w in ('Invented', 'Inventor', 'test message', '@test')), 'no names or text in the log')
wm = state / 'notify-watermark'
notifier.terminate()
check(notifier.wait(5) == 0, 'a clean stop')
check(wm.exists() and wm.read_text().strip().isdigit(), 'the watermark is only a number')
check(not (run / 'ztwhatsapp-notify.status').exists(), 'its status goes with it')
nd.terminate()
loop.quit()
shutil.rmtree(tmp, ignore_errors=True)
print('e2e: ok' if not failed else f'e2e: {failed} failed')
sys.exit(1 if failed else 0)
