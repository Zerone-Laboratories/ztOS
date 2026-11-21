import io, json, os, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import zroov_torrent as z


def flac(rate, bits, ch, samples):
    v = (rate << 44) | ((ch - 1) << 41) | ((bits - 1) << 36) | samples
    return b"fLaC\x00\x00\x00\x22" + b"\x10\x00\x10\x00" + b"\0" * 6 + v.to_bytes(8, "big") + b"\0" * 16


class Flac(unittest.TestCase):
    def test_hi_res(self):
        i = z.parse_flac_header(flac(96000, 24, 2, 96000 * 200))
        self.assertEqual((i["bit_depth"], i["sample_rate"], i["channels"], i["duration"]), (24, 96000, 2, 200.0))

    def test_real_bytes_from_archive_org(self):
        # First 42 bytes of a real netlabel file, also read back with metaflac.
        h = bytes.fromhex("664c6143000000221000100000067f002a290bb802f00142bc00ae6018c63fc81c90034cc8fbd1542b74")
        i = z.parse_flac_header(h)
        self.assertEqual((i["sample_rate"], i["bit_depth"]), (48000, 16))
        self.assertAlmostEqual(i["duration"], 440.64, 1)

    def test_id3_prefix(self):
        h = b"ID3\x03\0\0\0\0\0\x05" + b"xxxxx" + flac(44100, 16, 2, 44100)
        self.assertEqual(z.parse_flac_header(h)["bit_depth"], 16)

    def test_rejects(self):
        self.assertIsNone(z.parse_flac_header(b"RIFF" + b"\0" * 60))
        self.assertIsNone(z.parse_flac_header(b"fLaC"))


class Match(unittest.TestCase):
    q = {"artist": "Riding Alone For Thousands Of Miles", "title": "Brick City Love Song", "duration": 441}

    def test_good(self):
        s, _ = z.match_score(self.q, path="RidingAloneForThousandsOfMiles-BrickCityLoveSong.flac", duration=440.64)
        self.assertGreater(s, 0.9)

    def test_wrong_title_and_duration(self):
        s, _ = z.match_score(self.q, artist=self.q["artist"], path="01-Lullaby.flac", duration=509)
        self.assertLessEqual(s, 0.4)
        s, _ = z.match_score(self.q, artist=self.q["artist"], title=self.q["title"], path="a.flac", duration=3000)
        self.assertLessEqual(s, 0.4)

    def test_not_flac(self):
        self.assertEqual(z.match_score(self.q, path="x.mp3", is_flac=False)[0], 0)

    def test_pick_file(self):
        files = [{"i": 0, "path": "a/01 Lullaby.flac", "size": 1}, {"i": 1, "path": "a/02 Brick City Love Song.flac", "size": 2},
                 {"i": 2, "path": "a/02 Brick City Love Song.mp3", "size": 3}]
        self.assertEqual(z.pick_file(files, self.q)[0]["i"], 1)
        self.assertEqual(z.pick_file(files, self.q, "01 Lullaby.flac")[0]["i"], 0)
        self.assertIsNone(z.pick_file(files, self.q, "gone.flac")[0])


class Rank(unittest.TestCase):
    def test_claim_never_beats_verified(self):
        claim = {"claimed": {"bit_depth": 24, "sample_rate": 192000}, "verified": None, "match": 1}
        real = {"claimed": {}, "verified": {"bit_depth": 24, "sample_rate": 48000}, "match": 0.6}
        cd = {"claimed": {}, "verified": {"bit_depth": 16, "sample_rate": 44100}, "match": 1}
        order = sorted([cd, claim, real], key=z.rank_key, reverse=True)
        self.assertIs(order[0], real)
        self.assertIs(order[1], cd)

    def test_claimed_names(self):
        self.assertEqual(z.claimed_from_name("Album [24-96] FLAC"), {"bit_depth": 24, "sample_rate": 96000})
        self.assertEqual(z.claimed_from_name("x 24bit 88.2kHz"), {"bit_depth": 24, "sample_rate": 88200})
        self.assertEqual(z.claimed_from_name("plain"), {})


class Torznab(unittest.TestCase):
    XML = b"""<rss xmlns:torznab="http://torznab.com/schemas/2015/feed"><channel>
    <item><title>Some Artist - Some Song [24-96] FLAC</title><link>http://i.invalid/d/1.torrent</link><size>5000</size>
    <torznab:attr name="seeders" value="7"/></item>
    <item><title>Some Artist - Some Song 128kbps MP3</title><link>http://i.invalid/d/2.torrent</link></item>
    <item><title>Other - Thing FLAC</title><link>http://i.invalid/d/3.torrent</link></item>
    </channel></rss>"""

    def test_parse(self):
        got = z.parse_torznab(self.XML, {"artist": "Some Artist", "title": "Some Song"}, "i.invalid")
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0]["seeders"], 7)
        self.assertEqual(got[0]["claimed"], {"bit_depth": 24, "sample_rate": 96000})

    def test_redact(self):
        self.assertEqual(z.redact("http://h/api?apikey=abc&t=1"), "http://h/api?apikey=***&t=1")


class Policy(unittest.TestCase):
    def source(self):
        d = tempfile.mkdtemp()
        os.environ.pop("XDG_CONFIG_HOME", None); os.environ.pop("XDG_CACHE_HOME", None)
        out = io.StringIO()
        return z.Source(out=out, home=d, helper="/bin/false"), out

    def events(self, out):
        return [json.loads(l) for l in out.getvalue().splitlines()]

    def test_defaults(self):
        self.assertEqual(z.DEFAULTS["torznab_urls"], [])
        self.assertTrue(z.DEFAULTS["wifi_only"])
        self.assertEqual(z.DEFAULTS["seed_after_s"], 0)

    def test_no_swarm_before_consent(self):
        s, out = self.source()
        s.offers = {"a": {"id": "a", "torrent_url": "http://x/y", "magnet": "", "file": "f", "size": 1}}
        s.resolve("a")
        s.verify(["a"])
        ev = self.events(out)
        self.assertEqual([e["type"] for e in ev], ["error", "error"])
        self.assertIn("consent_required", ev[0]["reason"])
        self.assertIsNone(s.child)

    def test_search_never_starts(self):
        s, out = self.source()
        s.state["settings"]["internet_archive"] = False
        s.search("a - b")
        self.assertIsNone(s.child)
        self.assertEqual(self.events(out)[-1]["type"], "done")

    def test_query_forms(self):
        self.assertEqual(z.split_query("A - B")["title"], "B")
        self.assertEqual(z.split_query('{"artist":"A","title":"B","duration":3}')["duration"], 3)
        self.assertEqual(z.split_query("B"), {"artist": "", "title": "B", "duration": None})

    def test_evict_oldest_first(self):
        d = tempfile.mkdtemp()
        for i, n in enumerate(("old", "new", "live")):
            os.makedirs(f"{d}/{n}")
            with open(f"{d}/{n}/f", "wb") as f:
                f.write(os.urandom(1 << 20))
            os.utime(f"{d}/{n}", (1000 + i, 1000 + i))
        removed, total = z.evict(d, 2 << 20, keep=("live",))
        self.assertEqual(removed, ["old"])
        self.assertTrue(os.path.isdir(f"{d}/live"))


if __name__ == "__main__":
    unittest.main()
