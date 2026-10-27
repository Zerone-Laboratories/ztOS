# Bluetooth controller broken
Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Phone is often unattended: fixes must survive reboot.
User: "bluetooth controller was broken" (today, 2026-10-24). Read repo history for Bluetooth
(git log -- '*bt*' '*bluetooth*' rootfs/ os/ power/; zrn-bluetooth, zrn-bluetooth-addr,
memory notes: BT HOST_WAKE wake source, zrn-sleepd keeps the chip powered in sleep). Today the
phone had many forced resets, TWRP passes, a zrnperfd CPU spin, Waydroid installed (its own
bluetooth HAL/audio dlopen attempts), cpuidle off then on. Find the current state
(bluetoothctl show/list, hciconfig -a, journalctl -u bluetooth -u zrn-bluetooth -b, dmesg for
hci/uart/brcm, rfkill), the root cause, fix it durably, and verify: controller powered, scan finds
devices, survives a reboot and a screen-off/SoC-sleep cycle. Write power/BT-FIX.md.
