# Bug: host audio broken since Waydroid was installed

Phone access: `python /tmp/ztos-phone-control.py run|sudo|put|get`; two failed logins = stop.
User session env: XDG_RUNTIME_DIR=/run/user/1000. Phone is often unattended: any fix must
survive reboot and self-recover. Another Codex agent is concurrently working on ztos-android
registry, .desktop files and ztMETRO window mapping (WHATSAPP-DUAL-TASK.md): do NOT edit
ztmetro/ztmetro.c, ztos-android, or restart ztMETRO. Do not delete user data; do not uninstall
Waydroid or ATL.

User report: "the audio is somehow broken after waydroid installation".
Observed (PipeWire 1.6.9, WirePlumber, pipewire-pulse all active since boot 20:40 UTC):
- Card 0 snd-zrn-abox "Galaxy S9 speakers (ABOX CRL0, MAX98512)".
- Only sink: `alsa_output.platform-snd-zrn-abox.stereo-fallback` ("Built-in Audio Stereo",
  vol 0.80); NO sources. Stream "ztOS system sounds" (pid 699) ports stuck `[init]`;
  /proc/asound/card0/pcm0p/sub0/status = closed. Only wireplumber holds /dev/snd/controlC0.
- Waydroid session config bind-mounts /run/user/1000/pulse/native into the container.
- wireplumber log only shows UPower/BlueZ noise.
Find the root cause. Compare with how audio was configured/working before Waydroid: repo
history (git log/diff for audio, UCM, wireplumber, pipewire configs under os/, rootfs/,
power/ and the zrn abox driver/module), Phase 1/2 changes in android/waydroid/ (install.sh,
network.py, recover.sh, patch-vendor.sh, ztos-waydroid-*.service, ztos-android helpers,
freeze/unfreeze of the container, waydroid package install pulling deps like
pulseaudio/pipewire config, ALSA/UCM files, udev rules, group changes), and zrn-sleepd/SoC
sleep interaction (audio may break after SoC sleep resume — check whether a module/codec
needs re-init). Test playback directly: `aplay`/`pw-play` a wav to the speaker, ztOS system
sounds, and audio from a Waydroid app (Android -> pulse socket). Fix the root cause in the
repo and on the phone, verify audible playback state (pcm status RUNNING, stream not [init],
pw-top/wpctl), across a pipewire restart and, if safe, a reboot (the phone self-recovers via
greetd). Write results to android/waydroid/AUDIO.md.
