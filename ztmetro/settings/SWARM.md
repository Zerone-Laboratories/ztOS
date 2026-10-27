# Settings backgrounds and Segoe system icons — 2026-10-25

Status: implementation built, installed, and verified on the phone. Final live Start and app-list screenshots confirm corrected Segoe glyphs, including the monochrome battery. Remaining visual/performance limitations are listed below.

## Changes

- `ztmetro/settings/ztsettings.c`: one GLib worker handles image decode, orientation, downscale, source metadata and disk cache reads/writes. Immutable Cairo surfaces return through `GAsyncQueue`; drawing never decodes a source photo. Thumbnail cache: `$XDG_CACHE_HOME/ztmetro/wallpaper-thumbs`, normally `~/.cache/ztmetro/wallpaper-thumbs`. Keys include source path, size, nanosecond mtime, dimensions and format version. Invalid/wrong-size PNGs regenerate. Writes use a process-specific temporary file and atomic rename. Failed decodes leave placeholders.
- One GPU atlas per wallpaper category prevents the six-picture texture cache from cycling through visible photos every frame. The grey placeholder keeps a stable revision. Category preview redraws coalesce until pending work finishes. The existing 64 MiB GPU cache budget is unchanged.
- Fixed an entrance-timer race found during final verification: fast cached results previously shortened the redraw deadline, potentially stopping frames before staggered cards appeared. Results now only extend the deadline; category opening includes the full maximum stagger.
- `ztmetro/core/zrnmetrocore.c` and `.h`: added `zmc_gpu_quad_region` for atlas cells. Full-surface quad drawing delegates to it; existing GPU tiling supports tall atlases.
- `ztmetro/apps/ztapps.c`: map native desktop IDs to Segoe UI Symbol glyphs, rasterized once into the existing A8 icon cache. Theme icon handling remains for third-party apps.
- `ztmetro/ztmetro.c`: expanded native tile/name mappings, corrected Messaging, Power and Photos codepoints, and explicitly selected Segoe UI Symbol with Pango font fallback disabled. Live screenshot review caught emoji fallback for U+1F50B; the explicit font fix makes the battery monochrome.
- `os/overlay/usr/local/share/fonts/segoe/seguisym.ttf`: supplied the existing local Symbol font. Contrary to the task premise, it was missing from the phone initially: fontconfig returned Noto Sans. Installed font now resolves to `seguisym.ttf` and survived the reboot that preceded this resumed run.

Other agents' ringtone, shell, app-scanner and core changes were preserved. No commits were made. No ztmetro README files were present.

## Mapping

| Desktop ID | Glyph | Codepoint |
| --- | --- | --- |
| ztphone | Handset | U+E13A |
| ztmessages | Message bubble with smile | U+E15F |
| ztcontacts | People | U+E125 |
| ztsettings | Gear | U+E115 |
| ztpower | Monochrome battery | U+1F50B |
| ztcamera | Camera | U+E114 |
| ztphotos | Picture library | U+E158 |
| ztstore | Shopping bag | U+E14D |
| com.zerone.music | Music notes | U+E142 |
| ztfiles | Folder | U+E188 |
| superscan | Globe | U+E128 |
| ztcalendar | Calendar | U+E163 |
| ztclock | Clock | U+E121 |
| ztcalculator | Calculator | U+E1D0 |
| ztdevices | Connected devices | U+E212 |


Each ID includes `.desktop` in the scanner table. All 15 codepoints exist in the supplied font and were visually checked in `evidence/symbol-coverage.png`. Photos, Calendar, Clock and Calculator mappings prepare native IDs not present in the captured app list. Installed native entries include Phone, Messaging, People, Music, Files, Store, Devices, Superscan and Power Management; Settings is also mapped directly by the shell.

## Measurements on the phone

Original perf baseline: 98 samples, including Cortex-A55 events, no lost samples. Call paths included `picture`, `zmg_surface` and `present_gpu`; memcpy/Gallium upload costs support repeated texture upload as a cause. See `before-profile.txt`. The attempted strace capture was not useful; no syscall-profile conclusion is claimed.

