# Superscan: downloads don't work, no history, no cache
Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Build on the phone in /home/zerone/ztmetro-phase2 (push only the files you
change: browser/*), deploy /usr/local/bin/superscan with a backup. Do not touch ztmetro.c/perfd.
KEEP the orchestrator's new scrolling (webview.c snapshot scrolling: snap_*, scroll_script,
webview_overscroll, superscan.c touch/fling/rubber band) working and unchanged in behaviour.
1. Downloads: user says the browser can't download. Find why (download-started on the right
   WebKitNetworkSession? decide-destination; policy decision for responses that can't be shown
   (webkit_policy_decision_download / response "Content-Disposition: attachment", APK MIME);
   uptodown's download button flow; DOWNLOADS.md says the callback never ran). Make downloads
   work end to end: progress UI, ~/Downloads, notification, APK hands to the install chooser.
2. History: persistent browsing history (~/.local/share/superscan/history, title+URL+time), a
   history page in Superscan's Metro UI (open, delete one, clear all), address-bar suggestions
   from it.
3. Cache: the freeze fix set WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER (no memory cache) — use
   WEB_BROWSER with a persistent disk cache in ~/.cache/superscan (size-bounded, e.g. 150 MB)
   and keep the memory-pressure limits from ztmetro/FREEZE.md. Cookies/local storage persist.
Verify on the phone with screenshots: download an APK from uptodown.com and a PDF, see progress,
notification, open; history list; second visit loads from cache (log network/cached).
Write ztmetro/browser/BROWSER-FIX.md.
