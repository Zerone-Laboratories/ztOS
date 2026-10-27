# Superscan swarm report — 2026-10-25

## Result

Implemented and deployed browser suggestions, searchable day-grouped history,
favourites, InPrivate tabs/windows, and background graphics cleanup. Phone
checks cover the real touch path, screenshots, persistence, storage isolation,
snapshot scrolling, rubber banding, and downloads while another app is visible.

The exact originally reported shell corruption was not reproduced in the first
Commons gallery → Home → Settings cycle. Testing did reproduce Panfrost
allocation failures and a browser crash, and found a separate deterministic
snapshot ownership bug. The identified browser-side defects were corrected. Shell-glitch acceptance
remains partial: kernel/GPU allocation failures still occur in final validation.

## Changes

- `superscan.c`: completed the previously unfinished menu/action wiring,
  favourites restore, private-tab creation, history search, suggestion selection,
  and tab visibility. History receives late-arriving page titles. Search URLs
  are escaped. Private page names and audio titles do not enter the live tile.
- `superscan.c`, `suggest-json.h`: DuckDuckGo's actual OpenSearch endpoint,
  `https://duckduckgo.com/ac/?q=…&type=list`, replaces the mistaken search-page
  request. Both sending and reading are asynchronous through libsoup 3.
  Input changes cancel the previous request and debounce for 220 ms. Generation
  checks reject stale replies. JSON parsing handles escaped quotes, Unicode,
  surrogate pairs, duplicates, and malformed responses. Local favourites/history
  precede engine suggestions. InPrivate suppresses both the list and requests.
- `superscan.c`: history lives at `~/.local/share/superscan/history` and bookmarks
  at `~/.local/share/superscan/bookmarks`. History groups by today, yesterday, or
  date; supports filtering, opening, deleting, clearing, and paging.
- `superscan.c`, `webview.c`: the menu creates an InPrivate tab with a blue
  indicator. `superscan --inprivate URL` opens a private window. Each private
  session uses `webkit_network_session_new_ephemeral()`. Visits are not saved;
  opt-in network timing logs and failed-navigation logs exclude private URLs.
- `webview.c`: tabs share a graphics display and WebKit context instead of
  creating another display/context per tab. Network sessions retain their
  persistent/ephemeral ownership. Inactive views unmap; acknowledge pending
  frames, release held buffers and GPU imports, forget CPU/GPU snapshots, cancel
  captures, and discard late hidden results. Resume maps and repaints the view.
  Closing a view detaches callbacks and borrowed owner pointers.
- `webview.c`: **removed an invalid `g_bytes_unref()` of borrowed snapshot
  pixels**. The installed WPE 2.54 GIR declares `webkit_image_as_bytes()` as
  `transfer-ownership="none"`. Before the fix, every fixture snapshot emitted
  `g_atomic_ref_count_dec: assertion 'old_value > 0' failed`. Final snapshot
  captures and resume no longer emit that assertion.
- `test-suggest.c`, `verify-swarm.py`, `README.md`: focused parser regression
  checks, phone-only acceptance fixtures/driver, and current usage notes.

Existing browser work was preserved, including downloads and the native touch,
snapshot/fling, and rubber-band paths. `webview.h` already contained the private
view interface when this task began. No shell, SDK, kernel, power-policy,
Waydroid, or other agent's source files were edited.

## Deployment

Built on the phone in `/home/zerone/swarm-superscan`, copied once from
`~/ztmetro-phase2`. Current repository SDK sources were copied into this isolated
build because the original phone SDK copy lacked `zmc.active` and `zmg_forget`.
The shared build tree was not used for compilation. The phone build completed
without compiler warnings.

Installed `/usr/local/bin/superscan` SHA-256:

```text
9376e4a91f22fb65974d4fc23215da0b283ea526013b15bfc74a898782cd452f
```

Original binary preserved at `/usr/local/bin/superscan.before-superscan`:

```text
c9523cc353883af12bb54d1857b50e4d57e508a49f5a1d3f3633709866af43b6
```

The installed binary and isolated build binary hashes match. The normal desktop
launcher continues to use this installed path after reboot. No shell restart or
phone reboot was performed. Test browser and fixture units were stopped, the
keep-awake override was turned off, and the phone was left on Start. No commits
or pushes were made.

## Phone evidence

Screenshots below were retrieved and visually inspected. All destructive history
checks use `/home/zerone/swarm-superscan/test-home`, with a separate cache and
Downloads directory. Normal user history, cookies, downloads, and settings were
not cleared.

