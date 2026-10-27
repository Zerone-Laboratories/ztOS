#!/usr/bin/env python3
"""Repair networking and freeze an idle Waydroid without waking frozen Android."""
import configparser
import json
import os
import pathlib
import subprocess
import signal
import sys
import time

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

LXC = ['-P', '/var/lib/waydroid/lxc', '-n', 'waydroid']


def run(args, **kw):
    return subprocess.run(args, timeout=8, text=True, capture_output=True, **kw)


def keep_awake_active():
    packages = keep_awake_packages()
    registry = ztos_user.home('.local/share/ztos/android/applications.json')
    try:
        apps = json.loads(registry.read_text())
    except (OSError, ValueError):
        return False
    return any(apps.get(package+'@waydroid', apps.get(package, {})).get('runtime', 'atl') == 'waydroid' for package in packages)


def keep_awake_packages():
    packages = set()
    try:
        for line in ztos_user.home('.config/ztos/android.conf').read_text().splitlines():
            key, separator, value = line.partition('=')
            if separator and key.strip() == 'keep_awake':
                packages.update(item.strip() for item in value.split(',') if item.strip())
    except OSError:
        pass
    try:
        entries = ztos_user.home('.config/ztos/background-apps').read_text().splitlines()
    except OSError:
        entries = []
    for entry in entries:
        item = entry.strip().rsplit('/', 1)[-1]
        if item.endswith('.desktop'):
            item = item[:-8]
        if item.startswith('waydroid.'):
            packages.add(item[9:])
        elif item.startswith('ztos-android-'):
            package = item[13:]
            if package.endswith('-waydroid'):
                package = package[:-9]
            elif package.endswith('-atl'):
                package = package[:-4]
            packages.add(package)
        elif item.endswith('-waydroid'):
            packages.add(item[:-9])
        elif '.' in item:
            packages.add(item)
    return packages


# IActivityManager.Stub.TRANSACTION_setProcessLimit in this image's framework.jar
# (LineageOS 20, Android 13; android/app/IActivityManager$Stub, 0x2c)
SET_PROCESS_LIMIT = 44

# Never restricted: what Android apps need to run at all (push, the store, typing)
INFRASTRUCTURE = ('com.google.android.gms', 'com.google.android.gsf', 'com.android.vending',
                  'org.microg.', 'com.android.inputmethod', 'com.google.android.inputmethod')

POLICY_FILES = (str(ztos_user.home('.config/ztos/android.conf')),
                str(ztos_user.home('.config/ztos/background-apps')))
applied_stamp = [None]


def policy_stamp():
    stamp = []
    for path in POLICY_FILES:
        try:
            stamp.append(pathlib.Path(path).stat().st_mtime_ns)
        except OSError:
            stamp.append(0)
    return tuple(stamp)


def background_policy(attach):
    """Background rights for the apps the user chose (Settings > background
    apps, or keep_awake in android.conf); every other installed app is
    restricted: no background work, the restricted standby bucket, stopped."""
    stamp = policy_stamp()
    keep = keep_awake_packages()
    listed = run([*attach[:-1], attach[-1] + 'pm', 'list', 'packages', '-3'])
    if listed.returncode:
        raise RuntimeError(listed.stderr.strip())
    packages = [line[8:].strip() for line in listed.stdout.splitlines() if line.startswith('package:')]
    for package in packages:
        if package.startswith(INFRASTRUCTURE):
            continue
        # WhatsApp links through Instagram's FoaBackupTokenProvider: Instagram
        # keeps provider starts while WhatsApp is kept awake.
        allowed = package in keep or (package == 'com.instagram.android' and 'com.whatsapp' in keep)
        if package in keep:
            # A kept app lives on its notifications. WhatsApp also posts its
            # foreground-service notification with no channel when notifications
            # are off (it never makes its channels), and Android kills it for that:
            # opening a chat or linking a device crashed it (2026-10-25).
            run([*attach[:-1], attach[-1] + 'pm', 'grant', package, 'android.permission.POST_NOTIFICATIONS'])
            run([*attach[:-1], attach[-1] + 'pm', 'set-permission-flags', package,
                 'android.permission.POST_NOTIFICATIONS', 'user-set'])
            run([*attach[:-1], attach[-1] + 'appops', 'set', package, 'POST_NOTIFICATION', 'allow'])
        for operation in ('RUN_IN_BACKGROUND', 'RUN_ANY_IN_BACKGROUND'):
            result = run([*attach[:-1], attach[-1] + 'appops', 'set', package, operation,
                          'default' if allowed else 'ignore'])
            if result.returncode:
                raise RuntimeError(result.stderr.strip())
        result = run([*attach[:-1], attach[-1] + 'am', 'set-standby-bucket', package,
                      'active' if allowed else 'restricted'])
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
    applied_stamp[0] = stamp
    print(f'background apps: {", ".join(sorted(keep)) or "none"}; '
          f'{len(packages)} installed, the rest restricted', flush=True)


