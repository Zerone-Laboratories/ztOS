# Superscan: downloads manager

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Build on the phone in /home/zerone/ztmetro-phase2 (push only files
you change), deploy superscan with backup. Read ztmetro/browser/README.md, webview.c,
superscan.c, SCROLL.md (the previous agent just reworked scrolling — keep it working).
UI must match ztMETRO's Metro style (zmc helpers in ztmetro/core), GPU-drawn.

User: "there is no way to manage the downloads, no download progress or anything".
Deliver:
1. WebKit downloads (WebKitDownload: decide-destination, received-data, finished, failed):
   save to ~/Downloads with a non-clobbering name; progress (bytes, %, speed, ETA).
2. A downloads view in Superscan (Metro style: "downloads" pivot/page or entry in the bar's
   menu): list of current and past downloads (persist history in ~/.local/share/superscan/
   downloads.json), progress bar per item, cancel / retry / open / show in Files / remove from
   list. Opening an .apk hands it to the Files app's install chooser (ztOS runtime vs Waydroid,
   see ztmetro/files and `ztos-android install --runtime`).
3. Feedback while browsing: a small progress indicator in Superscan's bar while a download runs,
   and a ztMETRO notification (ztnotifyd / org.freedesktop.Notifications) on finish/failure with
   an action to open it.
4. Downloads continue while Superscan is in the background (tickle must not pause it mid-download:
   use the existing audio-style exemption mechanism or zrnperfd policy for the downloading
   process, released when done).
Verify on the phone: download a small file and an APK from uptodown.com, screenshots of progress,
list, notification; cancel and retry work. Write ztmetro/browser/DOWNLOADS.md.
