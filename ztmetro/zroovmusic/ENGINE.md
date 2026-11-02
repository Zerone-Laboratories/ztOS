# ZroovMusic engine

`zroovd` is the playback engine: one small C process on a `poll()` loop, with no
timers while nothing plays and no threads of its own. The UI talks to it over a
socket. Sources (YouTube, local cache, torrents) are separate helper programs, so
a slow or crashing source can never stall playback.

Status: this document is the contract. Measurements and limits are at the end.

## Starting it

    $XDG_RUNTIME_DIR/zroovmusic/engine.sock      SOCK_STREAM, Unix

Connect. If that fails, start `zroovd` (it forks nothing and stays in the
foreground, so start it detached) and connect again; a second copy exits at once
with status 3. `zroovd` exits by itself when it is stopped and no client has been
connected for 60 s. A client that goes away does not stop the music.

## Wire format

One JSON object per line, UTF-8, both ways. Requests may carry `"seq"`; the reply
repeats it. Unknown fields are ignored, so add fields freely.

    > {"cmd":"play","seq":7,"tracks":[{"uri":"yt:dQw4w9WgXcQ","title":"T","artist":"A","duration":212}],"index":0}
    < {"ev":"reply","seq":7,"ok":true}
    < {"ev":"state","state":"resolving", ...}

A failed request: `{"ev":"reply","seq":7,"ok":false,"error":"text"}`.

### Track

| field      | meaning                                                          |
|------------|------------------------------------------------------------------|
| `uri`      | required. `SOURCE:REF`, see Sources. A bare `/path` means `file:` |
| `title`, `artist`, `album`, `art` | shown text; `art` is a path or address |
| `duration` | seconds if known; replaced by the real length when it is known   |

The engine keeps and returns every field it was given, so a UI may add its own.

### Commands

| cmd          | fields                  | does                                          |
|--------------|-------------------------|-----------------------------------------------|
| `play`       | `tracks`, `index`       | replaces the queue and starts `index` (default 0) |
| `play`       | `index` only            | starts that entry of the present queue        |
| `enqueue`    | `tracks`, `next` (bool) | appends, or inserts after the current one     |
| `remove`     | `index`                 | removes an entry                              |
| `clear`      |                         | stops and empties the queue                   |
| `pause`, `resume`, `toggle` |          |                                               |
| `stop`       |                         | stops, frees the player, keeps the queue      |
| `next`, `previous` |                   | `previous` restarts the track if past 3 s     |
| `seek`       | `pos` or `rel` (seconds) |                                              |
| `shuffle`    | `on` (bool)             | order changes, the current track stays        |
| `repeat`     | `mode`: `off` `one` `all` |                                             |
| `volume`     | `level` 0..100          | this stream only, not the system volume       |
| `get`        |                         | sends `state` and `queue` to this client      |
| `search`     | `query`, `source` (default `yt`), `limit` | replies now, results by event `search`, echoing `seq` |
| `save`       | `track`                 | starts an offline copy, events `save`         |
| `unsave`     | `uri`                   | deletes the offline copy                      |
| `saved`      |                         | event `saved` with all offline tracks         |
| `quit`       |                         | ends the engine                               |

### Events

Sent to every client when something changes, never on a timer. **There is no
position tick**: draw the position yourself from `pos` and `pos_at`.

    {"ev":"state","state":"playing","index":2,"track":{...},
     "pos":12.4,"pos_at":183423.1,"duration":212.0,"cached_to":60.0,
     "shuffle":false,"repeat":"all","volume":100,
     "format":{"codec":"flac","rate":96000,"bits":24,"channels":2,"out_rate":96000,"out_format":"s32"},
     "via":"fast"}

| field     | meaning                                                                   |
|-----------|---------------------------------------------------------------------------|
| `state`   | `stopped` `resolving` `buffering` `playing` `paused`                       |
| `pos_at`  | `CLOCK_MONOTONIC` seconds when `pos` was true. While `playing`, the position now is `pos + (now - pos_at)` |
| `format`  | `rate`, `bits`: the file. `out_rate`, `out_format`: what is handed to PipeWire |
| `via`     | how the link was got: `fast`, `yt-dlp`, `cache`, `file`, or the helper's word |

Others:

    {"ev":"queue","tracks":[...],"order":[3,0,1,2],"index":2}   order: playing order, indexes into tracks
    {"ev":"started","uri":"...","resolve_ms":640,"total_ms":910,"via":"fast"}   once per start
    {"ev":"error","uri":"...","reason":"text","fatal":false}    the engine has moved on unless fatal
    {"ev":"search","seq":9,"results":[Track...],"done":true}
    {"ev":"save","uri":"...","percent":40.0}
    {"ev":"save","uri":"...","path":"/...","done":true}         or "error":"text"
    {"ev":"saved","tracks":[Track + "path"...]}

## Sources

