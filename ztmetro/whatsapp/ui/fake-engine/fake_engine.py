#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Speaks ztmetro/whatsapp/IPC.md with invented people and messages, for testing ztwhatsapp.

  fake_engine.py [--socket PATH] [--messages N] [--unpaired] [--every SECONDS]

--unpaired starts at the pairing flow: asking for a QR gives rotating codes, and
after two of them it reports paired. --every sends a new message that often.
"""
import argparse, base64, json, os, selectors, socket, time

NAMES = ["Alex Example", "Sam Sample", "Family", "Robin Test", "Jo Placeholder", "Book Club", "Kim Madeup"]
LINES = ["Are you coming today?", "Yes, around ten.", "Bring the blue folder if you can, the one from the top shelf, "
	"because we will need it for the meeting and nobody else knows where it went.", "ok", "Sounds good :)",
	"Line one\nLine two\nLine three", "දැන් එනවා", "Running late, sorry. The bus never came and I had to walk most of the way there."]
# 2x2 JPEG, grey: stands in for a thumbnail
THUMB = "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgICAgMCAgIDAwMDBAYEBAQEBAgGBgUGCQgKCgkICQkKDA8MCgsOCwkJDRENDg8QEBEQCgwSExIQEw8QEBD/wAALCAACAAIBAREA/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AVN//2Q=="

ap = argparse.ArgumentParser()
ap.add_argument("--socket", default=os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "ztwhatsapp.sock"))
ap.add_argument("--messages", type=int, default=300)
ap.add_argument("--unpaired", action="store_true")
ap.add_argument("--every", type=float, default=20)
ap.add_argument("--chats", type=int, default=7, help="how many chats, with invented names")
ap.add_argument("--picture", default="", help="a JPEG to give as everyone's profile picture")
args = ap.parse_args()

if args.picture:
	THUMB = base64.b64encode(open(args.picture, "rb").read()).decode()
now = int(time.time())
chats = [dict(id=f"6140000000{i}@{'g.us' if i in (2, 5) else 's.whatsapp.net'}", name=n, is_group=i in (2, 5),
	unread=[2, 0, 11, 0, 0, 1, 0][i], last_text=LINES[i % len(LINES)].replace("\n", " "), last_ts=now - i * 5000,
	last_from_me=i == 1, muted=False, pinned=False, archived=False) for i, n in enumerate(NAMES)]
for i in range(7, args.chats):
	chats.append(dict(id=f"6141{i:07d}@s.whatsapp.net", name=f"Invented Person {i}", is_group=False, unread=i % 5, last_text=LINES[i % len(LINES)].replace("\n", " "),
		last_ts=now - i * 5000, last_from_me=False, muted=False, pinned=False, archived=False))
store = {}

def make(chat, i, ts):
	mine = i % 3 == 1
	m = dict(chat=chat["id"], id=f"M{i}", from_me=mine, sender="" if mine else chat["id"],
		sender_name="Sam Sample" if chat["is_group"] and not mine else "", timestamp=ts, kind="text",
		text=LINES[i % len(LINES)], status="read" if mine else "received", quoted_id="", media=None)
	if i % 17 == 5:
		m.update(kind="image", text="Caption of a photo" if i % 2 else "",
			media=dict(mime="image/jpeg", size=9000, width=800, height=600, file_name="", thumb_b64=THUMB, path=""))
	return m

for n, c in enumerate(chats):
	count = args.messages if n < 7 else 40
	store[c["id"]] = [make(c, i, c["last_ts"] - (count - i) * 400) for i in range(count)]

chats[6]["archived"] = True
store[chats[1]["id"]][-3]["starred"] = True
def status_item(sender, i, ts, seen=False):
	return dict(chat="status@broadcast", id=f"S{sender[:12]}{i}", from_me=False, sender=sender, sender_name="", timestamp=ts,
		kind="text", text=f"Invented status number {i}", status="received", quoted_id="", media=None, seen=seen)
status_people = [dict(sender=chats[i]["id"], name=chats[i]["name"], last_ts=now - i * 900, unseen=2 if i == 0 else 0,
	items=[status_item(chats[i]["id"], k, now - i * 900 - k, seen=i != 0) for k in range(2)]) for i in (0, 3)]
people = [dict(jid=c["id"], name=c["name"], push_name=c["name"].split()[0], phone="+" + c["id"].split("@")[0])
	for c in chats if not c["is_group"]] + [dict(jid="61400000050@s.whatsapp.net", name="Lee Invented", push_name="Lee", phone="+61400000050")]

pairing = "unpaired" if args.unpaired else "paired"
clients, buffers = [], {}
qr_due = next_msg = 0
qr_left = 0
counter = 10**6
sel = selectors.DefaultSelector()

def send(c, obj):
	try:
		c.sendall((json.dumps(obj, ensure_ascii=False) + "\n").encode())
	except OSError:
		pass

def everyone(obj):
	for c in list(clients):
		send(c, obj)

def pairing_event(state, **kw):
	everyone(dict(event="pairing", state=state, qr=kw.get("qr", ""), expires_in=20, code=kw.get("code", ""), error=""))

def handle(c, req):
	global pairing, qr_due, qr_left, counter
	cmd, rid = req.get("cmd"), req.get("id")
	def ok(**r):
		send(c, dict(id=rid, ok=True, result=r))
	if cmd == "chats":
		ok(chats=chats, total=len(chats))
	elif cmd == "messages":
		all_ = store.get(req.get("chat"), [])
		end = len(all_)
		if req.get("before_id"):
			end = next((i for i, m in enumerate(all_) if m["id"] == req["before_id"]), 0)
		start = max(0, end - req.get("limit", 50))
		ok(messages=all_[start:end], more=start > 0)
	elif cmd == "send_text":
		counter += 1
		chat = next(x for x in chats if x["id"] == req["chat"])
		m = dict(make(chat, 0, int(time.time())), id=f"M{counter}", from_me=True, text=req["text"], status="pending", kind="text", media=None)
		store[chat["id"]].append(m)
		ok(id=m["id"], timestamp=m["timestamp"])
		everyone(dict(event="message", chat=chat["id"], id=m["id"], from_me=True, live=True, muted=False, message=m))
		m["status"] = "delivered"
		everyone(dict(event="receipt", chat=chat["id"], ids=[m["id"]], status="delivered", sender=""))
	elif cmd == "mark_read":
		for x in chats:
			if x["id"] == req["chat"]:
				x["unread"] = 0
				everyone(dict(event="chat", chat=x))
		ok(marked=0)
	elif cmd == "pair_qr":
		pairing, qr_due, qr_left = "qr", time.time(), 2
		ok()
	elif cmd == "pair_phone":
		pairing = "phone_code"
		ok(code="ABCD-EFGH")
		pairing_event("phone_code", code="ABCD-EFGH")
	elif cmd == "pair_cancel":
		pairing = "unpaired"
		ok()
		pairing_event("unpaired")
	elif cmd == "logout":
		pairing = "unpaired"
		ok()
		pairing_event("unpaired")
	elif cmd == "download_media":
		ok(path="")	# no real file: the app says it could not open it
	elif cmd == "statuses":
		ok(contacts=status_people)
	elif cmd == "status_seen":
		for p in status_people:
			if p["sender"] == req["sender"]:
				for it in p["items"]:
					if it["id"] in req["ids"] and not it["seen"]:
						it["seen"] = True
						p["unseen"] -= 1
		ok()
	elif cmd == "post_status":
		counter += 1
		ok(id=f"S{counter}", timestamp=int(time.time()))
	elif cmd == "contacts":
		ok(contacts=people, total=len(people))
	elif cmd == "profile_picture":
		if args.picture:
			ok(path=args.picture)
		else:
			send(c, dict(id=rid, ok=False, error="not_found", message=""))
	elif cmd == "group_info":
		ok(id=req["chat"], subject="", topic="An invented group", owner="", created=now, announce=False, locked=False,
			participants=[dict(jid=p["jid"], name=p["name"], is_admin=i == 0, is_super_admin=False) for i, p in enumerate(people)])
	elif cmd == "search":
		q = req["query"].lower()
		hits = [m for ms in store.values() for m in ms if q in m["text"].lower()][-req.get("limit", 50):]
		ok(messages=hits[::-1], more=False)
	elif cmd == "starred":
		ok(messages=[m for ms in store.values() for m in ms if m.get("starred")])
	elif cmd in ("typing", "set_presence", "subscribe_presence"):
		ok()
	elif cmd == "status":
		ok(ipc_version=1, connection="connected", pairing=pairing, self="61400000099@s.whatsapp.net", push_name="Tester")
	else:
		send(c, dict(id=rid, ok=False, error="unknown_cmd", message=str(cmd)))

try:
	os.unlink(args.socket)
except FileNotFoundError:
	pass
server = socket.socket(socket.AF_UNIX)
server.bind(args.socket)
os.chmod(args.socket, 0o600)
server.listen()
sel.register(server, selectors.EVENT_READ)
next_msg = time.time() + args.every
while True:
	for key, _ in sel.select(0.5):
		if key.fileobj is server:
			c, _a = server.accept()
			clients.append(c)
			buffers[c] = b""
			sel.register(c, selectors.EVENT_READ)
			send(c, dict(event="hello", ipc_version=1, daemon_pid=os.getpid()))
			send(c, dict(event="connection", state="connected", detail=""))
			send(c, dict(event="pairing", state=pairing, qr="", expires_in=0, code="", error=""))
			continue
		c = key.fileobj
		try:
			data = c.recv(65536)
		except OSError:
			data = b""
		if not data:
			sel.unregister(c)
			clients.remove(c)
			c.close()
			continue
		buffers[c] += data
		while b"\n" in buffers[c]:
			line, buffers[c] = buffers[c].split(b"\n", 1)
			handle(c, json.loads(line))
	t = time.time()
	if pairing == "qr" and t >= qr_due:
		if qr_left:
			qr_left -= 1
			qr_due = t + 8
			pairing_event("qr", qr="2@" + base64.b64encode(os.urandom(150)).decode())
		else:
			pairing = "paired"
			pairing_event("paired")
	if pairing == "paired" and t >= next_msg:
		next_msg = t + args.every
		counter += 1
		chat = chats[counter % 3]
		m = make(chat, 0, int(t))
		m.update(id=f"M{counter}", text=LINES[counter % len(LINES)])
		store[chat["id"]].append(m)
		chat.update(unread=chat["unread"] + 1, last_text=m["text"].replace("\n", " "), last_ts=m["timestamp"], last_from_me=False)
		everyone(dict(event="message", chat=chat["id"], id=m["id"], from_me=False, live=True, muted=False, message=m))
		everyone(dict(event="chat", chat=chat))
