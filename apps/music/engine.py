"""High-performance audio engine with buffered streaming, non-blocking mpv IPC,
Cava playback-monitor visualizer, and stream URL pre-fetching.
"""
import os
import sys
import math
import time
import json
import socket
import tempfile
import threading
import subprocess
from typing import Optional, List, Dict
from PySide6.QtCore import QObject, Signal, QTimer, QThread

from data import (
    ensure_music_dir,
    get_local_file,
    is_saved,
    save_track_meta,
    add_to_history,
)

SOCKET_PATH = "/tmp/ztos-music-mpv.sock"
SOCKET_PATH_B = "/tmp/ztos-music-mpv-b.sock"
SOUND_CONFIG = os.path.expanduser("~/.config/ztmetro/sound.json")
CAVA_BARS = 24

MPV_ARGS = [
    "--idle=yes",
    "--no-video",
    "--ao=pipewire,pulse,alsa",
    "--demuxer-readahead-secs=120",
    "--demuxer-max-bytes=64M",
    "--demuxer-max-back-bytes=32M",
    "--cache=yes",
    "--cache-secs=120",
    "--force-seekable=yes",
    "--ytdl-format=bestaudio/best",
]


def crossfade_setting():
    """(enabled, seconds) from the Sound app's file; (False, 0) when absent."""
    try:
        with open(SOUND_CONFIG) as f:
            cfg = json.load(f)
        return bool(cfg.get("crossfade", False)), max(1.0, min(12.0, float(cfg.get("crossfade_seconds", 6.0))))
    except (OSError, ValueError, TypeError):
        return False, 0.0


class Deck:
    """One mpv process with its JSON IPC socket. The engine plays on one deck;
    while a crossfade runs the next track plays on the other, and they swap."""

    def __init__(self, engine, path, name):
        self.engine = engine
        self.path = path
        self.name = name
        self.proc: Optional[subprocess.Popen] = None
        self.sock: Optional[socket.socket] = None
        self.lock = threading.Lock()
        self.running = False
        self.position = 0.0
        self.duration = 0.0

    def start(self, kill_others=False):
        if kill_others:
            try:
                subprocess.run(["killall", "-q", "mpv"], check=False)
                time.sleep(0.05)
            except Exception:
                pass
        if os.path.exists(self.path):
            try:
                os.unlink(self.path)
            except OSError:
                pass
        args = [a for a in MPV_ARGS if not (os.environ.get("ZTOS_MUSIC_AO") and a.startswith("--ao="))]
        if os.environ.get("ZTOS_MUSIC_AO"):
            args.append("--ao=" + os.environ["ZTOS_MUSIC_AO"])    # tests: ZTOS_MUSIC_AO=null
        cmd = ["mpv", f"--input-ipc-server={self.path}"] + args
        try:
            self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception as exc:
            print("Failed to start mpv:", exc)
            return False
        for _ in range(25):
            if os.path.exists(self.path):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(self.path)
                    self.sock = s
                    break
                except (OSError, socket.error):
                    time.sleep(0.08)
            time.sleep(0.08)
        if not self.sock:
            return False
        for i, prop in enumerate(["time-pos", "duration", "pause", "demuxer-cache-duration", "idle-active"], 1):
            self.send(["observe_property", i, prop])
        self.running = True
        threading.Thread(target=self._reader, daemon=True).start()
        return True

    def alive(self):
        return self.sock is not None and self.proc is not None and self.proc.poll() is None

    def send(self, command: list):
        with self.lock:
            if not self.sock:
                return
            try:
                self.sock.sendall((json.dumps({"command": command}) + "\n").encode())
            except Exception:
                pass

    def _reader(self):
        sock_file = self.sock.makefile("r", buffering=1, errors="ignore")
        while self.running and self.sock:
            try:
                line = sock_file.readline()
                if not line:
                    break
                line = line.strip()
                if line:
                    self.engine._deck_event(self, json.loads(line))
            except Exception:
                pass

    def close(self):
        self.running = False
        if self.sock:
            try:
                self.send(["quit"])
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if self.proc:
            try:
                self.proc.terminate()
            except OSError:
                pass


