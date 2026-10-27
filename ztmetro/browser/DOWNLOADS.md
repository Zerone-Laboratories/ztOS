# Superscan downloads

Superscan saves WebKit downloads in `~/Downloads`. Existing names are preserved;
collisions receive ` (N)` before the extension. The downloads page is in the
address bar's `...` menu. It shows active and saved entries with bytes, percent,
rate, and ETA when WebKit reports a total size. Active entries can be cancelled.
Failed entries can be retried. Completed entries can be opened or removed.

History lives at `~/.local/share/superscan/downloads.json`. Downloads keep
running while Superscan is backgrounded. zrnperfd reads the live tile's
`download=1` marker and exempts Superscan from tickle until all downloads end.
Completion and failure notifications have an Open action. Opening an APK
passes its path to Files, which offers ztOS runtime (ATL) or Waydroid install.

## Phone build and deployment

Built on the phone with:

```sh
make browser/superscan files/ztfiles perfd/zrnperfd
```

Installed on the phone. Original binaries are backed up as:
`/usr/local/bin/superscan.downloads.bak`,
`/usr/local/bin/ztfiles.downloads.bak`, and
`/usr/local/sbin/zrnperfd.downloads.bak`.

## Verification status

The phone build completed. The deployed browser started and rendered through
WPE. A local attachment server returned a file with `Content-Disposition`, but
the download did not appear in `~/Downloads`; the phone session did not reach
the WebKit download callback. Uptodown's page yielded no direct APK URL in the
phone shell. Cancel, retry, Files handoff, notifications, and tickle exemption
remain unverified on-device. Phone screenshot capture via `grim` failed with
`failed to copy output DSI-1`, so no progress, list, or notification screenshots
were captured.