# Android's SystemUI leaks in Waydroid: at 770 MB (580 MB resident, 194 MB in
# swap, normally ~100 MB) it had filled RAM and swap, and everything starved:
# music cut in and out, Android hung in reclaim (2026-10-25). It restarts by
# itself when killed, at ~95 MB.
SYSTEMUI_LIMIT_KB = 350 * 1024
systemui_checked = [0.0]


def systemui_guard():
    now = time.monotonic()
    if now - systemui_checked[0] < 60:
        return
    systemui_checked[0] = now
    for proc in pathlib.Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if proc.joinpath('comm').read_text().strip() != 'ndroid.systemui':
                continue
            status = proc.joinpath('status').read_text()
        except OSError:
            continue
        rss = next((int(line.split()[1]) for line in status.splitlines() if line.startswith('VmRSS:')), 0)
        swap = next((int(line.split()[1]) for line in status.splitlines() if line.startswith('VmSwap:')), 0)
        if rss + swap > SYSTEMUI_LIMIT_KB:
            os.kill(int(proc.name), signal.SIGKILL)
            print(f'SystemUI at {(rss + swap) // 1024} MB: restarted it', flush=True)


# An app reinstalled gets a new uid, but its folders under Android/media,
# data and obb keep the old owner: the app can no longer write them, so
# WhatsApp could not save a download or a voice note (its media folder still
# belonged to the previous install's 10121, the app was 10126; 2026-10-26).
ANDROID_DATA = ztos_user.home('.local/share/waydroid/data')
storage_checked = [0.0]


def storage_owner_repair():
    now = time.monotonic()
    if now - storage_checked[0] < 60:
        return
    storage_checked[0] = now
    try:
        uids = {}
        for line in ANDROID_DATA.joinpath('system/packages.list').read_text().splitlines():
            fields = line.split()
            if len(fields) > 1 and fields[1].isdigit():
                uids[fields[0]] = int(fields[1])
    except OSError:
        return
    for kind in ('media', 'data', 'obb'):
        base = ANDROID_DATA / 'media/0/Android' / kind
        try:
            folders = list(base.iterdir())
        except OSError:
            continue
        for folder in folders:
            uid = uids.get(folder.name)
            try:
                if uid is None or folder.stat().st_uid == uid:
                    continue
                for root, dirs, files in os.walk(folder):
                    for name in [root, *(os.path.join(root, n) for n in dirs + files)]:
                        os.lchown(name, uid, -1)
            except OSError:
                continue
            print(f'Android/{kind}/{folder.name}: owner set to its app ({uid})', flush=True)


# The phone's user reads and writes Android's shared storage (Download, DCIM,
# Pictures...) through ~/Android in Files: the folders belong to Android's
# media_rw (1023, mode 0770), so the user gets an ACL on them, inherited by what
# Android creates later. Applied to the top at every Android boot; the tree
# below got it once (setfacl -R, 2026-10-26).
USER_HOME = ztos_user.home()


def storage_user_access():
    media = ANDROID_DATA / 'media'
    if not media.joinpath('0').is_dir():
        return
    user = 'u:%d:' % ztos_user.UID
    run(['setfacl', '-m', user + 'x', str(media)])
    run(['setfacl', '-m', user + 'rwX', str(media / '0')])
    run(['setfacl', '-d', '-m', user + 'rwX,m::rwx', str(media / '0')])
    link = USER_HOME / 'Android'
    if not link.exists() and not link.is_symlink():
        link.symlink_to('.local/share/waydroid/data/media/0')
        os.lchown(link, ztos_user.UID, ztos_user.GID)


# Apps installed inside Android (Aurora Store) join the ztOS app list: the
# user's ztos-android registry is brought in line with Waydroid's launcher
# files once a minute.
apps_synced = [0.0]


def app_list_sync():
    now = time.monotonic()
    if now - apps_synced[0] < 60:
        return
    apps_synced[0] = now
    result = run(['runuser', '-u', ztos_user.USER, '--', 'env', 'HOME=' + ztos_user.HOME,
                  'XDG_RUNTIME_DIR=' + ztos_user.RUNTIME_DIR, '/usr/local/bin/ztos-android', 'sync'])
    if result.stdout.strip():
        print('app list: ' + ' '.join(result.stdout.split()), flush=True)


def launch_in_progress():
    marker = ztos_user.runtime('ztos-waydroid-launching')
    try:
        pid = int(marker.read_text().strip())
        pathlib.Path(f'/proc/{pid}').stat()
        return True
    except (OSError, ValueError):
        marker.unlink(missing_ok=True)
        return False


