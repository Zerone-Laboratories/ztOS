# ztCamera

Lumia Camera style viewfinder with right-side photo/video shutter, five manual
control arcs, camera roll preview, framing grid, self-timer and source menu.
ZrnMetroCore owns Wayland/input; GLES draws the frame and cached Cairo control
surfaces as textures. The animated test scene and YUYV conversion run on CPU;
this is not a GPU ISP. No extra service or camera module is started.

Build **on the phone** in a private copy of `~/ztmetro-phase2`:

```sh
make camera/ztcamera
```

The parent Makefile already includes this target in `all` and has installation
rules for the executable, desktop file and SVG. This task does not change that
shared file. Install only camera artifacts while others work:

```sh
sudo cp -an /usr/local/bin/ztcamera /usr/local/bin/ztcamera.before-camera
sudo install -Dm755 camera/ztcamera /usr/local/bin/ztcamera
sudo install -Dm644 camera/ztcamera.desktop /usr/local/share/applications/ztcamera.desktop
sudo install -Dm644 camera/ztcamera.svg /usr/local/share/icons/hicolor/scalable/apps/ztcamera.svg
```

Skip the backup command if no old executable exists. Do not overwrite an existing
backup. Restart only this app; no compositor restart is needed. The shell's
existing Camera mapping and app-list glyph use Segoe UI Symbol U+E114; the photo
shutter now uses the same glyph. The SVG remains an icon-theme fallback.

## Sources

* Default: `/dev/video0` if accessible, otherwise a labelled animated test scene.
* `--device /dev/videoN`: single-plane V4L2 capture, MMAP streaming, negotiated
  YUYV at a requested 640x480. Failed setup falls back to the labelled scene.
  Multi-planar, Bayer, MJPEG and media-graph setup are not implemented.
* `--replay FILE --size 640x480 --rate 15`: headerless packed YUYV frames,
  looped. Width must be even; dimensions must be 16–4096. Partial final frames
  are ignored. Invalid sources fall back to the scene.
* `--render OUT.png [dial 0-4]`: offline reference image of the scene/UI;
  this is not a compositor screenshot or proof of phone rendering.

Dials change simulated exposure, WB, ISO grain, focus and slow-shutter trails.
For V4L2/replay only exposure and WB are applied in software. Sensor-side manual
controls and hardware autofocus are not wired. Front/rear toggles select test
fixtures only; real source switching uses the source menu or `--device`.

Photos save full source frames (not the cropped portrait preview) as PNG under
`~/Pictures/ztCamera`. The thumbnail opens the newest photo. Video writes BGRA
frames at up to 15 fps, without audio or a container, with a 48 MiB safety cap.
Disk errors stop recording. Convert with the dimensions of the selected source:

```sh
ffmpeg -f rawvideo -pixel_format bgra -video_size 640x480 -framerate 15 -i VID_file.bgra video.mp4
```

The recording cadence depends on incoming frames and load; raw files do not
carry timestamps. This is a testable recording path, not production video.

## Validation

Run `sh camera/check.sh` from `ztmetro/` **on the phone**. It builds the app and
regression harness in that private checkout, checks replay validation and padded
YUYV rows, tests non-overwriting captures, and renders all five dial references.
It requires the normal app dependencies; it does not access camera hardware.
Then launch `ztcamera`, capture the screen with `grim`, and exercise photo,
thumbnail, each dial, source fallback, front test card and video start/stop.
Inspect actual screenshots before declaring UI complete. See the task report
at `../../camera/SWARM.md` for what was actually verified this session.

Visual references:

* https://allaboutwindowsphone.com/reviews/item/18067_Nokia_Lumia_1020-the_camera.php
* https://allaboutwindowsphone.com/features/item/22261_Lumia_Camera_for_the_1020_retu.php
* https://blogs.windows.com/devices/2013/10/25/nokia-camera-combining-best-nokia-camera-apps/

The reference uses translucent controls over a full viewfinder, five manual
settings, curved dials and a shutter cluster. This app adapts those elements to
the S9 portrait viewport. Phone screenshots and checks are recorded in `../../camera/SWARM.md`. The
scene runs around 21–27 fps; 60 fps smoothness and pixel matching are not claimed.
