SYS_SLEEP runtime files for the Galaxy S9's 7.1.0-zboot kernel:

- `zrn-sleep-init.ko`: built from `power/suspend`.
- `zrn-exynos-rtc.ko`: built from `power/rtc`.
- `exynos9810_acpm_tmu.fw`: Samsung TMU plugin extracted with
  `power/suspend/extract-acpm-tmu.py` (5,120 bytes).

The installer copies these and the calendar/ACPM helpers to
`/usr/local/libexec/ztos-power/mif-trial`. The sleep daemon is enabled as a
service; automatic sleep remains opt-in with `sudo zrn-sleepd enable`.
The daemon keeps MIF sleep disabled and refuses sleep while charging.
Update these modules and `prebuilts/modules/zrn-hotplug.ko` together when
changing the kernel or suspend implementation.
