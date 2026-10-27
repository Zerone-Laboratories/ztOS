"""The phone's user, for programs that run as root (or as anyone) and need
that user's home or runtime directory.

/etc/ztos/user.conf names the user (written at install and first boot:
ZTOS_USER, ZTOS_UID, ZTOS_GID, ZTOS_HOME). Without it, the passwd entry with
UID 1000, so a phone installed before the file existed keeps working.

    sys.path.insert(0, '/usr/lib/ztos/python')
    import ztos_user
    ztos_user.home('.config/ztos/android.conf')
"""
import os
import pwd
from pathlib import Path

CONF = os.environ.get('ZTOS_USER_CONF', '/etc/ztos/user.conf')


def _load():
    conf = {}
    try:
        with open(CONF) as stream:
            for line in stream:
                key, eq, value = line.strip().partition('=')
                if eq and not key.startswith('#'):
                    conf[key.strip()] = value.strip().strip('"\'')
    except OSError:
        pass
    # fill what the file left out from passwd: by name, else UID 1000
    try:
        entry = pwd.getpwnam(conf['ZTOS_USER']) if conf.get('ZTOS_USER') \
            else pwd.getpwuid(int(conf.get('ZTOS_UID', 1000)))
    except (KeyError, ValueError):
        entry = None
    user = conf.get('ZTOS_USER') or (entry.pw_name if entry else '')
    uid = int(conf.get('ZTOS_UID') or (entry.pw_uid if entry else 1000))
    gid = int(conf.get('ZTOS_GID') or (entry.pw_gid if entry else uid))
    home = conf.get('ZTOS_HOME') or (entry.pw_dir if entry else '/home/' + user)
    return user, uid, gid, home


USER, UID, GID, HOME = _load()
RUNTIME_DIR = '/run/user/%d' % UID
# the user's session bus, as DBUS_SESSION_BUS_ADDRESS
BUS = 'unix:path=%s/bus' % RUNTIME_DIR


def home(*parts):
    """A path in the user's home directory."""
    return Path(HOME, *parts)


def runtime(*parts):
    """A path in the user's runtime directory (/run/user/UID)."""
    return Path(RUNTIME_DIR, *parts)


def as_user(*command, **env):
    """argv that runs command as the user, with their runtime directory and
    session bus (and any other environment given), for subprocess as root."""
    extra = ['%s=%s' % item for item in env.items()]
    return ['runuser', '-u', USER, '--', 'env', 'XDG_RUNTIME_DIR=' + RUNTIME_DIR,
            'DBUS_SESSION_BUS_ADDRESS=' + BUS, *extra, *command]
