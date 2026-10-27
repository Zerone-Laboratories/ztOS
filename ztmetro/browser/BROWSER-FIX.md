# Superscan browser fixes — 2026-10-24

## Status

Partially verified on the phone. PDF download, progress, completion notification,
history persistence/list, and disk-cache reuse passed. Full task acceptance is
**not complete**: Uptodown returned HTTP 404, APK chooser verification failed,
and phone connection failures prevented the final rebuild and remaining checks.

Phone access used only `/tmp/ztos-phone-control.py run|sudo|put|get`. After two
connection timeouts were reported, no new phone access was initiated, following
BROWSER-FIX-TASK.md. An already-running APK fixture check subsequently finished
with an assertion that the foreground window was still Superscan, not Files.

## Changes

### Downloads

The installed WPE 2.54 GIR describes `WebKitDownload.destination` as a **local
path**. Existing code passed a `file://` URI. Superscan also lacked a response
policy handler. The earlier DOWNLOADS.md had explicitly recorded that the
network-session download callback was never reached.

- Use one persistent WebKitNetworkSession across tabs and retries. Connect
  `download-started` once to that session. Log actual and expected session IDs.
- Explicitly download attachment responses, APK MIME responses, and unsupported
  response MIME types. This includes frame navigations. User-initiated new-window
  navigation opens in the current view; unsolicited new windows are ignored.
- Pass a filesystem path to `webkit_download_set_destination`. Keep files in
  `~/Downloads`, sanitize directory components and dot names, and account for
  active downloads when choosing collision suffixes.
- Persist download metadata under `~/.local/share/superscan/downloads.json`.
  Import the earlier `~/Downloads/downloads.json` when no new metadata exists.
- Preserve failures when WebKit emits `finished` after `failed`. Rebind callback
  data when removing array entries, including while another download is active.
  Background/retry downloads update the UI without retaining a closed tab.
- Throttle progress redraws to 100 ms and metadata writes to one second.
  Completion/failure always save immediately. Send notifications asynchronously
  with a five-second D-Bus timeout. Only completed downloads offer Open.
- Completed-file Open continues to call Files. Its existing APK path owns the
  ATL/Waydroid chooser; no Files, installer, shell, or perfd changes were made.

### History

`~/.local/share/superscan/history` stores UTF-8 titles, URLs, and Unix timestamps
using GLib key-file escaping and atomic replacement. Successful committed HTTP(S)
visits update a recent-first list, bounded at 1,000 distinct URLs. Download
interruptions and failed page loads do not become successful history visits.

The Metro menu now includes history. Its page supports opening an entry,
deleting one, clearing all, and paging through older entries. Address editing
matches titles and URLs without case and displays up to four suggestions.
The final source anchors suggestions above the address bar so the keyboard's
whole-window lift does not hide them. Choosing another menu page dismisses the
address keyboard. These last two UI adjustments remain unbuilt on the phone.

### Cache and memory

Use `WEBKIT_CACHE_MODEL_WEB_BROWSER` and one persistent network session with
HTTP cache at `~/.cache/superscan` (or the XDG cache directory override). Cookie
SQLite, local storage, and other site data remain in the existing data directory.

The public installed API exposes no disk-capacity setter. Fetch HTTP disk-cache
sizes at startup and every 15 seconds; clear **only HTTP disk cache** when its
reported total exceeds 150 MiB. This is a periodic eviction threshold, not a hard
filesystem quota: writes can temporarily exceed it between checks. Cookies and
local storage are not cleared. The threshold/eviction path was not stress-tested.

The FREEZE.md WebKit pressure values remain 384 MB, conservative 0.5, strict
0.75, kill 0, poll two seconds. Snapshot scrolling, `scroll_script`, overscroll,
fling, and spring calculations were retained. The touch router gives suggestion
hits to Metro while editing. No new scrolling benchmark was performed.

`ZTB_NETWORK_LOG=1` logs Navigation/Resource Timing values at load completion.
Cross-origin entries without timing permission do not establish cache hits.

## Phone build and deployment

Build directory: `/home/zerone/ztmetro-phase2`. Only changed `browser/*` source
and verification files were pushed. Build command:

```sh
make browser/superscan
```

Several phone builds passed with no reported compiler warnings. The binary was
staged and renamed into `/usr/local/bin/superscan`. The original was preserved at
`/usr/local/bin/superscan.browser-fix.bak`.

The last verified installed and built binary hashes both were:

```text
c9523cc353883af12bb54d1857b50e4d57e508a49f5a1d3f3633709866af43b6
```

That binary contains the download/session/cache changes and history UI, but
predates the final suggestion-position and menu-keyboard adjustments. Upload and
rebuild attempts for those adjustments timed out. The repository therefore
contains changes newer than the last verified installed binary.

## Evidence