class CavaVisualizerListener(threading.Thread):
    """Passively monitor existing playback without keeping the speaker sink active."""

    def __init__(self, callback):
        super().__init__(daemon=True)
        self.callback = callback
        self.running = True
        self._proc: Optional[subprocess.Popen] = None
        self._cfg_path: Optional[str] = None

    def run(self):
        conf = f"""
[general]
bars = {CAVA_BARS}
framerate = 25

[input]
# Cava defaults to always processing, which drives the S9 speaker sink even
# when music is paused. Only observe playback that is already running.
method = pipewire
source = auto
active = 0
sample_rate = 48000

[output]
method = raw
raw_target = /dev/stdout
data_format = binary
bit_format = 8bit
"""
        try:
            with tempfile.NamedTemporaryFile("w", delete=False) as f:
                f.write(conf)
                self._cfg_path = f.name

            self._proc = subprocess.Popen(
                ["cava", "-p", self._cfg_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                # Override Cava's hard-coded 512-frame latency request. The
                # S9 playback path needs the usual 2048-frame music buffer.
                env={**os.environ, "PIPEWIRE_LATENCY": "2048/48000"},
            )
            frame_size = CAVA_BARS
            pending = bytearray()
            while self.running and self._proc.poll() is None:
                raw = self._proc.stdout.read1(frame_size * 4)
                if not raw:
                    time.sleep(0.01)
                    continue
                pending.extend(raw)
                while len(pending) >= frame_size:
                    frame = pending[:frame_size]
                    del pending[:frame_size]
                    # Normalize 0..255 to 0.0..1.0
                    values = [b / 255.0 for b in frame]
                    self.callback(values)
        except Exception:
            pass
        finally:
            self.stop()

    def stop(self):
        self.running = False
        if self._proc:
            try:
                self._proc.terminate()
            except Exception:
                pass
            self._proc = None
        if self._cfg_path and os.path.exists(self._cfg_path):
            try:
                os.unlink(self._cfg_path)
            except Exception:
                pass
            self._cfg_path = None


class SearchWorker(QThread):
    results_ready = Signal(list)
    search_failed = Signal(str)

    def __init__(self, query: str, parent=None):
        super().__init__(parent)
        self.query = query

    def run(self):
        try:
            import yt_dlp
            opts = {
                "quiet": True,
                "extract_flat": True,
                "skip_download": True,
                "no_warnings": True,
            }
            with yt_dlp.YoutubeDL(opts) as ydl:
                res = ydl.extract_info(f"ytsearch8:{self.query}", download=False)
                entries = res.get("entries", [])
                results = []
                for e in entries:
                    if not e:
                        continue
                    results.append({
                        "id": e.get("id"),
                        "title": e.get("title") or "Unknown Title",
                        "artist": e.get("uploader") or e.get("channel") or "Unknown Artist",
                        "duration": float(e.get("duration") or 0.0),
                        "category": "search",
                    })
                self.results_ready.emit(results)
        except Exception as exc:
            self.search_failed.emit(str(exc))


class DownloadWorker(QThread):
    progress = Signal(str, float)
    finished = Signal(dict, str)
    failed = Signal(str, str)

    def __init__(self, track: dict, parent=None):
        super().__init__(parent)
        self.track = track

    def run(self):
        track_id = self.track.get("id", "")
        if not track_id:
            self.failed.emit("", "Invalid track ID")
            return

        dest_dir = ensure_music_dir()
        out_tmpl = os.path.join(dest_dir, f"{track_id}.%(ext)s")

        def hook(d):
            if d.get("status") == "downloading":
                total = d.get("total_bytes") or d.get("total_bytes_estimate") or 0
                downloaded = d.get("downloaded_bytes") or 0
                if total > 0:
                    pct = (downloaded / total) * 100.0
                    self.progress.emit(track_id, pct)
            elif d.get("status") == "finished":
                self.progress.emit(track_id, 100.0)

        try:
            import yt_dlp
            opts = {
                "format": "bestaudio/best",
                "outtmpl": out_tmpl,
                "quiet": True,
                "no_warnings": True,
                "progress_hooks": [hook],
            }
            with yt_dlp.YoutubeDL(opts) as ydl:
                ydl.download([f"https://www.youtube.com/watch?v={track_id}"])

            fpath = get_local_file(track_id)
            if fpath:
                save_track_meta(self.track, fpath)
                self.finished.emit(self.track, fpath)
            else:
                self.failed.emit(track_id, "File not found after download")
        except Exception as exc:
            self.failed.emit(track_id, str(exc))


class StreamResolverWorker(QThread):
    resolved = Signal(dict, str)     # track, stream_url
    failed = Signal(dict, str)

    def __init__(self, track: dict, parent=None):
        super().__init__(parent)
        self.track = track

    def run(self):
        tid = self.track.get("id")
        try:
            import yt_dlp
            opts = {
                "format": "bestaudio/best",
                "quiet": True,
                "no_warnings": True,
                "skip_download": True,
            }
            with yt_dlp.YoutubeDL(opts) as ydl:
                info = ydl.extract_info(f"https://www.youtube.com/watch?v={tid}", download=False)
                url = info.get("url")
                if url:
                    self.resolved.emit(self.track, url)
                else:
                    self.failed.emit(self.track, "No stream URL found")
        except Exception as exc:
            self.failed.emit(self.track, str(exc))


class PlayerEngine(QObject):
    position_changed = Signal(float, float)      # (pos, duration)
    buffered_changed = Signal(float)             # buffered seconds
    state_changed = Signal(bool)                 # is_playing
    track_changed = Signal(dict)                 # current track dict
    queue_changed = Signal()
    buffering_changed = Signal(bool)             # is_buffering
    amplitudes_updated = Signal(list)            # real-time FFT bars

    def __init__(self, parent=None):
        super().__init__(parent)
        self.deck_a = Deck(self, SOCKET_PATH, "A")
        self.deck_b = Deck(self, SOCKET_PATH_B, "B")
        self._active = self.deck_a
        self._fade_gen = 0               # bumped to cancel a crossfade in progress
        self._fading = False
        self._fade_checked = 0.0
        self._cf_cache = (0.0, False, 0.0)

        self.current_track: Optional[dict] = None
        self.queue: List[dict] = []
        self.queue_index: int = 0
        self.is_playing: bool = False
        self.is_buffering: bool = False
        self.position: float = 0.0
        self.duration: float = 0.0
        self.buffered: float = 0.0
        self.repeat_mode: str = "all"

        self._active_downloads: Dict[str, DownloadWorker] = {}
        self._active_search: Optional[SearchWorker] = None
        self._active_resolver: Optional[StreamResolverWorker] = None
        self._url_cache: Dict[str, tuple[str, float]] = {}  # id -> (url, expire_time)

        self._start_mpv()
        self._start_cava()

    def _start_mpv(self):
        """Starts deck A (the only mpv, unless a crossfade is in use)."""
        self.deck_a.start(kill_others=True)

    def _other(self):
        return self.deck_b if self._active is self.deck_a else self.deck_a

    def _deck_event(self, deck, msg):
        """An event from a deck's mpv. Only the deck the engine plays on drives the UI."""
        event = msg.get("event")
        if event == "property-change":
            name, val = msg.get("name"), msg.get("data")
            if name == "time-pos" and val is not None:
                deck.position = float(val)
            elif name == "duration" and val is not None:
                deck.duration = float(val)
            if deck is not self._active:
                return
            if name == "time-pos" and val is not None:
                self.position = float(val)
                self.position_changed.emit(self.position, self.duration)
                self._maybe_crossfade()
            elif name == "duration" and val is not None:
                self.duration = float(val)
                self.position_changed.emit(self.position, self.duration)
            elif name == "pause":
                playing = not bool(val)
                if self.is_playing != playing:
                    self.is_playing = playing
                    self.state_changed.emit(self.is_playing)
            elif name == "demuxer-cache-duration" and val is not None:
                self.buffered = self.position + float(val)
                self.buffered_changed.emit(self.buffered)
            elif name == "idle-active":
                if val is True and self.duration > 0 and self.position >= (self.duration - 2.0) \
                        and not self._fading:
                    self._handle_track_finished()
        elif event == "playback-restart" and deck is self._active:
            self.is_buffering = False
            self.buffering_changed.emit(False)
            self.is_playing = True
            self.state_changed.emit(True)

    def _send_raw(self, command: list):
        self._active.send(command)

    # ---- crossfade -------------------------------------------------------
    # The Sound app keeps "crossfade" and "crossfade_seconds" in
    # ~/.config/ztmetro/sound.json. Off: a track ends and the next is loaded on
    # the same mpv, as before. On: a second mpv (deck B) starts the next track a
    # few seconds before the end, volumes cross on an equal-power curve, and the
    # decks swap roles.
    def _crossfade_cfg(self):
        now = time.time()
        if now - self._cf_cache[0] > 2.0:
            en, secs = crossfade_setting()
            self._cf_cache = (now, en, secs)
        return self._cf_cache[1], self._cf_cache[2]

    def _next_track(self):
        if not self.queue or self.repeat_mode == "one":
            return None
        idx = self.queue_index + 1
        if idx >= len(self.queue):
            if self.repeat_mode != "all" or len(self.queue) < 2:
                return None
            idx = 0
        return idx, self.queue[idx]

    def _source_for(self, track):
        """A local file or a cached stream URL; None if it would have to be resolved."""
        tid = track.get("id", "")
        local = get_local_file(tid)
        if local:
            return local
        cached, expire = self._url_cache.get(tid, ("", 0))
        if cached and time.time() < expire:
            return cached
        return None

    def _maybe_crossfade(self):
        if self._fading or not self.is_playing or self.duration <= 0:
            return
        enabled, secs = self._crossfade_cfg()
        if not enabled:
            return
        secs = min(secs, self.duration / 3.0)
        remaining = self.duration - self.position
        if remaining > secs or remaining < 0.3:
            return
        nxt = self._next_track()
        if not nxt:
            return
        idx, track = nxt
        source = self._source_for(track)
        if not source:
            return                       # not ready: the ordinary path plays it
        self._start_crossfade(idx, track, source, min(secs, remaining))

    def _start_crossfade(self, idx, track, source, secs):
        other = self._other()
        if not other.alive():
            if not other.start():
                return
        self._fading = True
        self._fade_gen += 1
        gen = self._fade_gen
        old = self._active
        other.send(["set_property", "volume", 0])
        other.send(["loadfile", source, "replace"])
        other.send(["set_property", "pause", False])
        # the engine moves to the next track now; the UI shows it
        self._active = other
        self.queue_index = idx
        self.current_track = track
        self.position = 0.0
        self.duration = float(track.get("duration") or 0.0)
        self.buffered = 0.0
        self.track_changed.emit(track)
        self.position_changed.emit(0.0, self.duration)
        add_to_history(track)
        self.is_playing = True
        self.state_changed.emit(True)
        track["is_saved"] = bool(get_local_file(track.get("id", "")))
        self._prefetch_next()

        def ramp():
            steps = max(5, int(secs * 10))
            for i in range(1, steps + 1):
                if gen != self._fade_gen:
                    return
                t = i / steps
                old.send(["set_property", "volume", round(100 * math.cos(t * math.pi / 2), 1)])
                other.send(["set_property", "volume", round(100 * math.sin(t * math.pi / 2), 1)])
                time.sleep(secs / steps)
            if gen == self._fade_gen:
                self._end_fade(old)

        threading.Thread(target=ramp, daemon=True).start()

    def _end_fade(self, old):
        old.send(["stop"])
        old.send(["set_property", "volume", 100])
        self._active.send(["set_property", "volume", 100])
        self._fading = False

    def _cancel_fade(self):
        """Another track was chosen (or a seek): finish the fade at once."""
        if not self._fading:
            return
        self._fade_gen += 1
        self._end_fade(self._other())

    def _start_cava(self):
        self._cava = CavaVisualizerListener(self._on_cava_amplitudes)
        self._cava.start()

    def _on_cava_amplitudes(self, values: list):
        self.amplitudes_updated.emit(values)

    def _handle_track_finished(self):
        if self.repeat_mode == "one":
            self.replay()
        elif self.repeat_mode == "all":
            self.next()
        else:
            self.is_playing = False
            self.state_changed.emit(False)

    def play_track(self, track: dict, playlist: Optional[List[dict]] = None):
        """Loads and begins playing a track with buffered streaming."""
        if playlist is not None:
            self.queue = list(playlist)
            try:
                self.queue_index = next(
                    i for i, t in enumerate(self.queue) if t.get("id") == track.get("id")
                )
            except StopIteration:
                self.queue.insert(0, track)
                self.queue_index = 0
            self.queue_changed.emit()

        self._cancel_fade()
        self.current_track = track
        self.position = 0.0
        self.duration = float(track.get("duration") or 0.0)
        self.buffered = 0.0
        self.track_changed.emit(track)
        self.position_changed.emit(0.0, self.duration)
        add_to_history(track)

        # 1. Offline file
        local = get_local_file(track.get("id", ""))
        if local:
            track["is_saved"] = True
            track["local_path"] = local
            self._load_source(local)
            self._prefetch_next()
            return

        track["is_saved"] = False

        # 2. Check cached direct stream URL
        cached_url, expire = self._url_cache.get(track.get("id", ""), ("", 0))
        if cached_url and time.time() < expire:
            self._load_source(cached_url)
            self._prefetch_next()
            return

        # 3. Resolve stream URL in background, or fall back to ytdl stream
        self.is_buffering = True
        self.buffering_changed.emit(True)

        def on_resolved(tr, stream_url):
            if self.current_track and self.current_track.get("id") == tr.get("id"):
                self._url_cache[tr["id"]] = (stream_url, time.time() + 14400)
                self._load_source(stream_url)
                self._prefetch_next()

        def on_failed(tr, err):
            if self.current_track and self.current_track.get("id") == tr.get("id"):
                # Fallback to direct mpv ytdl hook
                source = f"https://www.youtube.com/watch?v={tr.get('id')}"
                self._load_source(source)

        if self._active_resolver and self._active_resolver.isRunning():
            self._active_resolver.terminate()
            self._active_resolver.wait()

        self._active_resolver = StreamResolverWorker(track, self)
        self._active_resolver.resolved.connect(on_resolved)
        self._active_resolver.failed.connect(on_failed)
        self._active_resolver.start()

    def _load_source(self, source: str):
        self._send_raw(["loadfile", source, "replace"])
        self._send_raw(["set_property", "pause", False])
        self.is_playing = True
        self.state_changed.emit(True)

    def _prefetch_next(self):
        """Pre-fetches stream URL for the upcoming track in the queue."""
        if not self.queue or len(self.queue) <= 1:
            return
        next_idx = (self.queue_index + 1) % len(self.queue)
        nxt = self.queue[next_idx]
        nid = nxt.get("id")
        if not nid or is_saved(nid) or nid in self._url_cache:
            return

        def _do_prefetch():
            try:
                import yt_dlp
                opts = {"format": "bestaudio/best", "quiet": True, "no_warnings": True, "skip_download": True}
                with yt_dlp.YoutubeDL(opts) as ydl:
                    info = ydl.extract_info(f"https://www.youtube.com/watch?v={nid}", download=False)
                    u = info.get("url")
                    if u:
                        self._url_cache[nid] = (u, time.time() + 14400)
            except Exception:
                pass

        threading.Thread(target=_do_prefetch, daemon=True).start()

    def toggle_pause(self):
        if not self.current_track:
            if self.queue:
                self.play_track(self.queue[0])
            return
        self._send_raw(["cycle", "pause"])
        if self._fading:
            self._other().send(["cycle", "pause"])

    def seek(self, seconds: float):
        if not self.current_track:
            return
        seconds = max(0.0, float(seconds))
        self._cancel_fade()
        self.position = seconds
        # Streaming sources can take a long time to rebuild their precise
        # decoder position after an HR seek. A keyframe seek is immediate and
        # avoids provoking long audio graph reconfiguration on the phone.
        self._send_raw(["seek", seconds, "absolute+keyframes"])
        self.position_changed.emit(self.position, self.duration)

    def replay(self):
        if self.current_track:
            self.seek(0.0)
            self._send_raw(["set_property", "pause", False])
            self.is_playing = True
            self.state_changed.emit(True)

    def next(self):
        if not self.queue:
            return
        self.queue_index = (self.queue_index + 1) % len(self.queue)
        self.play_track(self.queue[self.queue_index])

    def prev(self):
        if self.position > 3.0:
            self.replay()
            return
        if not self.queue:
            return
        self.queue_index = (self.queue_index - 1) % len(self.queue)
        self.play_track(self.queue[self.queue_index])

    def cycle_repeat(self) -> str:
        modes = ["all", "one", "off"]
        idx = (modes.index(self.repeat_mode) + 1) % len(modes)
        self.repeat_mode = modes[idx]
        return self.repeat_mode

    def download_track(self, track: dict, on_progress=None, on_finished=None, on_failed=None):
        tid = track.get("id")
        if not tid or tid in self._active_downloads:
            return

        worker = DownloadWorker(track, self)
        if on_progress:
            worker.progress.connect(on_progress)
        if on_finished:
            worker.finished.connect(on_finished)
        if on_failed:
            worker.failed.connect(on_failed)

        def _cleanup(*_):
            self._active_downloads.pop(tid, None)

        worker.finished.connect(_cleanup)
        worker.failed.connect(_cleanup)

        self._active_downloads[tid] = worker
        worker.start()

    def search(self, query: str, on_results=None, on_error=None):
        if self._active_search and self._active_search.isRunning():
            self._active_search.terminate()
            self._active_search.wait()

        worker = SearchWorker(query, self)
        if on_results:
            worker.results_ready.connect(on_results)
        if on_error:
            worker.search_failed.connect(on_error)

        self._active_search = worker
        worker.start()

    def close(self):
        if self._cava:
            self._cava.stop()
        self._fade_gen += 1
        self.deck_a.close()
        self.deck_b.close()