Tests opened the wallpaper category picker, entered Lumia 1020 and performed four alternating drags. Callback intervals use compositor timestamps from the Wayland Default Queue, retaining `0 < dt < 250 ms` to exclude deliberate idle gaps. Debug logging and shared phone load affect results. Original baseline CPU affinity was not recorded, so these are not controlled CPU-for-CPU comparisons.

| Build / cache | Intervals | Median | p95 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Original baseline | 128 | 39 ms | 99 ms | 197 ms |
| Worker/cache, before atlas | 192 | 36 ms | 52 ms | 112 ms |
| Atlas, cold cache | 386 | 17 ms | 30 ms | 155 ms |
| Atlas, warm cache | 227 | 21 ms | 34 ms | 196 ms |
| Atlas, invalid cache PNGs | 385 | 18 ms | 31 ms | 137 ms |
| Final atlas + timer fix, warm | 274 | 19 ms | 33 ms | 105 ms |

Atlas tests launched Settings restricted to efficiency CPUs 0–3. The cold/warm/corrupt harness confirmed affinity `[0,1,2,3]` after each run. The final process was later observed on CPUs 0–1 while idle; no performance-core or frequency boost was used. No cpuidle settings were changed.

The first three atlas runs precede the final timer fix. Final timer-fix calculation starts at the category touch and includes the idle grid, four drags and Back. Its startup occurred with the display off and took 15 seconds; startup callbacks are excluded from that calculation and are not entrance-performance evidence. See `frame-summary.json`, `atlas-summary.json`, `atlas-scroll-summary.json`, `final-timer-summary.json`, and their corresponding `.frames` logs.

Final grid statistics: **255 GLES frames, 1.48 ms average CPU submission, 12.14 ms worst, three texture uploads totaling 18.8 MB**. An earlier atlas run recorded 231 frames with three uploads. Texture reuse therefore persists across scrolling; uploads do not recur per visible cell/frame.

Final atlas perf: **314 Cortex-A55 cycle samples, zero lost samples**. PNG compression appears on the `wallpaper-thumb` worker. See `atlas-profile.txt` and `atlas-perf.log`. Perf also reported unavailable `/proc/schedstat`; no scheduler statistic is claimed.

## Correctness and visual evidence

All paths below are relative to `ztmetro/settings/evidence/`; retrieved PNGs were opened and visually inspected.

- Before: `before-categories.png`, `before-grid.png`.
- Worker-only stage: `after-grid.png`.
- Atlas: `final-categories.png`, `final-grid.png`, `atlas-cold.png`, `atlas-warm.png`, `atlas-corrupt.png`.
- Final timer fix: `final-timer-state.png`, `final-idle-grid.png`, `final-return.png`. Thumbnails appeared without a scroll; Back returned to categories.
- User photo without a shipped thumbnail: `user-categories.png`, `user-grid.png`. A uniquely named temporary copy of a shipped full-size JPEG rendered correctly, then was removed. Existing user photos were not modified.
- Font proof on phone: `symbol-render-check.png`, produced from the exact drawing function, and `final-binary-battery-tile.png`, produced by the installed shell's `--render` path. Both show monochrome battery glyphs.
- Recovery: `post-restart.png` shows the restored lock wallpaper and shell-restarted notification after the first resumed restart.

Cold, warm and corrupt-cache runs each produced 17 thumbnail entries. The original cache was renamed rather than deleted and later merged back without replacing regenerated entries. Subsequent PNG validation checked signatures, dimensions, every chunk CRC and IDAT decompression: all 43 files then present passed (`cache-check.json`). Cache test and measurement scripts are retained alongside the evidence.

A rapid-switch automation attempt sent taps before transitions completed, so it does not prove safe switching while jobs remain pending. Scripted taps accidentally selected `none` during the earlier run and this resumed test. Both wallpaper keys were restored to repository defaults, preserving other config keys. Their original pre-test values were not captured. Defaults now point to existing `Lumia Cyan/Background_01.jpg` and `Lumia Cyan lock screens/Wallpaper_01.jpg`. A config backup is retained on the phone.

