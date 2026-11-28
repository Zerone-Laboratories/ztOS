#!/usr/bin/env python3
"""prune-missing-services.py: drop init.waydroid.rc services the vendor lacks.

Waydroid's system image declares, in /system/etc/init/init.waydroid.rc, the
HALs of every vendor image it may be paired with ("override" services such as
vendor.audio-hal-2-0 for android.hardware.audio@2.0-service and
vendor.camera-provider-2-4). A vendor image without that binary (the x86_64
MAINLINE vendor has android.hardware.audio.service and the 2.7 external camera
provider only) makes init log, on every Android start and every audioserver
restart (ztOS's audio reconnect restarts it):

  Could not start service 'vendor.audio-hal-2-0' as part of class 'hal':
    Cannot find '/vendor/bin/hw/android.hardware.audio@2.0-service'
  Could not restart 'vendor.audio-hal-2-0': Cannot find ...

Such a service can never start, so dropping its section changes nothing but
the log. Services whose binary the vendor has are kept as they are (the HAL
Android really uses, vendor.audio-hal, stays).

  prune-missing-services.py RC VENDOR_IMAGE   exit 0: done (or nothing to drop)

RC is edited in place, so its mode, owner and SELinux label stay. VENDOR_IMAGE
is read with debugfs only.
"""
import pathlib
import subprocess
import sys

SECTIONS = ('service', 'on', 'import')


def vendor_has(image, path):
    # the vendor image's root is /vendor: /vendor/bin/hw/x is /bin/hw/x in it
    inner = path[len('/vendor'):]
    out = subprocess.run(['debugfs', '-R', f'stat {inner}', str(image)],
                         capture_output=True, text=True).stdout
    return 'Inode:' in out


def prune(text, has):
    kept, dropped, section = [], [], None
    for line in text.splitlines(keepends=True):
        words = line.split()
        if words and words[0] in SECTIONS:
            section = None
            if words[0] == 'service' and len(words) >= 3 and words[2].startswith('/vendor/') \
                    and not has(words[2]):
                section = words[1]
                dropped.append((words[1], words[2]))
        if section is None:
            kept.append(line)
    return ''.join(kept), dropped


def main():
    if len(sys.argv) != 3:
        print('usage: prune-missing-services.py RC VENDOR_IMAGE', file=sys.stderr)
        return 2
    rc, image = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    if not rc.is_file() or not image.is_file():
        print(f'{rc} or {image} is missing: nothing pruned', file=sys.stderr)
        return 0
    new, dropped = prune(rc.read_text(), lambda p: vendor_has(image, p))
    for name, path in dropped:
        print(f'dropped service {name}: the vendor has no {path}')
    if dropped:
        with rc.open('r+') as stream:      # in place: the inode, its label and mode stay
            stream.write(new)
            stream.truncate()
    return 0


if __name__ == '__main__':
    sys.exit(main())
