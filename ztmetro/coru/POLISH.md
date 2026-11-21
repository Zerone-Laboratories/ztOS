# Sensor hub failure containment, 2026-10-25

The previous recovery loop restarted lhd twice but left the last daemon running
after showing the failure dialog. It now stops the hub before resetting power,
and stops the sensor and hub units after recovery attempts are exhausted.

The sensorhub service runs the user-space supervisor in
`rootfs/overlay/usr/local/sbin/ztoscsf-sensorhub --run`. Firmware gets a bounded
30-second startup window because lhd must download it before readiness exists.
After readiness, the supervisor checks every two seconds and exits on lost or
unreadable readiness. It terminates lhd on exit; systemd kills the control group,
with a five-second stop timeout. Automatic service restarts are disabled;
ztoscsf retains ownership of its two recovery attempts. No kernel code changed.

Local checks: C syntax, shell syntax, unit verification with executable paths
mapped to this checkout, and four mocked supervisor tests passed. The mocks
cover startup timeout, missing readiness, firmware crash, and normal stop.

Deployed helper, service, and CSF binary on the phone after saving each installed
file as `.before-polish`. `systemd-analyze verify --man=no` passed on the phone;
service remains enabled, with `Restart=no`, `KillMode=control-group`, and a
five-second stop timeout. The newly installed CSF binary takes effect on its
next normal start; the existing CSF process was not restarted.

Live test, 07:24:03–07:24:34: restarting only the user-space hub unit encountered
readiness 0 and lhd consuming about one core. At the deadline the supervisor
logged `firmware not ready after 30 seconds; stopping lhd`. No lhd process
survived; NRestarts stayed 0. The existing CSF process independently ran its
pre-existing two power retries; the final supervised lhd also exited at its
deadline. Both units were stopped afterward. No reboot, module operation,
kernel change, or manual sysfs reset was performed. Firmware recovery itself
remains outside this task; failed hardware now cannot leave lhd spinning.

Evidence: `evidence/sensor-failure-live.txt` and the final journal/status capture.
The four supervisor mock tests pass. `sensors/test-csf-failure.c` additionally
checks stop-before-reset ordering, two-attempt exhaustion, final stop of both
units, and suppression of repeated recovery, including failed reset commands.
All service/sysfs operations are mocked in that test.