## Deployment and recovery

Build copy: `/home/zerone/swarm-settings-icons`, copied once from `~/ztmetro-phase2`; builds ran on the phone. Settings, ztapps and shell compiled successfully. Existing snprintf truncation warnings remain; see `resume-build.log`, `final-settings-build.log`, and `final-shell-build.log`.

Backups: `/usr/local/bin/{ztsettings,ztapps,ztmetro}.before-settings-icons`. Final Settings binary SHA-256: `8300cda5553052779fd572012363bf4648f23f3dcdefdfffb9366ce220d697a0`. Final shell binary SHA-256: `94c2c539078fd1baf119ae1c7d183143141569fd3144a58c1edc7f54ba72860c`. Installed binaries matched build outputs. The scanner daemon was confirmed to use the installed executable and retain corrected mappings.

The first resumed shell restart ran under flock at 06:41:39 after the prior run's 06:11 restart. The battery fallback correction required waiting the full next interval. Final restart ran under `/tmp/swarm-shell.lock` at **07:11:56**, over 30 minutes later. Shell source SHA-256 matched current repository source before restart: `45908e9a8b6c308f3942dc365fef05093651ed70e4c57cd78dce085c2bf9c740`. Final Settings source SHA-256: `8574a6faf7bcbf571cd7bd45ce3dd55b1193f01510adf1431dc67b07a6f83736`.

Automatic session recovery started shell PID 18934. The existing boot-readiness path can show the splash for about 45 seconds before continuing. Sensor warnings were closed through `dialog_close`; no recovery/reboot button was tapped. No deliberate full reboot was performed. Binaries, font and thumbnail cache are installed persistently, and shell restart recovery was observed.

## Reference comparison

Searched Lumia 1020 Start-screen and Windows Phone app-list references. Microsoft blog image hosts failed to load. A primary reference was successfully retrieved and inspected instead: [Nokia Lumia 1020 User Guide, page 24](https://www.vodafone.de/media/downloads/help-devices/bedienungsanleitung-nokia-lumia-1020-en.pdf), copyright Microsoft Mobile. `lumia-reference.png` contains that page. Its white handset, speech bubble, gear and store silhouettes, accent tiles, and square app-list icons align with the chosen Symbol mappings and shell layout. Superscan uses a globe rather than Internet Explorer branding. This is a visual comparison, not a pixel match to Lumia firmware.

Additional references: [Microsoft Lumia screen guide](https://blogs.windows.com/devices/2015/01/26/lumia-screens-explained-glance-lock-start-app-list/) and [Microsoft Lumia 1020 Start-screen example](https://blogs.windows.com/devices/2013/08/13/mark-guim-my-windows-phone-8-start-screen/).

## Remaining limitations

- Page transitions still have occasional long frames: warm opening recorded about 185 ms worst CPU submission. Improved scrolling does not prove sustained 60 FPS.
- Some header/caps and target-button text disappears after page turns, visible in final Settings captures. The timer fix resolves delayed/blank thumbnail animation, but this text-rendering defect remains.
- Rapid switching while decode jobs are pending and a deliberate full reboot were not proven. Native apps absent from the installed catalogue have font coverage checks, not live app-list screenshots.
- Original wallpaper selections cannot be reconstructed; repository defaults were restored after automation mistakes.

## Final live icon verification

Retrieved and visually inspected `evidence/final-start.png`, `final-apps-top.png`, and `final-apps-2.png` after the final restart. Start shows the correct white handset, gear, people, devices, folder, music, globe and monochrome battery. App-list captures show Store, Devices, Files, Messaging, Music, People, Phone and Power Management with the mapped Segoe glyphs. The battery is white in both locations; the earlier green emoji is gone. `final-apps-1.png` and `final-apps-3.png` provide additional captured list positions. The shell was left on Start.

`evidence/final-source-check.json` confirms all eight relevant shell, Settings, scanner and core build sources match the current repository, with no mismatches. Focused `git diff --check` passed. Final installed binary hashes and phone state are recorded in `evidence/final-phone-state.txt`. No further deployment is pending.
