# Superscan scrolling

## Shipped behavior

Superscan now owns drag, fling, and edge spring motion. It requests WebKit document scroll positions asynchronously, tags imported buffers with the latest completed position after a one-buffer fence, then draws the buffer at `actualY - P`. It fills exposed pixels with the page background read from CSS. Taps and non-scroll gestures still reach WebKit. If document `scrollY` stalls for two observations, Superscan switches that gesture to WebKit touch scrolling.

Physics match ztMETRO lists: fling time constant 380 ms, maximum speed 7 px/ms, edge resistance `0.55*d/(1+d/screen)`, and spring frequency 0.016. Smooth WebKit scrolling is disabled. A trace summary reports scroll frame count, worst interval, and intervals over 20 ms.

Overscan is omitted. The WPE view remains screen-sized to preserve `100vh` and fixed-layout pages; uncovered strips use the sampled page background. This avoids additional WebKit buffers and their memory cost. No full-document snapshots or persistent tile cache are used.

## Phone build and deployment

Built on the phone in `/home/zerone/ztmetro-phase2` and installed as `/usr/local/bin/superscan`. Final binary SHA-256: `02f853c27e40f4f34242c61219944657814622a5b4e580e2f39a06611dbb0f29`.

Deployment kept the original binary at `/var/lib/ztos-scroll-backup/usr/local/bin/browser.before` (SHA-256 `67979d5422e0bd1cf8afbb3887ebd34a3eaa773564ba90e87e919d32770bf8b0`). ztMETRO and Waydroid were not restarted.

## Measurements

On-device `ZTB_TRACE=1` measurements. A frame means time between Superscan scroll timer frames. The baseline predates this change and includes the same repeated swipe workload on Uptodown; its long gaps include idle periods, so compare as workload evidence, not a controlled percentile benchmark.

| Page / run | Scroll frames | Worst interval | Over 20 ms | Notes |
| --- | ---: | ---: | ---: | --- |
| Uptodown, before | 424 | 1187.9 ms | 325 | 18 alternating swipe workload; trace included idle gaps |
| Uptodown, after | 16 | 22.81 ms | 1 | Controlled drag and fling; requested end position 1441.1 px, WebKit actual 1441 px |
| Guardian long news, after | 61 + 59 | 23.68 + 20.28 ms | 2 + 1 | Two timer windows; document actual Y reached about 1506 px; no inner-scroll fallback |
| Commons featured-pictures gallery, after | 55 + 57 | 51.51 + 29.10 ms | 14 + 6 | Image-heavy gallery; actual Y reached 787 px |
| Commons gallery, second workload | 58 + 58 | 35.39 + 23.90 ms | 5 + 6 | Further swipes; actual Y reached about 6093 px of 6458 px |

The `<20 ms` target is not met consistently. Guardian had occasional misses, and the image-heavy gallery had frequent misses. The browser emits accurate translated snapshots while WebKit catches up, but GPU/browser work still creates long frame intervals on these pages. The initial Uptodown baseline is not directly comparable because it includes idle gaps and a different trace window.

Inner-element fallback was observed on a Guardian consent overlay where document `scrollY` did not move. After dismissing it, document scrolling worked and the test above stayed in Superscan mode. No memory peak was measured; this design adds no overscan buffers or document-sized captures.

## Rubber-band capture

The screenshot below shows the top-edge stretch over Uptodown, with the page background filling the exposed strip. It was captured on an earlier deployed iteration; the edge resistance and spring code were unchanged in the final build.

![Superscan top-edge rubber-band capture](scroll-rubber-band.png)
