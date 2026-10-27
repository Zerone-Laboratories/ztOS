# Bug: audio crackles since Waydroid was installed

Read android/waydroid/AUDIO.md and audio/README.md first. Phone access:
`python /tmp/ztos-phone-control.py run|sudo|put|get`; two failed logins = stop. User session
env XDG_RUNTIME_DIR=/run/user/1000. Another agent (WHATSAPP-DUAL-TASK.md) may still be using
ztMETRO/ztos-android: do not edit ztmetro/ztmetro.c or ztos-android, do not restart ztMETRO.
Phone is often unattended: fixes must survive reboot. ztOS rules: tune for smooth use on the
four efficiency cores; no always-on polling.

Correction from the user: audio was NOT silent; it plays but CRACKLES since Waydroid was
installed. The previous agent's "silence" theory is wrong for this report.

Current config: PipeWire 1.6.9, rate 48000 only, quantum 2048, min-quantum 512, max 2048;
ABOX driver 512-frame periods x8, 256-frame headroom, timer scheduling disabled (see
history 6309064, 7d7357b); cpufreq schedutil on all 8 cores; zrnperfd boosts on focus.

Crackle = underruns/xruns or resampling/clock problems. Measure first, then fix:
1. Reproduce with pw-top -b (ERR column, QUANT, B/Q), ALSA xrun counts
   (/proc/asound/card0/pcm0p/sub0/status, xrun_debug if available), during: host playback
   (pw-play a 30 s music WAV, the ztOS music app if quick, system sounds) with the Waydroid
   container (a) stopped, (b) frozen, (c) running idle, (d) running with an Android app
   playing; and Android playback alone. Record which states crackle.
2. Likely suspects: Waydroid's Pulse client forcing a small quantum (512 = 10.7 ms) on the
   whole graph, Android at 44.1 kHz being resampled, CPU contention from the container
   (Android services on the little cores, cgroup cpu weight), the audio IRQ/PipeWire threads
   losing RT priority (rtkit/limits), schedutil dropping clocks, zrnperfd freq changes,
   period interrupt latency in snd-zrn-abox.
3. Fix the root cause with the smallest change (e.g. pulse.min.quantum / node.latency rules
   for Waydroid's client in a pipewire-pulse.conf.d or wireplumber rule, RT priority, container
   cpu.weight, resampler quality), deploy, and show before/after numbers (ERR counts, xruns)
   for the same scenarios. Keep keyboard click latency responsive (see 7d7357b).
Write results to android/waydroid/AUDIO.md (new section "Crackle").
