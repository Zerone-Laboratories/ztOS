#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline tests of zrn-contacts-sync: a fake Google, invented people."""
import json, os, stat, sys, tempfile, threading, time, unittest, urllib.parse, urllib.request
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import zrn_contacts_sync as z

FIX = json.load(open(os.path.join(HERE, "fixture_people.json")))


class FakeGoogle:
    def __init__(self):
        self.script = []	# (status, dict) per People call
        self.urls = []
        self.token_replies = [(200, {"access_token": "at", "expires_in": 3600})]

    def __call__(self, url, data=None, headers=None):
        if url == z.TOKEN_URL:
            status, body = self.token_replies[0]
            return status, json.dumps(body)
        self.urls.append(url)
        status, body = self.script.pop(0)
        return status, json.dumps(body)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = os.path.join(self.tmp.name, "cfg")
        self.file = os.path.join(self.tmp.name, "share", "contacts")
        os.makedirs(self.cfg)
        os.makedirs(os.path.dirname(self.file))
        z.write_json(os.path.join(self.cfg, "client.json"), {"client_id": "id"})
        z.write_json(os.path.join(self.cfg, "token.json"), {"refresh_token": "rt"})
        self.fake = FakeGoogle()
        self.google = z.Google(self.cfg, self.fake)

    def tearDown(self):
        self.tmp.cleanup()

    def rows(self):
        with z.Store(self.file) as s:
            return {r["name"]: r for r in s.rows}

    def full(self):
        self.fake.script += [(200, FIX["full_page1"]), (200, FIX["full_page2"])]
        return z.sync_once(self.google, self.file)


class Sync(Base):
    def test_full_mapping_and_paging(self):
        c = self.full()
        self.assertEqual((c["kind"], c["added"]), ("full", 3))	# c4 has nothing to show
        r = self.rows()
        self.assertEqual(r["Test Alpha"]["mobile"], "+15550101")	# mobile first
        self.assertEqual(r["Test Alpha"]["other"], "+15550100")
        self.assertEqual(r["Test Alpha"]["note"], "line one line two")
        self.assertIn("Test Company", r)
        self.assertIn("pageToken=p2", self.fake.urls[1])

    def test_incremental(self):
        self.full()
        self.fake.script.append((200, FIX["incremental"]))
        c = z.sync_once(self.google, self.file)
        self.assertEqual(c["kind"], "incremental")
        self.assertIn("syncToken=sync-1", self.fake.urls[-1])
        r = self.rows()
        self.assertIn("Test Alpha Renamed", r)
        self.assertNotIn("Test Alpha", r)
        self.assertNotIn("Test Beta", r)
        self.assertIn("Test Gamma", r)

    def test_expired_token_full_keeps_phone_edits_and_deletes(self):
        self.full()
        with z.Store(self.file) as s:
            s.rows = [r for r in s.rows if r["name"] != "Test Beta"]	# deleted here
            for r in s.rows:
                if r["name"] == "Test Company":
                    r["note"] = "edited here"
            s.save()
        self.fake.script.append((400, {"error": {"details": [{"reason": "EXPIRED_SYNC_TOKEN"}]}}))
        self.fake.script += [(200, FIX["full_page1"]), (200, FIX["full_page2"])]
        c = z.sync_once(self.google, self.file)
        self.assertEqual(c["kind"], "full")
        r = self.rows()
        self.assertNotIn("Test Beta", r)
        self.assertEqual(r["Test Company"]["note"], "edited here")

    def test_full_removes_people_gone_from_google(self):
        self.full()
        os.unlink(os.path.join(self.cfg, "state.json"))
        z.write_json(os.path.join(self.cfg, "state.json"),
                     {"known": {z.uid_of("people/c1"): "e1", z.uid_of("people/c2"): "e2",
                                z.uid_of("people/c3"): "e3"}})
        self.fake.script.append((200, {"connections": FIX["full_page1"]["connections"][:1],
                                       "nextSyncToken": "s"}))
        z.sync_once(self.google, self.file)
        self.assertEqual(list(self.rows()), ["Test Alpha"])

    def test_empty_full_refused(self):
        with z.Store(self.file) as s:
            s.rows = [dict(zip(z.COLUMNS, (f"P{i}", "", "", "", "", f"g:people/c{i}"))) for i in range(9)]
            s.save()
        z.write_json(os.path.join(self.cfg, "state.json"),
                     {"known": {f"g:people/c{i}": "e" for i in range(9)}})
        self.fake.script.append((200, {"nextSyncToken": "s"}))
        with self.assertRaises(z.SyncError):
            z.sync_once(self.google, self.file)
        self.assertEqual(len(self.rows()), 9)

    def test_failed_page_changes_nothing(self):
        self.fake.script += [(200, FIX["full_page1"]), (503, {})]
        with self.assertRaises(z.HttpFailure):
            z.sync_once(self.google, self.file)
        self.assertEqual(self.rows(), {})
        self.assertFalse(os.path.exists(os.path.join(self.cfg, "state.json")))

    def test_local_people_survive_and_twins_merge(self):
        with z.Store(self.file) as s:
            s.rows = [dict(zip(z.COLUMNS, ("Mine", "555", "", "", "", ""))),
                      dict(zip(z.COLUMNS, ("Test Alpha", "+15550101", "", "", "", "")))]
            s.save()
        self.full()
        r = self.rows()
        self.assertEqual(r["Mine"]["uid"], "")
        self.assertTrue(r["Test Alpha"]["uid"].startswith("g:"))
        self.assertEqual(len(r), 4)

    def test_refused_refresh_needs_signin(self):
        self.fake.token_replies = [(400, {"error": "invalid_grant"})]
        with self.assertRaises(z.NeedsSignIn):
            z.sync_once(self.google, self.file)

    def test_files_private(self):
        self.full()
        for path in (os.path.join(self.cfg, "token.json"), os.path.join(self.cfg, "state.json"), self.file):
            self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600, path)