| Check | Evidence and result |
| --- | --- |
| Heavy real page baseline | [Commons gallery](evidence/superscan/heavy-before.png), [Settings before](evidence/superscan/settings-before.png): both rendered normally in the initial reproduction attempt. |
| Engine suggestions | [Typed query](evidence/superscan/engine-suggestions-final.png): real DuckDuckGo answers and search glyphs. [Selection](evidence/superscan/engine-suggestion-open.png): address navigates to the selected search URL; remote results-page completion is not asserted. |
| Bookmark suggestion/open | [Starred local match](evidence/superscan/bookmark-suggestions.png), [opened page](evidence/superscan/bookmark-open.png). Bookmark persistence is checked on disk. |
| History | [Three day groups](evidence/superscan/history-grouped.png), [search](evidence/superscan/history-search.png), [delete](evidence/superscan/history-delete.png), [clear](evidence/superscan/history-clear.png). Assertions verify three entries become two after deletion, then zero after clear. The filtered empty state says “nothing found.” |
| Private isolation | [First private session](evidence/superscan/private-a.png), [fresh second session](evidence/superscan/private-b.png): both start with empty cookies and local storage. History bytes remain identical across private visits. |
| Public persistence | [Normal session after private exits](evidence/superscan/public-storage-restored.png): `swarm=public` cookie and `public` local storage survive browser restarts and private sessions. |
| Private suggestions/menu | [Typing without suggestions](evidence/superscan/private-no-suggestions.png), [menu-created tab](evidence/superscan/private-from-menu.png). Trace assertions find no suggestion request during private typing; WebKit reports `ephemeral=1`. |
| Snapshot scrolling | [Before](evidence/superscan/snapshot-page.png), [after drag/fling](evidence/superscan/snapshot-scrolled.png), [same position after resume](evidence/superscan/scroll-resumed.png). |
| Rubber band | [Held top-edge stretch](evidence/superscan/rubber-band-final.png); page background fills the exposed area. |
| Other shell UIs | [Settings](evidence/superscan/settings-final.png), [Power Management](evidence/superscan/other-app-final.png) after Home from the canvas-heavy browser fixture; no corruption visible. Power settings were not changed. |
| Background download | [Progress](evidence/superscan/download-progress-final.png), [completion](evidence/superscan/download-complete-final.png): 1,048,576 bytes completed while Settings was shown. Collision suffixes preserve existing fixture downloads. |

[Final scroll log](evidence/superscan/scroll-final.log) records 720 × 3408 pixel
snapshots, and background cleanup releasing two imported frames plus 9,815,040
snapshot bytes. A fresh snapshot appears after resume. The final controlled
scroll cycle has no borrowed-bytes refcount assertion. These are functional
checks, not a new frame-time benchmark.

[Storage reports](evidence/superscan/storage-reports.jsonl) contain only loopback
fixture values. [Acceptance log](evidence/superscan/acceptance.log) records the
final feature checks.

## Remaining limits

- Panfrost failed allocations during earlier and final runs, including while opening a
  second tab and during app switching. Mesa reported
  `DRM_IOCTL_PANFROST_CREATE_BO failed (err=12)`; one browser process exited with
  SIGSEGV. [Kernel evidence](evidence/superscan/panfrost-mmu.txt) shows warnings in
  `panfrost_mmu_map`, reached through `panfrost_gem_open` and
  `panfrost_ioctl_create_bo`. At inspection, roughly 1 GB of system memory was
  available but swap was essentially full. This is insufficient evidence to
  identify the underlying kernel allocation failure or guarantee its removal.
  [Final validation log](evidence/superscan/final-validation.log) still contains
  allocation failures despite passing functional checks.
  Graphics sharing, hidden-buffer release, and the snapshot ownership fix reduce
  browser pressure; a general driver fix remains outside this browser patch.
- `wl-copy`/`wl-paste` fallback surfaces temporarily acquire focus in this shell
  and return to Start on unmap. The optional clipboard navigation test helper
  explicitly restores browser focus and keyboard visibility. Main acceptance
  checks use direct launches and real keyboard touches instead of clipboard
  navigation. General clipboard focus behavior remains unchanged.
- A ZrnDevTest launch did not become foreground during an early scrolling
  check; the accepted other-app check uses the already-running Power app.
  Failed early captures are not used as acceptance evidence.
- No reboot, long-duration soak, fresh scroll performance benchmark, or complete
  downloads cancel/retry/APK handoff retest was performed. Existing unrelated
  issues recorded in DOWNLOADS.md/BROWSER-FIX.md are not declared fixed here.

## Repeat checks

Run on the phone through `/tmp/ztos-phone-control.py`. Start
`python ~/swarm-superscan/browser/verify-swarm.py server` in the user transient
unit `swarm-superscan-fixtures`, then run the following modes sequentially:

```sh
python ~/swarm-superscan/browser/verify-swarm.py private-check
python ~/swarm-superscan/browser/verify-swarm.py history-check
python ~/swarm-superscan/browser/verify-swarm.py suggest-check
python ~/swarm-superscan/browser/verify-swarm.py scroll-check
python ~/swarm-superscan/browser/verify-swarm.py download-check
```

`scroll-check` expects an existing `ztpower` window for its other-app check.
The parser test also passed locally under AddressSanitizer and UBSan, including
all truncated prefixes of a valid response, Unicode/surrogates, malformed input,
and duplicate answers. Python syntax checks and browser `git diff --check` pass.

Rollback: stop Superscan and restore `/usr/local/bin/superscan.before-superscan`
to `/usr/local/bin/superscan`. Keep browser data and downloads.
