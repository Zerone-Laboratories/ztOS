#!/usr/bin/env python3
"""Battery-only C2 A/B and automatic Wi-Fi sleep measurement.

Run as root in a detached systemd service. Waits for unplug, blanks the
screen, samples for 390 seconds, and restores CPU idle settings on exit.
Only seconds 60..120 disable C2. Never changes firmware masks or radios.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import time
import sys

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

BATTERY = Path("/sys/class/power_supply/max170xx_battery")
STATUS = Path("/run/zrnperfd.status")
CTL = ztos_user.runtime('ztmetro.ctl')


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def status():
    return dict(line.split("=", 1) for line in (read(STATUS) or "").splitlines()
                if "=" in line)


def display_state():
    return {str(p): read(p) for p in Path("/sys/class/drm").glob("card*-*/enabled")}


def display_is_off(daemon, outputs):
    return (daemon.get("screen") == "off" and bool(outputs)
            and all(value == "disabled" for value in outputs.values()))


def plugged():
    return any(read(p) == "1" for p in Path("/sys/class/power_supply").glob("*/online"))


def screen(value):
    fd = os.open(CTL, os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, f"screen {value}\n".encode())
    finally:
        os.close(fd)


def interrupted(signum, frame):
    raise SystemExit(f"stopped by signal {signum}")


def run(log):
    def record(**fields):
        print(json.dumps({"time": time.time(), **fields}), file=log, flush=True)

    record(event="waiting for unplug", timeout_s=600)
    deadline = time.monotonic() + 600
    while plugged():
        if time.monotonic() >= deadline:
            record(event="cancelled: cable still attached")
            return
        time.sleep(1)
    settings = {p: read(p) for p in Path("/sys/devices/system/cpu").glob(
        "cpu[0-9]*/cpuidle/state1/disable")}
    if not settings:
        raise RuntimeError("C2 driver is not loaded")
    previous_screen = status().get("screen", "on")
    phase = None
    try:
        screen("off")
        timeout = time.monotonic() + 15
        while not display_is_off(status(), display_state()):
            if time.monotonic() >= timeout:
                record(event="aborted: display shutdown not confirmed", daemon=status(),
                       display=display_state())
                return
            time.sleep(0.2)
        start = time.monotonic()
        record(event="unplugged", idle_settings={str(p): v for p, v in settings.items()})
        while time.monotonic() - start < 390:
            elapsed = time.monotonic() - start
            if plugged():
                record(event="ended: cable reconnected", elapsed_s=elapsed)
                break
            current_phase = "C2 disabled" if 60 <= elapsed < 120 else "C2 enabled"
            if current_phase != phase:
                for path, original in settings.items():
                    path.write_text("1" if current_phase == "C2 disabled" else original)
                phase = current_phase
                record(event="phase", phase=phase, elapsed_s=elapsed)
            fields = {name: read(BATTERY / name) for name in
                      ("capacity", "voltage_now", "current_now", "current_avg", "temp")}
            daemon, outputs = status(), display_state()
            record(event="sample", elapsed_s=round(elapsed, 1), phase=phase,
                   battery=fields, daemon=daemon, display=outputs,
                   valid_screen_off=display_is_off(daemon, outputs),
                   online=read("/sys/devices/system/cpu/online"))
            time.sleep(5)
        record(event="finished")
    finally:
        for path, original in settings.items():
            try:
                path.write_text(original)
            except OSError as error:
                record(event="restore failed", path=str(path), error=str(error))
        if plugged():
            try:
                screen(previous_screen)
            except OSError as error:
                record(event="screen restore failed", error=str(error))
        else:
            # Finishing the measurement is not a reason to light the phone.
            # The power key remains available for the user to wake it.
            record(event="screen restore deferred while on battery")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, default=Path("/var/tmp/ztos-idle-ab.jsonl"))
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit("run as root")
    for signum in (signal.SIGTERM, signal.SIGINT):
        signal.signal(signum, interrupted)
    with args.log.open("w", buffering=1) as log:
        run(log)