The scheme of a URI picks a helper, `zroov-source-SCHEME`, searched for in
`$ZROOV_SOURCE_PATH` (colon separated), then the `sources/` directory beside
`zroovd`, then `/usr/local/lib/zroovmusic/sources`. `file:` is built in.
The engine runs a helper only while it has work for it.

    zroov-source-NAME resolve REF
    zroov-source-NAME search QUERY LIMIT
    zroov-source-NAME fetch REF DESTDIR

The helper writes one JSON object per line on stdout and logs to stderr.

**resolve** ends with one of:

    {"type":"stream","url":"https://... or /path","via":"fast","hold":false,
     "headers":{"Name":"value"},"title":"","artist":"","duration":0,"expires":1762000000}
    {"type":"error","reason":"text"}

`url` is anything libmpv opens: an address, or a file path that may still be
growing. **`hold: true`** means the helper is serving that stream: it keeps running
and the engine sends it SIGTERM when the track is left. A torrent helper should
hold, and answer as soon as enough of the first pieces are there, exposing the
file over a local HTTP server with range support, or as a growing file. It may send
`{"type":"status","text":"12 peers"}` lines before the answer; these reach the UI
as `{"ev":"status","uri":"...","text":"..."}`.

If playback of a stream fails with `via` of `fast`, the engine calls
`resolve` again once with `ZROOV_NO_FAST=1` in the environment. A helper with no
fast path ignores this.

**search** writes `{"type":"result", Track fields...}` lines, then `{"type":"done"}`.

**fetch** writes `{"type":"progress","percent":n}` lines, then
`{"type":"saved","path":"DESTDIR/REF.ext"}` or an error line.

To add a source: drop an executable in one of those directories. Nothing in the
engine changes. `sources/` has `zroov-source-yt` as the worked example.

## Offline cache

`~/.local/share/ztos/music/`, the old app's directory, so its downloads still play:
`REF.{flac,m4a,webm,opus,mp3,ogg}`, with `REF.json` beside it holding the track. A
URI whose REF has a copy there plays from it with `via` of `cache`, without
running a helper. `/` in a REF is stored as `_`.

## MPRIS and the tile

`org.mpris.MediaPlayer2.zroovmusic` on the session bus, held while the engine runs.
The tile signal is the file `$XDG_RUNTIME_DIR/ztos-live/zroovmusic`
(`ztmetro/TILE-SIGNALS.md`), so the app's tile command must end in `zroovmusic`.
It changes with track and state, and `progress` moves once per 15 s only while
playing. It is deleted when the engine stops.

## Audio

Everything goes to PipeWire through libmpv's `ao=pipewire`, as a stream named
ZroovMusic. libmpv is loaded only while something plays or is paused; 120 s after a
pause it is freed, and `resume` reopens the track where it was.

## Additions for helpers

A `resolve` run with `ZROOV_PREFETCH=1` in the environment is ahead of time, for
gapless: answer only if it is cheap, else send an error. The engine drops a
prefetched answer that has `hold` or `headers`. A stream line may carry `"bits"`
(say, 24), since the engine can only read FLAC's bit depth from a local path.

`ZROOV_VOLUME` sets the starting volume, for tests.

## Measured on the S9 (1 Nov 2026, real network, stream volume low)

Tap here means the socket or function call, not a finger on a UI; first audio
means the first non-silent sample on the ztos-speaker monitor. Same search, same
track, runs alternated, a fresh process each run.

| arm                                        | median  | worst   |
|--------------------------------------------|---------|---------|
| old `apps/music` as shipped                | no audio in 5 of 5 (45 s limit) | |
| old, test copy changed to play format 18   | 4.06 s  | 5.24 s  |
| zroovd, fast path                          | 1.64 s  | 2.04 s  |
| zroovd, offline copy (engine's own figure) | 0.16 s  | one run |
| zroovd, yt-dlp fallback (engine's own figure) | 9.9 s | one run |

| state                       | CPU, % of one core | context switches/s |
|-----------------------------|--------|-------|
| stopped, client connected   | 0.03   | 0.0   |
| paused, libmpv loaded       | 0.00   | 0.1   |
| playing a stream            | 8.4    | 257   |
| playing local FLAC 24/48    | 6.2    | 264   |
| old stack playing, with cava | 22.9  | 625   |

## Known limits

**FLAC above 48 kHz reaches the speaker resampled.** The engine hands PipeWire the
file's own format (24/96 measured as S32LE 96000 on the stream), but the phone's
graph has `clock.allowed-rates = [ 48000 ]`, so PipeWire converts to 48 kHz after
that, as it does for 44.1 kHz. 24/48 is not rate converted. Changing this is a
system audio decision, outside the engine.

**yt-dlp's `bestaudio` is dead on this phone.** With no JS runtime, its links answer
403 past the first megabyte. The fallback asks for format 18, about 96 kb/s AAC.