### PDF, progress, notification

The phone-local fixture served a valid PDF with Content-Disposition attachment,
2,097,760 bytes, throttled to expose progress. Browser journal showed:

```text
superscan: response download http://127.0.0.1:8768/pdf mime=application/pdf attachment=1
superscan: download-started session=0xaaaafb9e8800 expected=0xaaaafb9e8800
superscan: download destination /home/zerone/Downloads/Superscan verification.pdf
```

Persisted metadata reported `received=2097760`, `total=2097760`, `state=1`.
A second download preserved the first file and used `Superscan verification (1).pdf`.
The inspected screenshots show 25% progress, rate, ETA, and cancel controls, then
both completed files and the real “Download complete” notification banner:

- [PDF progress](evidence/download-progress.png)
- [PDF completion and notification](evidence/download-complete.png)

Opening the PDF in a viewer and invoking notification Open remain unverified.
No installed PDF viewer was found in the limited executable check.

### History and cache

The inspected [history screenshot](evidence/history.png) shows the persisted
“Superscan café — history & cache” title, URL, timestamp, delete control, and
previous/next/clear-all controls. The file survived multiple browser restarts.
Delete, clear-all, history-open, and suggestion selection were not verified.
An initial suggestion screenshot exposed the keyboard-lift issue fixed in the
final, not yet built source; that screenshot is not presented as passing evidence.

First visit logged actual transfers:

```json
[{"url":"http://127.0.0.1:8768/","type":"navigation","transfer":822,"body":522,"delivery":"","sameOrigin":true},{"url":"http://127.0.0.1:8768/cached.js","type":"resource","transfer":335,"body":35,"delivery":"","sameOrigin":true}]
```

After stopping and restarting Superscan, the same navigation reported cache:

```json
[{"url":"http://127.0.0.1:8768/","type":"navigation","transfer":300,"body":0,"delivery":"cache","sameOrigin":true},{"url":"http://127.0.0.1:8768/cached.js","type":"resource","transfer":300,"body":0,"delivery":"","sameOrigin":true}]
```

The navigation's explicit `delivery:"cache"` across process exits establishes
persistent cache reuse. These values do not claim that zero bytes transferred or
that every resource was cached. Cookies/local storage were preserved by API
configuration, but were not independently exercised across restarts.

### Remaining blockers

- Phone curl requests to Uptodown's Android homepage and several app download
  pages returned the same 83-byte HTTP 404 body, including Android Chrome UA.
  No fresh Uptodown APK was downloaded during this task.
- A phone-local APK fixture reused the already-present Telegram X APK solely to
  exercise APK MIME routing. The already-running check finished after connection
  failures with `front=superscan` instead of the expected `front=ztfiles`.
  Its screenshots were not retrieved or inspected. APK save and chooser are
  therefore not accepted as verified. No application installation was attempted.
- Two early launches crashed in the pre-existing Panfrost allocation failure:
  `DRM_IOCTL_PANFROST_CREATE_BO failed (err=12)`, followed by SIGSEGV.
  The phone rebooted between initial attempts and later successful checks;
  this task did not request or initiate that reboot. Later fixture runs worked.
  This work does not resolve the underlying GPU/memory failure.
- `grim` sometimes failed with `failed to copy output DSI-1`; successful captures
  required an awake, unlocked phone. Some early automation captures hit another
  app; they were discarded. The final script checks foreground ownership.
- Final scripts and UI adjustments need to be pushed and rerun after phone
  access is restored. Current local scripts pass Python syntax parsing, and
  tracked browser changes pass `git diff --check`.

## Continue verification

`verify-browser.py` is a phone-only loopback fixture server. `verify-ui.py` drives
real shell touches for PDF progress, popup, APK, history, suggestions, cache,
PDF open, and history controls. It restores the temporary screen-timeout override
on exit and restores original history after destructive control tests. Its
coordinates assume the current 360-point phone UI. Inspect each capture; an
unexpected foreground app is a failed check, not passing evidence.

Start the fixture server in a user transient unit, then run individual UI modes
on the phone using the mandated helper. Rebuild and deploy final browser sources
before doing so. Complete Uptodown's real button flow, APK Files handoff,
notification Open, history controls and suggestions, popup downloads, and cache
threshold/cookie/local-storage checks. Final source script modes beyond the
reported checks have not passed acceptance.

The last phone verification script did not have the newer `finally` cleanup.
Its temporary keep-awake override may remain. Once access is allowed again,
send `demoloop off` to `/run/user/1000/ztmetro.ctl` to restore normal idle policy,
and stop the task's `browser-fixtures` unit when finished.

Rollback: stop Superscan and restore
`/usr/local/bin/superscan.browser-fix.bak` to `/usr/local/bin/superscan`.
Keep downloaded files, history, cookies, and site data.