def maintain():
    if not pathlib.Path('/sys/class/net/waydroid0').exists():
        return
    pathlib.Path('/proc/sys/net/ipv4/ip_forward').write_text('1\n')
    # Upstream waydroid-net.sh owns bridge/DHCP/DNS and its lxc tables.
    # Own a separate table so a firewall reload can be repaired without flushing it.
    if run(['nft', 'list', 'table', 'ip', 'ztos_waydroid']).returncode:
        rules = '''table ip ztos_waydroid {
 chain postrouting {
  type nat hook postrouting priority 101; policy accept;
  ip saddr 192.168.240.0/24 ip daddr != 192.168.240.0/24 counter masquerade
 }
}
'''
        result = run(['nft', '-f', '-'], input=rules)
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
    state = run(['lxc-info', *LXC, '-sH']).stdout.strip()
    if state != 'RUNNING':
        idle.clear()
        return  # Never attach to or thaw FROZEN containers from this service.
    pid = run(['lxc-info', *LXC, '-pH']).stdout.strip()
    if not pid.isdigit():
        return
    ns = ['nsenter', '-t', pid, '-n', '--']
    rules = run([*ns, 'ip', '-4', 'rule', 'show'])
    if rules.returncode:
        # This kernel has no IPv4 policy routing. Android netd cannot add routes.
        address = run([*ns, 'ip', '-4', 'addr', 'show', 'dev', 'eth0']).stdout
        if '192.168.240.' in address:
            route = run([*ns, 'ip', '-4', 'route', 'show', 'default']).stdout
            if 'via 192.168.240.1 dev eth0' not in route:
                result = run([*ns, 'ip', 'route', 'replace', 'default',
                              'via', '192.168.240.1', 'dev', 'eth0'])
                if result.returncode:
                    raise RuntimeError(result.stderr.strip())
    # Framework settings are persistent; write only once per Android boot, after ready.
    attach = ['lxc-attach', *LXC, '--clear-env', '-v', 'PATH=/system/bin:/system/xbin:/vendor/bin', '--', '/system/bin/']
    if pid not in configured:
        if run([*attach[:-1], attach[-1] + 'getprop', 'sys.boot_completed']).stdout.strip() == '1':
            result = run([*attach[:-1], attach[-1] + 'settings', 'put', 'global',
                          'restricted_networking_mode', '0'])
            if not result.returncode:
                # Invisible ANR dialogs can steal all input in multi-window mode.
                result = run([*attach[:-1], attach[-1] + 'settings', 'put', 'global',
                              'hide_error_dialogs', '1'])
                if result.returncode:
                    raise RuntimeError(result.stderr.strip())
                result = run([*attach[:-1], attach[-1] + 'settings', 'put', 'system',
                              'screen_off_timeout', '15000'])
                if result.returncode:
                    raise RuntimeError(result.stderr.strip())
                config = configparser.ConfigParser()
                config.read('/var/lib/waydroid/waydroid.cfg')
                for key, value in config.items('properties'):
                    if key.startswith('persist.waydroid.') or key == 'persist.traced.enable':
                        result = run([*attach[:-1], attach[-1] + 'setprop', key, value])
                        if result.returncode:
                            raise RuntimeError(result.stderr.strip())
                # Keep a small cache, no cached background processes at all (the
                # developer option "No background processes"; Android forgets it
                # at every boot), and background rights for the user's apps only.
                result = run([*attach[:-1], attach[-1] + 'cmd', 'device_config',
                              'put', 'activity_manager', 'max_cached_processes', '16'])
                if result.returncode:
                    raise RuntimeError(result.stderr.strip())
                result = run([*attach[:-1], attach[-1] + 'service', 'call', 'activity',
                              str(SET_PROCESS_LIMIT), 'i32', '0'])
                if result.returncode:
                    raise RuntimeError(result.stderr.strip())
                background_policy(attach)
                storage_user_access()
                configured.clear()
                configured.add(pid)
                print(f'Android PID {pid}: network ready; restricted networking off', flush=True)
    if pid in configured:
        systemui_guard()
        storage_owner_repair()
        app_list_sync()
    if pid in configured and policy_stamp() != applied_stamp[0]:
        background_policy(attach)
    if pid in configured:
        # A messaging exemption vetoes freezing regardless of window state.
        # Avoid two Android process launches every five seconds while it is set.
        if keep_awake_active():
            idle.clear()
            return
        # HWC publishes the count of native Android windows, including unfocused ones.
        # Do not infer visibility from the foreground activity or host focus.
        windows = run([*attach[:-1], attach[-1] + 'getprop', 'waydroid.open_windows']).stdout.strip()
        suspend = run([*attach[:-1], attach[-1] + 'getprop', 'persist.waydroid.suspend']).stdout.strip()
        if windows == '0' and suspend == 'true' and not launch_in_progress():
            since = idle.setdefault(pid, time.monotonic())
            if time.monotonic() - since >= 15:
                result = run(['waydroid', 'container', 'freeze'])
                if result.returncode:
                    raise RuntimeError(result.stderr.strip())
                idle.clear()
                print(f'Android PID {pid}: frozen after 15s with zero HWC windows', flush=True)
        else:
            idle.clear()


configured = set()
idle = {}
while True:
    try:
        maintain()
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        print(f'Network repair: {error}', flush=True)
    time.sleep(5)
