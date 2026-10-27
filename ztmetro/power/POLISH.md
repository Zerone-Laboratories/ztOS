# Power Manager memory follow-up, 2026-10-25

Source inspection found a large retained page allocation, not app-owned images:
history rendered all 64 events at 59 points per row. ZrnMetroCore caches the
entire page at scale 4 and grows that cache without shrinking it on page changes.
The GPU also holds textures and references to Cairo source surfaces.

History now displays eight events per page with newer/older buttons. All 64
events remain available; log storage and aggregate statistics are unchanged.
The focused layout test covers event counts 0 through 64, verifies each event
appears exactly once across pages, and checks page clamping after a log shrinks.
Maximum history height is 806 points with widget geometry stubs, versus 3930
points for the old full history. At 360 points wide and scale 4, that changes
the history raster estimate from 86.4 MiB to 17.7 MiB, before GPU allocations.
These are allocation estimates, not measured RSS results. The sleep page may
still determine the retained cache height after visiting both tabs.

The polling timer now redraws only when readings or the displayed rounded
sleep ratio change, instead of redrawing because unshown uptime changed.
The current shared core's existing GPU cache budget is included in the build;
it was not changed by this task.

Built on the phone in `~/swarm-polish`, installed after backing up
`/usr/local/bin/ztpower.before-polish`. Fresh screenshots in `evidence/` show
the sleep page, events 1–8, and the working older-events page (9–16).
The focused history test passes for counts 0–64, with all events reachable
exactly once and a maximum history height of 806 points. Its command now links
libm because the app's polling path compares rounded ratios.

Measured baseline PID 23945 for 10m45s: final RSS 278924 kB, sampled maximum
291792 kB, process peak 302164 kB. After-run PID 26767 is sampled separately
for ten minutes; final numbers are recorded in `~/swarm/polish-report.md`.
At seven minutes its RSS was 164160 kB, after a peak of 201492 kB. Repeated
tab transitions did not exceed that peak. Smaps shows the large anonymous
raster mapping shrinking from 88428 kB RSS to 21896 kB, alongside a smaller
heap (118500 kB to 88064 kB in the captured snapshots). No app images exist.

Evidence: `evidence/power-before-rss.txt`, `evidence/power-after-rss.txt`, both
smaps captures, build logs, and before/after screenshots. This is an interactive
retained-memory comparison; old/new share the same phone and were backgrounded
while other checks ran. It does not isolate pagination from the existing core
cache budget, and it does not prove absence of leaks beyond the sample window.

Final ten-minute after sample: 07:25:52–07:35:58, endpoint 164152 kB RSS,
maximum sampled 201460 kB, process peak 201492 kB. Endpoint reduction is
41.1%; peak reduction is 33.3%. Raw logs and `evidence/memory-summary.json`
contain exact timestamps, PIDs, RSS, high-water marks, and swap. Final installed
build uses memcpy for the reading snapshot so padding cannot cause spurious
memcmp invalidation. It was rebuilt and relaunched after the measurement.
