# ztOSIAL

**ztOSIAL** is the Zerone Technologies Operating System Interface Abstraction Layer.

The interfaces between the ztOS system image (GSI) and a phone's own hardware.
Design and reasons: `design/ztOSIAL.md`. Licence: LGPL-2.1-or-later.

| Part | Where it installs |
|---|---|
| `ztos-iald`, the daemon that serves `org.ztos.ial` | `/usr/bin/ztos-iald`, `ztos-iald.service` |
| `ztos-ial`, the command line | `/usr/bin/ztos-ial` |
| the interfaces, one introspection file each | `/usr/share/ztos-ial/spec/`, `/usr/share/dbus-1/interfaces/` |
| the generic backends and the Python client | `/usr/lib/ztos-ial/ztos_ial/` |
| `libztos-ial`, the C client | `/usr/lib/libztos-ial.so`, `ztos-ial.h`, `ztos-ial.pc` |
| what this GSI serves and needs | `/usr/share/ztos/gsi.json` |

## Porting a phone

A device layer is a manifest, `/vendor/ztos/manifest.toml`, and whatever it
names. Start with no manifest at all: every interface uses its generic
backend, which is enough on a phone with full mainline support. Then:

1. `ztos-ial status` and `ztos-ial test` on the phone show what works.
2. Where a generic backend guesses wrong, give it parameters in the
   manifest (`[ial.Display1] node = ...`). The parameters each backend takes
   are at the top of its class in `ztos_ial/backends/generic.py`.
3. Where parameters are not enough, write a backend: a Python file in the
   device layer, `backend = "vendor"` and `module = "/vendor/ztos/ial/<file>.py"`,
   with a class named after the interface without its version (`Display` for
   `Display1`), usually a subclass of the generic one. The Galaxy S9's
   (`devices/samsung-starlte/vendor/ztos/ial/starlte.py`) is the example.
4. A backend in another language is a service: it owns
   `org.ztos.ial.<Area>` itself and implements the spec;
   `backend = "service"`, `service = "<unit>"`.
5. `ztos-ial check` before shipping (ztConflo runs it on every image).

A backend never needs to know the shell: it answers properties and methods,
raises `NotSupported` for what the phone cannot do, and says whether the
hardware is there (`available()`), which becomes `Device1.Capabilities`.

## Perf1 topology

`Perf1.Topology` (and the file at `TopologyPath`, for daemons without D-Bus):

```json
{
 "clusters": [{"policy": "policy4", "cpus": [4, 5, 6, 7], "freqs_khz": [...],
               "caps_by_online": {"1": 2704000, "2": 2314000, "4": 1794000}}],
 "keep_online": [0, 1, 4],
 "idle_mask": "0x0c",
 "hotplug": true,
 "thermal": {"big": "/sys/.../temp_big"},
 "thermal_unit": "celsius",
 "thermal_marks": [70, 76, 82],
 "gpu_devfreq": "/sys/class/devfreq/17500000.gpu",
 "battery_current_node": "/sys/class/power_supply/max170xx_battery/current_now"
}
```

`caps_by_online` is a safety limit, not a preference: a policy must never run
the cluster above it with that many cores online.
