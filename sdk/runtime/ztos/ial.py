# SPDX-License-Identifier: LGPL-2.1-or-later
"""ztos.ial: the phone's hardware, the same on every phone ztOS runs on.

    from ztos import ial

    ial.get('Power1', 'Capacity')          # the battery, 0..100
    ial.call('Haptics1', 'Play', 'tap')    # a short buzz
    if ial.has('Modem1'): ...              # only where there is a modem

The interfaces are ztOSIAL's (design/ztOSIAL.md, ial/spec/). A call the phone
cannot do raises ial.Error with .name 'org.ztos.ial.Error.NotSupported'.
"""
import sys

if '/usr/lib/ztos-ial' not in sys.path:
    sys.path.insert(0, '/usr/lib/ztos-ial')

from ztos_ial.client import Error, call, get, get_all, has  # noqa: E402,F401
