# zroov-source-torrent

Torrent source for the ZroovMusic engine (`../ENGINE.md`). URI: `torrent:OFFERID`.

Needs: `libtorrent-rasterbar` to run, plus `boost` headers to build, `python` 3.

    make && make install        # into $(PREFIX)/lib/zroovmusic/sources

## Use

Engine calls:

| call | does |
|---|---|
| `search QUERY LIMIT` | indexers only, no swarm. QUERY is `Artist - Title`, or JSON `{"artist","title","duration"}` for duration matching |
| `resolve OFFERID` | joins the swarm, answers `hold:true` once the buffer is there. The engine's SIGTERM leaves the swarm |
| `fetch` | refuses: this source streams, it does not save |

`resolve` with `ZROOV_PREFETCH=1` refuses, so a queued track never joins a swarm early.

The UI calls these directly, as they are not in the engine contract:

| call | does |
|---|---|
| `disclosure` | the first-use text and whether it was accepted |
| `consent yes\|no` | `resolve` and `verify` refuse until yes |
| `verify OFFERID...` | fetches the first pieces of each, reads the FLAC header, leaves. Ends with `{"type":"offer",...}` lines, best first |
| `settings [JSON]` | shows or changes settings |
| `clear-cache` | empties the cache |

Every search result carries an `offer` object, which the engine passes through:
`quality` (`status` is `verified`, `claimed` or `unknown`), `size`, `seeders` and
`seeders_from`, `verified`, `claimed`, `match` with reasons, `license` and
`autostart: false`. `verify` is a separate tap because it joins a swarm.

## Settings

Stored in `~/.config/zroovmusic/torrent.json` (0600, indexer links can hold keys).

| key | default | |
|---|---|---|
| `torznab_urls` | `[]` | the user's own Torznab or Jackett style URLs |
| `internet_archive` | `true` | built-in source |
| `ia_collections` | `netlabels`, `etree` | see below |
| `wifi_only` | `true` | checked at start and every 2 s; traffic is bound to that interface |
| `upload_kib` | `64` | upload cap. libtorrent reads 0 as unlimited, so 0 here means 1 KiB/s |
| `download_kib` | `0` | 0 is no cap |
| `seed_after_s` | `0` | how long to keep sharing after the track is left |
| `cache_mib` | `1024` | oldest entries go first |
| `buffer_seconds` | `5` | audio held before the player starts |
| `ahead_low_s`, `ahead_high_s` | `20`, `50` | seconds fetched beyond what the player has read: refill below low, up to high |
| `idle_stop_s` | `30` | pauses the torrent when nothing has been read for this long |
| `dht_for_magnets` | `false` | a magnet with no tracker finds no peers without it |

UPnP, NAT-PMP and local peer discovery are always off.

## What is verified and what is claimed

`quality.status` is `verified` only after STREAMINFO was read from pieces that passed
the torrent's hash check. It proves the stored bit depth, sample rate, channels and
length. It does not prove the master: an upsampled file looks the same. Names, the
archive's format field, indexer sizes and seeder counts stay claims.

A claim never outranks a verified file.

## Internet Archive

Search requires a Creative Commons or public domain licence field **and** one of the
collections above. The licence field alone is not enough: uploaders set it themselves,
and open upload collections hold commercial albums marked CC0. The offer reports it as
`license_is: stated by the uploader`.

The torrents' own web seed `https://archive.org/download/` redirects per file, and
libtorrent then treats the target as holding only that file, so pieces spanning two files
never arrive. The source passes the item's direct servers instead (`--webseed`).