class Fields(unittest.TestCase):
    def test_clean_fits_utf8(self):
        out = z.clean("é" * 100 + "\t", 63)
        self.assertLessEqual(len(out.encode()), 63)
        out.encode().decode("utf-8")
        self.assertNotIn("\t", z.clean("a\tb\nc", 10))

    def test_long_uid_hashed(self):
        self.assertLessEqual(len(z.uid_of("people/" + "c" * 200)), 63)


class SignIn(unittest.TestCase):
    def test_pkce(self):
        v, c = z.pkce_pair()
        self.assertTrue(43 <= len(v) <= 128)
        self.assertNotIn("=", c)

    def test_url_has_no_device_flow_or_secret(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(z.auth_url("id", "http://127.0.0.1:1", "s", "c")).query)
        self.assertEqual(q["code_challenge_method"], ["S256"])
        self.assertEqual(q["scope"], [z.SCOPE])
        self.assertNotIn("client_secret", q)

    def test_state_checked(self):
        with self.assertRaises(z.SyncError):
            z.code_from_redirect("http://127.0.0.1/?code=x&state=bad", "good")
        self.assertEqual(z.code_from_redirect("http://127.0.0.1/?code=x&state=good\n", "good"), "x")

    def test_loopback_ignores_strays(self):
        out = {}
        t = threading.Thread(target=lambda: out.update(code=z.wait_for_redirect(18765, "st", 20)))
        t.start()
        time.sleep(0.3)
        try:
            urllib.request.urlopen("http://127.0.0.1:18765/favicon.ico")
        except Exception:
            pass
        urllib.request.urlopen("http://127.0.0.1:18765/?code=abc&state=st").read()
        t.join()
        self.assertEqual(out["code"], "abc")


if __name__ == "__main__":
    unittest.main()
