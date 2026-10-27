#!/usr/bin/env python3
"""Backport composer fixes for the tested 20260927 arm64 binary and fixed size.

Upstream removes configureRpcThreadpool(1, true) from hwc_binder_thread.
The composer service already configures four threads. Replace only that BL
instruction with AArch64 NOP; reject any other binary before writing.

Also suppress configure-time hotplug for this fixed-resolution deployment.
With persist.waydroid.width and height fixed, choose_width_height always selects
those same dimensions. This is the fixed-size case of upstream 5af0c11.
Initial display registration remains intact. Resolution changes require a
session restart. Do not use this patch for dynamically sized displays.
https://github.com/waydroid/android_hardware_waydroid/commit/5af0c114a19fc916de837ad553524cd2aade2719
https://github.com/waydroid/android_hardware_waydroid/commit/4aee4c8d953e5cf7cb58d838242b71899a51269a
"""
import hashlib
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
nop = bytes.fromhex('1f2003d5')
patches = {
    0x3EEE8: bytes.fromhex('deb30094'),  # configureRpcThreadpool(1, true)
    0x40C70: bytes.fromhex('e8ac0094'),  # wl_shell configure: do_hotplug
    0x42E40: bytes.fromhex('74a40094'),  # xdg_toplevel configure: do_hotplug
}
stock_hash = 'c5847fd6b1228afb803f36a0818d037ce65369e6b95e96d85666b67f2158463f'
data = bytearray(path.read_bytes())
check = data.copy()
for offset, original in patches.items():
    if data[offset:offset + 4] not in (original, nop):
        sys.exit('Unsupported composer instructions; no changes made.')
    check[offset:offset + 4] = original
if hashlib.sha256(check).hexdigest() != stock_hash:
    sys.exit('Unsupported composer binary; no changes made. Rebuild the upstream fixes for this vendor image.')
# Modify in place to preserve ownership, mode and SELinux xattrs inside ext4.
with path.open('r+b') as output:
    for offset in patches:
        output.seek(offset)
        output.write(nop)
print('Composer fixes applied; SHA256=' + hashlib.sha256(path.read_bytes()).hexdigest())
