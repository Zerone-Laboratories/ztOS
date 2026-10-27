# The S9 vibration motor as a rumble device, with its own pulse timing
# (see zrn_haptic_main.c). No ff-memless any more.
obj-m += zrn-haptic.o
zrn-haptic-y := zrn_haptic_main.o
# zrn-haptic-dev.ko: the same source as a second driver name, to try a build
# beside the loaded one (bind it by hand; never rmmod).
obj-m += zrn-haptic-dev.o
zrn-haptic-dev-y := dev_main.o
CFLAGS_dev_main.o := -DZH_DEV
