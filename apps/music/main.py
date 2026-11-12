"""ztOS Music Streaming & Offline Player.
Built with ztOS SDK, yt-dlp, and mpv.
"""
import atexit
import os
from typing import List, Optional
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QLabel, QPushButton

import ztos
from engine import PlayerEngine
from player_view import NowPlayingPage, GLYPH_PLAY, GLYPH_PAUSE, GLYPH_NEXT, GLYPH_REPEAT, GLYPH_MUSIC, GLYPH_REPLAY, GLYPH_SAVE
from data import (
    CURATED_TRACKS,
    get_saved_tracks,
    remove_saved_track,
    get_history,
    is_saved,
)

app = ztos.App()
engine = PlayerEngine()


class DynamicList(QWidget):
    """Container for dynamically updated rows inside a ztos.Page."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(0, 0, 0, 0)
        self._layout.setSpacing(0)

    def clear(self):
        while self._layout.count():
            item = self._layout.takeAt(0)
            w = item.widget()
            if w:
                w.deleteLater()

    def add(self, widget: QWidget):
        self._layout.addWidget(widget)


def fmt_dur(seconds: float) -> str:
    s = int(max(0.0, seconds))
    m, s = divmod(s, 60)
    return f"{m}:{s:02d}"


class MusicHome(ztos.Pivot):
    """Home screen: discover curated genres, offline saved tracks, search & recent history."""

    def __init__(self):
        self.engine = engine

        # ---- Tab 1: Discover ----
        self.p_discover = ztos.Page("discover")
        self.current_category = "top"
        self._setup_discover_page()

        # ---- Tab 2: Saved (Offline) ----
        self.p_saved = ztos.Page("saved")
        self._setup_saved_page()

        # ---- Tab 3: Search ----
        self.p_search = ztos.Page("search")
        self._setup_search_page()

        # ---- Tab 4: Recent (Replay) ----
        self.p_recent = ztos.Page("recent")
        self._setup_recent_page()

        super().__init__([self.p_discover, self.p_saved, self.p_search, self.p_recent])

        # Bottom player control bar
        self.btn_now = ztos.RoundButton(GLYPH_MUSIC, "playing", self._open_now_playing)
        self.btn_play = ztos.RoundButton(GLYPH_PLAY, "play", self.engine.toggle_pause)
        self.btn_next = ztos.RoundButton(GLYPH_NEXT, "next", self.engine.next)
        self.btn_repeat = ztos.RoundButton(GLYPH_REPEAT, "repeat", self._cycle_repeat)

        self.bar(self.btn_now, self.btn_play, self.btn_next, self.btn_repeat)

        # Connect engine signals
        self.engine.state_changed.connect(self._on_state_changed)
        self.engine.track_changed.connect(self._on_track_changed)
        self.turned.connect(self._on_tab_switched)

    def _setup_discover_page(self):
        p = self.p_discover
        p.section("FEATURED CHANNELS")

        # Category chips
        cat_box = QWidget()
        cat_layout = QHBoxLayout(cat_box)
        cat_layout.setContentsMargins(0, 4, 0, 8)
        cat_layout.setSpacing(10)

        self.cat_buttons = {}
        cats = [("top", "Top 50"), ("pop", "Pop"), ("hiphop", "Hip-Hop"), ("rnb", "R&&B / Soul")]
        for key, label in cats:
            btn = QPushButton(label)
            btn.setCheckable(True)
            btn.setChecked(key == self.current_category)
            btn.setStyleSheet(
                "QPushButton { background: #262626; color: #d0d0d0; border: none; padding: 7px 14px; font-size: 13px; font-weight: bold; border-radius: 4px; }"
                "QPushButton:checked { background: #0078d7; color: white; }"
            )
            btn.clicked.connect(lambda _, k=key: self._switch_category(k))
            cat_layout.addWidget(btn)
            self.cat_buttons[key] = btn
        cat_layout.addStretch(1)
        p.add(cat_box)

        self.discover_list = p.add(DynamicList())
        self._populate_category_tracks()

    def _switch_category(self, cat: str):
        ztos.haptic("tick")
        self.current_category = cat
        for k, b in self.cat_buttons.items():
            b.setChecked(k == cat)
        self._populate_category_tracks()

    def _populate_category_tracks(self):
        self.discover_list.clear()
        tracks = CURATED_TRACKS.get(self.current_category, [])
        for t in tracks:
            saved_tag = " • Saved" if is_saved(t.get("id")) else ""
            detail = f"{t.get('artist')} • {fmt_dur(t.get('duration', 0))}{saved_tag}"
            entry = ztos.Entry(
                t.get("title"),
                detail,
                on_tap=lambda tr=t: self._play_and_open(tr, tracks),
                on_hold=lambda tr=t, e=None: self._on_track_hold(tr, e),
            )
            self.discover_list.add(entry)

    def _setup_saved_page(self):
        p = self.p_saved
        p.section("OFFLINE LIBRARY")

        # Action bar with Play All and Replay
        self.btn_saved_play_all = ztos.RoundButton(GLYPH_PLAY, "play all", self._play_all_saved)
        self.btn_saved_replay = ztos.RoundButton(GLYPH_REPLAY, "replay", self.engine.replay)
        p.round_buttons(self.btn_saved_play_all, self.btn_saved_replay)
        p.space(12)

        self.saved_list = p.add(DynamicList())
        self._refresh_saved_list()

    def _refresh_saved_list(self):
        self.saved_list.clear()
        tracks = get_saved_tracks()
        if not tracks:
            lbl = QLabel("No saved offline songs yet.\n\nHold any track in Discover or Search and select 'Save Offline' to listen without network.")
            lbl.setStyleSheet("color: #888888; font-size: 14px; padding: 20px 0;")
            lbl.setWordWrap(True)
            self.saved_list.add(lbl)
            return

        for t in tracks:
            detail = f"{t.get('artist')} • {fmt_dur(t.get('duration', 0))} • Offline"
            entry = ztos.Entry(
                t.get("title"),
                detail,
                on_tap=lambda tr=t: self._play_and_open(tr, tracks),
                on_hold=lambda tr=t: self._on_saved_track_hold(tr),
            )
            self.saved_list.add(entry)

    def _play_all_saved(self):
        tracks = get_saved_tracks()
        if tracks:
            ztos.haptic("tap")
            self._play_and_open(tracks[0], tracks)
        else:
            ztos.notify("Music", "No offline songs saved yet")

    def _setup_search_page(self):
        p = self.p_search
        p.section("ONLINE SEARCH")
        self.search_box = p.textbox("Search YouTube Music", placeholder="Artist, track, or lyrics...",
                                    on_submit=self._do_search)
        p.space(12)
        self.search_status = p.text("Type a query above to search", size=13, color=ztos.GREY)
        self.search_list = p.add(DynamicList())

    def _do_search(self, query: str):
        query = query.strip()
        if not query:
            return
        ztos.haptic("tap")
        self.p_search.busy(True)
        self.search_status.setText(f"Searching for '{query}'...")
        self.search_list.clear()
        self.engine.search(query, on_results=self._on_search_results, on_error=self._on_search_error)

    def _on_search_results(self, results: list):
        self.p_search.busy(False)
        self.search_list.clear()
        if not results:
            self.search_status.setText("No results found")
            return
        self.search_status.setText(f"{len(results)} songs found:")
        for t in results:
            saved_tag = " • Saved" if is_saved(t.get("id")) else ""
            dur_str = f" • {fmt_dur(t.get('duration', 0))}" if t.get('duration') else ""
            detail = f"{t.get('artist')}{dur_str}{saved_tag}"
            entry = ztos.Entry(
                t.get("title"),
                detail,
                on_tap=lambda tr=t: self._play_and_open(tr, results),
                on_hold=lambda tr=t: self._on_track_hold(tr),
            )
            self.search_list.add(entry)

    def _on_search_error(self, err: str):
        self.p_search.busy(False)
        self.search_status.setText(f"Search failed: {err[:60]}")

    def _setup_recent_page(self):
        p = self.p_recent
        p.section("RECENTLY PLAYED")
        self.recent_list = p.add(DynamicList())
        self._refresh_recent_list()

    def _refresh_recent_list(self):
        self.recent_list.clear()
        hist = get_history()
        if not hist:
            lbl = QLabel("No playback history yet.\n\nTracks you listen to will appear here for instant replay.")
            lbl.setStyleSheet("color: #888888; font-size: 14px; padding: 20px 0;")
            lbl.setWordWrap(True)
            self.recent_list.add(lbl)
            return

        for t in hist:
            saved_tag = " • Saved" if is_saved(t.get("id")) else ""
            dur_str = f" • {fmt_dur(t.get('duration', 0))}" if t.get('duration') else ""
            detail = f"{t.get('artist')}{dur_str}{saved_tag}"
            entry = ztos.Entry(
                t.get("title"),
                detail,
                on_tap=lambda tr=t: self._play_and_open(tr, hist),
                on_hold=lambda tr=t: self._on_track_hold(tr),
            )
            self.recent_list.add(entry)

    def _play_and_open(self, track: dict, playlist: List[dict]):
        ztos.haptic("tap")
        self.engine.play_track(track, playlist)
        app.push(NowPlayingPage(self.engine))

    def _open_now_playing(self):
        ztos.haptic("tap")
        app.push(NowPlayingPage(self.engine))

    def _cycle_repeat(self):
        ztos.haptic("tap")
        mode = self.engine.cycle_repeat()
        self.btn_repeat.set_label(f"rep: {mode}")
        ztos.notify("Repeat", f"Mode set to {mode}")

    def _on_track_hold(self, track: dict, anchor=None):
        ztos.haptic("tick")
        tid = track.get("id")
        items = [
            ("Play Now", lambda: self.engine.play_track(track)),
            ("Replay", lambda: self.engine.play_track(track)),
        ]
        if not is_saved(tid):
            items.append(("Save Offline", lambda: self._download_track(track)))
        else:
            items.append(("Remove Offline Copy", lambda: self._remove_track(tid)))
        items.append(("Open Now Playing", self._open_now_playing))

        ztos.menu(anchor or self.p_discover, items)

    def _on_saved_track_hold(self, track: dict):
        ztos.haptic("tick")
        tid = track.get("id")
        items = [
            ("Play Now", lambda: self.engine.play_track(track)),
            ("Replay", lambda: self.engine.play_track(track)),
            ("Delete Offline File", lambda: self._remove_track(tid)),
            ("Open Now Playing", self._open_now_playing),
        ]
        ztos.menu(self.p_saved, items)

    def _download_track(self, track: dict):
        ztos.notify("Saving Track", track.get("title", "Song"))
        self.engine.download_track(
            track,
            on_finished=lambda tr, path: self._on_saved_done(tr),
            on_failed=lambda tid, err: ztos.notify("Download Failed", err[:50]),
        )

    def _on_saved_done(self, track: dict):
        ztos.haptic("tick")
        ztos.notify("Saved Offline", track.get("title", "Song"))
        self._refresh_saved_list()
        self._populate_category_tracks()

    def _remove_track(self, track_id: str):
        remove_saved_track(track_id)
        ztos.notify("Removed", "Deleted offline track")
        self._refresh_saved_list()
        self._populate_category_tracks()

    def _on_tab_switched(self, index: int):
        if index == 1:
            self._refresh_saved_list()
        elif index == 3:
            self._refresh_recent_list()

    def _on_state_changed(self, is_playing: bool):
        self.btn_play.set_glyph(GLYPH_PAUSE if is_playing else GLYPH_PLAY)
        self.btn_play.set_label("pause" if is_playing else "play")
        self._publish_tile()

    def _on_track_changed(self, track: dict):
        self._refresh_recent_list()
        self._publish_tile()

    def _publish_tile(self):
        """Now playing on this app's Start tile (a tile signal): the track and
        its artist on the back, a play or pause mark on the front. Sent when
        the track or the play state changes, never on a timer."""
        track = self.engine.current_track
        if not track:
            ztos.tile_clear()
            return
        playing = self.engine.is_playing
        ztos.tile_update(
            "Playing" if playing else "Paused",
            [track.get("title", ""), track.get("artist", "")],
            badge="play" if playing else "pause",
        )


# the tile goes back to its plain front when the app is closed
atexit.register(ztos.tile_clear)
app.run(MusicHome())
