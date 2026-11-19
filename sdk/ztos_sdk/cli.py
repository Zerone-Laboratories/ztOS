# SPDX-License-Identifier: GPL-2.0-or-later
"""ztos-sdk: make ztOS apps in Python and Qt, and put them on the phone.

    ztos-sdk new Notes              a new app, in ./notes
    ztos-sdk run                    try it here, at the phone's size
    ztos-sdk designer [form.ui]     draw a page in Qt Designer
    ztos-sdk build                  dist/<id>: one file the phone runs
    ztos-sdk install                build it, put it on the phone, list it in the apps
    ztos-sdk deploy                 install it, then open it on the phone
    ztos-sdk launch | logs | uninstall
    ztos-sdk list                   the SDK's apps on the phone
    ztos-sdk doctor [--fix]         is the phone ready to run them?

The phone is alarm@172.16.42.1 (USB) unless --device, $ZTOS_DEVICE, or
[device] host in ztos.toml say otherwise. ssh asks for the password, or takes
it from $ZTOS_PASSWORD; `doctor --fix` takes root's from $ZTOS_ROOT_PASSWORD.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import platform
import fnmatch
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import tomllib
import zipfile
from pathlib import Path

SDK = Path(__file__).resolve().parent.parent
RUNTIME = SDK / "runtime"
TEMPLATE = SDK / "templates" / "app"
LAUNCHERS = SDK / "launcher" / "prebuilt"
DEFAULT_DEVICE = "alarm@172.16.42.1"
DEFAULT_INCLUDE = ["**/*.py", "**/*.ui", "**/*.svg", "**/*.png", "**/*.jpg", "**/*.json", "**/*.txt",
                   "**/*.qss", "**/*.ttf", "**/*.otf", "**/*.wav", "**/*.ogg"]
SKIP_DIRS = {"dist", "build", "__pycache__", ".git", ".venv", "venv", ".ztos"}
# where an app lives on the phone, under the alarm user's home
APPS_DIR = ".local/share/ztos/apps"
LOG_DIR = ".local/state/ztos"


class Fail(Exception):
    pass


def say(*args):
    print("ztos-sdk:", *args, flush=True)


# ---- the project ---------------------------------------------------------------

class Project:
    def __init__(self, root: Path):
        self.root = root
        try:
            data = tomllib.loads((root / "ztos.toml").read_text())
        except FileNotFoundError:
            raise Fail(f"no ztos.toml in {root} (make an app with: ztos-sdk new NAME)")
        except tomllib.TOMLDecodeError as e:
            raise Fail(f"ztos.toml: {e}")
        app = data.get("app", {})
        self.build_cfg = data.get("build", {})
        self.device_cfg = data.get("device", {})
        self.id = app.get("id", "")
        self.name = app.get("name", "")
        if not re.fullmatch(r"[a-z][a-z0-9_-]*(\.[a-z0-9_-]+)+", self.id):
            raise Fail(f'ztos.toml: id "{self.id}" should be like "com.example.notes" (lower case, dots)')
        if self.id.startswith(("zt", "zrn")):
            raise Fail('ztos.toml: ids starting "zt" or "zrn" are the system\'s own apps')
        if not self.name:
            raise Fail("ztos.toml: the app needs a name")
        self.version = str(app.get("version", "0.1.0"))
        # system packages this app needs on the phone besides Python and Qt
        self.requires = [str(p) for p in app.get("requires", [])]
        bad = [p for p in self.requires if not re.fullmatch(r"[a-z0-9@._+-]+", p)]
        if bad:
            raise Fail(f"ztos.toml: requires: {bad[0]!r} is not a package name")
        self.entry = app.get("entry", "main.py")
        self.icon = app.get("icon", "icon.svg")
        self.manifest = {
            "id": self.id, "name": self.name, "version": self.version,
            "caps": app.get("caps", self.name.upper()), "entry": self.entry,
            "description": app.get("description", ""),
            "requires": self.requires,
            "performance": app.get("performance", "normal"),
        }
        if self.manifest["performance"] not in ("normal", "eco", "big"):
            raise Fail('ztos.toml: performance is "normal", "eco" or "big"')
        if not (root / self.entry).is_file():
            raise Fail(f"ztos.toml: the entry {self.entry} is not there")

    def files(self) -> list:
        include = self.build_cfg.get("include", DEFAULT_INCLUDE)
        exclude = self.build_cfg.get("exclude", [])
        found = []
        for path in sorted(self.root.rglob("*")):
            rel = path.relative_to(self.root)
            if not path.is_file() or any(part in SKIP_DIRS for part in rel.parts):
                continue
            name = rel.as_posix()
            match = lambda pattern: fnmatch.fnmatch(name, pattern) or \
                (pattern.startswith("**/") and fnmatch.fnmatch(name, pattern[3:]))
            if any(match(p) for p in include) and not any(match(p) for p in exclude):
                found.append(rel)
        return found


def find_project(path: str | None) -> Project:
    here = Path(path or ".").resolve()
    for d in [here, *here.parents]:
        if (d / "ztos.toml").is_file():
            return Project(d)
    raise Fail(f"no ztos.toml in {here} or above it (make an app with: ztos-sdk new NAME)")


# ---- new -------------------------------------------------------------------------

def cmd_new(args):
    name = args.name.strip()
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "app"
    target = Path(args.dir or slug).resolve()
    if target.exists() and any(target.iterdir()):
        raise Fail(f"{target} is there already, and not empty")
    app_id = args.id or f"com.example.{slug.replace('-', '')}"
    shutil.copytree(TEMPLATE, target, dirs_exist_ok=True)
    for path in target.rglob("*"):
        if path.is_file() and path.suffix in (".toml", ".py", ".md", ".ui"):
            text = path.read_text()
            text = text.replace("@NAME@", name).replace("@ID@", app_id).replace("@CAPS@", name.upper())
            path.write_text(text)
    say(f"made {target}")
    print(f"\n  cd {os.path.relpath(target)}\n  ztos-sdk run        # try it here\n  ztos-sdk deploy     # put it on the phone\n")


# ---- run (on this computer) ------------------------------------------------------------

def app_env(project: Project) -> dict:
    env = dict(os.environ)
    env["ZTOS_MANIFEST"] = json.dumps(project.manifest)
    env["ZTOS_APP_ROOT"] = str(project.root)
    env["PYTHONPATH"] = os.pathsep.join([str(project.root), str(RUNTIME), env.get("PYTHONPATH", "")])
    return env


def cmd_run(args):
    project = find_project(args.project)
    native_library("host")
    env = app_env(project)
    if args.shot:
        env["ZTOS_SHOT"] = str(Path(args.shot).resolve())
        env.setdefault("QT_QPA_PLATFORM", "offscreen")
        if args.offscreen:
            env["QT_QPA_PLATFORM"] = "offscreen"
    say(f"running {project.name} here at the phone's size (Escape is the back key)")
    return subprocess.call([sys.executable, str(project.root / project.entry), *args.args], env=env,
                           cwd=project.root)


def cmd_designer(args):
    project = find_project(args.project)
    designer = shutil.which("designer6") or shutil.which("pyside6-designer") or shutil.which("designer")
    if not designer:
        raise Fail("Qt Designer is not installed (pacman -S qt6-tools)")
    forms = [str(project.root / args.form)] if args.form else [str(project.root / f) for f in project.files()
                                                             if f.suffix == ".ui"]
    say("drawing with", designer, *forms)
    subprocess.Popen([designer, *forms], cwd=project.root, start_new_session=True)


# ---- build -----------------------------------------------------------------------------

BOOTSTRAP = '''\
# Made by ztos-sdk: starts the app inside this file.
import json, os, sys, zipfile
archive = os.path.dirname(os.path.abspath(__file__))
with zipfile.ZipFile(archive) as z:
    info = json.loads(z.read("ztos.json"))
os.environ["ZTOS_MANIFEST"] = json.dumps(info)
os.environ["ZTOS_APP_ROOT"] = archive + "!app"
sys.path[:0] = [archive + "/app", archive + "/lib"]
# started from the warm runtime: its ztos is used if it is the very same as ours
if "ztos" in sys.modules:
    import hashlib
    try:
        with zipfile.ZipFile(archive) as z:
            ours = hashlib.sha256(z.read("lib/ztos/__init__.py")).hexdigest()
    except KeyError:
        ours = ""
    if ours != os.environ.get("ZTOS_PRELOADED"):
        for name in [m for m in sys.modules if m == "ztos" or m.startswith("ztos.")]:
            del sys.modules[name]
sys.argv[0] = info["id"]
if not sys.stderr.isatty():
    # what the app prints goes to ~/.local/state/ztos/ID.log (ztos-sdk logs)
    state = os.path.expanduser("~/.local/state/ztos")
    try:
        os.makedirs(state, exist_ok=True)
        log = os.path.join(state, info["id"] + ".log")
        if os.path.exists(log):
            os.replace(log, log + ".1")
        out = open(log, "w", buffering=1)
        sys.stdout = sys.stderr = out
        os.dup2(out.fileno(), 1)
        os.dup2(out.fileno(), 2)
    except OSError:
        pass
def tell_shell(title, text):
    # a notification on the phone, so a failure is seen and not just logged
    runtime = os.environ.get("XDG_RUNTIME_DIR") or "/run/user/%d" % os.getuid()
    try:
        fd = os.open(os.path.join(runtime, "ztmetro.ctl"), os.O_WRONLY | os.O_NONBLOCK)
        line = "notify %s|%s|%s\\n" % (info["name"], title, text.replace("|", "/").replace("\\n", " ")[:300])
        os.write(fd, line.encode())
        os.close(fd)
    except OSError:
        pass
# what the app needs, checked before anything else
try:
    import PySide6.QtWidgets
except ImportError as e:
    print("ztos: Qt for Python (PySide6) is missing on this phone:", e, file=sys.stderr)
    print("ztos: on the computer, run: ztos-sdk doctor --fix", file=sys.stderr)
    tell_shell("Cannot start", "Qt for Python is not installed. On the computer run: ztos-sdk doctor --fix")
    raise SystemExit(127)
if os.path.exists(os.path.expanduser("~/.config/ztos/profile")):
    import time
    print("ztos start: %-28s %7.0f ms CPU" % ("Qt imported", time.process_time() * 1000), file=sys.stderr, flush=True)
import runpy, traceback
try:
    runpy.run_module(info["entry"][:-3].replace("/", "."), run_name="__main__", alter_sys=True)
except SystemExit:
    raise
except BaseException as e:
    traceback.print_exc()
    missing = getattr(e, "name", None) if isinstance(e, ImportError) else None
    tell_shell("Stopped", ("It needs %s, which is not on the phone" % missing) if missing else
               "%s: %s" % (type(e).__name__, e))
    raise SystemExit(1)
'''


def compile_ui(path: Path) -> str:
    uic = shutil.which("pyside6-uic")
    command = [uic] if uic else (["/usr/lib/qt6/uic", "-g", "python"] if Path("/usr/lib/qt6/uic").exists() else None)
    if command is None:
        raise Fail("no uic to compile .ui files with (pacman -S pyside6 or qt6-base)")
    result = subprocess.run([*command, str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        raise Fail(f"{path.name}: {result.stderr.strip()}")
    return result.stdout


def vendor(deps: list, into: Path):
    """Pure-Python packages from PyPI, for the phone (whatever its processor)."""
    if not deps:
        return
    say("fetching", ", ".join(deps))
    result = subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "--no-compile",
                             "--target", str(into), "--only-binary=:all:", "--platform", "any",
                             "--implementation", "py", "--python-version", "3.11", *deps],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise Fail("could not fetch the dependencies (only pure-Python packages can go in an app):\n"
                   + result.stderr.strip())
    native = [p for p in into.rglob("*") if p.suffix in (".so", ".pyd")]
    if native:
        raise Fail(f"{native[0].name} is compiled code: a package like that has to be installed on the phone "
                   "with pacman instead (ztos-sdk doctor shows how to reach it)")


def pyc(source: bytes, name: str) -> bytes:
    """Compiled code for this Python, hash-based and unchecked: zipimport takes
    it as it is (no timestamps to agree), and the phone skips compiling."""
    import importlib.util
    from importlib._bootstrap_external import _code_to_hash_pyc
    code = compile(source, name, "exec", dont_inherit=True, optimize=0)
    return bytes(_code_to_hash_pyc(code, importlib.util.source_hash(source), checked=False))


def native_library(target: str, device=None) -> Path:
    """Build/cache the C backend for the actual target, including phone builds.

    Apps carry their own library; the warm runtime gets the same bytes.
    Source hashes prevent deploying a stale ABI after a renderer change.
    """
    arch = platform.machine() if target == "host" else target
    native = SDK / "native"
    shared = SDK.parent / "ztmetro/core/zrnmetrorender.h"
    digest = hashlib.sha256((native / "renderer.c").read_bytes() + shared.read_bytes()).hexdigest()
    out = native / "prebuilt" / arch / "libztos-render.so"
    stamp = out.with_suffix(".sha256")
    if out.is_file() and stamp.is_file() and stamp.read_text().strip() == digest:
        return out
    if arch == platform.machine():
        result = subprocess.run(["make", "-B", "-C", str(native), f"ARCH={arch}"], capture_output=True)
        if result.returncode:
            raise Fail("cannot build native renderer (install a C compiler and GLES2 development files):\n" + result.stderr.decode())
    elif device is not None:
        say("building the C GLES renderer on the phone")
        bundle = io.BytesIO()
        with tarfile.open(fileobj=bundle, mode="w") as tar:
            tar.add(native / "renderer.c", "renderer.c")
            tar.add(shared, "zrnmetrorender.h")
        encoded = device.run(
            "set -e; t=$(mktemp -d); trap 'rm -rf \"$t\"' EXIT; tar xf - -C \"$t\"; "
            f"test \"$(uname -m)\" = {shlex.quote(arch)}; "
            "cd \"$t\"; cc -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared -I. "
            "$(pkg-config --cflags glesv2) -o renderer.so renderer.c $(pkg-config --libs glesv2) -lm; "
            "base64 -w0 renderer.so", data=bundle.getvalue())
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(base64.b64decode(encoded, validate=True))
    else:
        cc = os.environ.get("CC") or shutil.which(f"{arch}-linux-gnu-gcc") or shutil.which(f"{arch}-linux-gnu-cc")
        if cc:
            result = subprocess.run(["make", "-B", "-C", str(native), f"ARCH={arch}", f"CC={cc}"], capture_output=True)
            if result.returncode == 0 and out.is_file():
                stamp.write_text(digest + "\n")
                return out
        raise Fail(f"no current C renderer for {arch}; use deploy to build it on the phone, "
                   f"or cross-compile with make -C {native} ARCH={arch} CC=YOUR_CC")
    stamp.write_text(digest + "\n")
    return out


def build(project: Project, target: str = "aarch64", python: str | None = None) -> Path:
    """`python` is the phone's Python (major.minor): when it is this one's, the
    code goes in compiled too, and the app starts without compiling it."""
    native = native_library(target)
    compiled = python == "%d.%d" % sys.version_info[:2]
    launcher = LAUNCHERS / f"ztos-launch-{'x86_64' if target == 'host' else target}"
    if not launcher.is_file():
        raise Fail(f"no launcher for {target} in {LAUNCHERS} (build it: make -C {SDK / 'launcher'})")
    compile(BOOTSTRAP, "__main__.py", "exec")     # the SDK's own part must be sound too
    files = project.files()
    if project.entry not in [f.as_posix() for f in files]:
        files.append(Path(project.entry))
    # every .py must at least compile: better here than on the phone
    for rel in files:
        if rel.suffix == ".py":
            try:
                compile((project.root / rel).read_bytes(), str(rel), "exec", dont_inherit=True)
            except SyntaxError as e:
                raise Fail(f"{rel}:{e.lineno}: {e.msg}")
    dist = project.root / "dist"
    dist.mkdir(exist_ok=True)
    out = dist / project.id
    tmp = dist / f".{project.id}.new"
    with tempfile.TemporaryDirectory() as work:
        lib = Path(work) / "lib"
        lib.mkdir()
        vendor(project.build_cfg.get("dependencies", []), lib)
        with open(tmp, "wb") as f:
            f.write(launcher.read_bytes())
            with zipfile.ZipFile(f, "w", zipfile.ZIP_DEFLATED) as z:
                def put(data: bytes, name: str):
                    z.writestr(name, data)
                    if compiled and name.endswith(".py"):
                        z.writestr(name + "c", pyc(data, name))
                put(BOOTSTRAP.encode(), "__main__.py")
                put(native.read_bytes(), "lib/ztos/_native_renderer.so")
                z.writestr("ztos.json", json.dumps(project.manifest, indent=1))
                for rel in files:
                    put((project.root / rel).read_bytes(), f"app/{rel.as_posix()}")
                    if rel.suffix == ".ui":
                        put(compile_ui(project.root / rel).encode(), f"app/ui_{rel.stem}.py")
                for path in sorted((RUNTIME / "ztos").rglob("*.py")):
                    put(path.read_bytes(), f"lib/ztos/{path.relative_to(RUNTIME / 'ztos').as_posix()}")
                for path in sorted(lib.rglob("*")):
                    if path.is_file() and "__pycache__" not in path.parts:
                        put(path.read_bytes(), f"lib/{path.relative_to(lib).as_posix()}")
    tmp.chmod(0o755)
    tmp.replace(out)
    return out


def cmd_build(args):
    project = find_project(args.project)
    out = build(project, args.target)
    say(f"built {os.path.relpath(out)} ({out.stat().st_size // 1024} KB, {args.target})")
    return out


# ---- the phone ------------------------------------------------------------------------------

class Device:
    def __init__(self, spec: str):
        self.spec = spec if "@" in spec else f"alarm@{spec}"
        self._home: str | None = None

    def _ssh(self) -> tuple:
        # one connection, kept open five minutes and shared by every step (and
        # the next command): the key's passphrase is asked once, not each time
        control = Path.home() / ".cache" / "ztos-sdk"
        control.mkdir(parents=True, exist_ok=True, mode=0o700)
        command = ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                   "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=8",
                   "-o", "ControlMaster=auto", "-o", f"ControlPath={control}/ssh-%C", "-o", "ControlPersist=300"]
        env = dict(os.environ)
        if os.environ.get("ZTOS_PASSWORD"):
            # the password goes in by ssh's own askpass, so stdin stays free for files
            env["SSH_ASKPASS"] = str(SDK / "bin" / "ztos-askpass")
            env["SSH_ASKPASS_REQUIRE"] = "force"
            env.setdefault("DISPLAY", ":0")
            command += ["-o", "PreferredAuthentications=password,keyboard-interactive",
                        "-o", "PubkeyAuthentication=no", "-o", "NumberOfPasswordPrompts=1"]
        return command + [self.spec], env

    def run(self, script: str, data: bytes | None = None, check: bool = True, capture: bool = True) -> str:
        command, env = self._ssh()
        result = subprocess.run([*command, script], input=data, env=env,
                                stdout=subprocess.PIPE if capture else None,
                                stderr=subprocess.PIPE if capture else None,
                                stdin=None if data is not None else subprocess.DEVNULL)
        out = (result.stdout or b"").decode(errors="replace")
        if check and result.returncode != 0:
            err = (result.stderr or b"").decode(errors="replace").strip()
            if result.returncode == 255 and (not err or err.startswith(("ssh:", "kex_", "Connection", "Permission denied"))):
                raise Fail(f"cannot reach the phone at {self.spec}: {err or 'ssh failed'}")
            raise Fail(f"on the phone: {err or out.strip() or f'exit {result.returncode}'}")
        return out

    def home(self) -> str:
        if self._home is None:
            self._home = self.run('printf %s "$HOME"').strip()
        return self._home


def device_of(args, project: Project | None = None) -> Device:
    spec = args.device or os.environ.get("ZTOS_DEVICE") or \
        (project.device_cfg.get("host") if project else None) or DEFAULT_DEVICE
    return Device(spec)


SHELL_CTL = 'ctl="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ztmetro.ctl"; ' \
            'tell() { [ -p "$ctl" ] && timeout 2 sh -c \'printf "%s\\n" "$2" > "$1"\' _ "$ctl" "$1"; }; '


def desktop_entry(project: Project, binary: str, icon: str) -> str:
    return "\n".join([
        "[Desktop Entry]", "Type=Application", f"Name={project.name}",
        f"Comment={project.manifest['description'] or project.name}",
        f"Exec={binary}", f"Icon={icon}", "Terminal=false", f"StartupWMClass=ztos.{project.id}",
        "Categories=Utility;", f"X-ZtOS-Id={project.id}", f"X-ZtOS-Version={project.version}", "",
    ])


def install(project: Project, device: Device, binary: Path):
    home = device.home()
    apps = f"{home}/{APPS_DIR}"
    icon_src = project.root / project.icon
    icon_ext = icon_src.suffix if icon_src.is_file() else ".svg"
    icon_dir = f"{home}/.local/share/icons/hicolor/{'scalable' if icon_ext == '.svg' else '256x256'}/apps"
    icon = f"{icon_dir}/{project.id}{icon_ext}"
    bundle = io.BytesIO()
    with tarfile.open(fileobj=bundle, mode="w") as tar:
        tar.add(binary, "app")
        tar.add(icon_src if icon_src.is_file() else TEMPLATE / "icon.svg", "icon")
        tar.add(native_library("aarch64"), "lib/ztos/_native_renderer.so")
        tar.add(RUNTIME / "zygote.py", "zygote.py")    # the warm runtime apps start from
        for path in sorted((RUNTIME / "ztos").glob("*.py")):
            tar.add(path, f"lib/ztos/{path.name}")     # what it preloads
        entry = desktop_entry(project, f"{apps}/{project.id}", icon).encode()
        info = tarfile.TarInfo("app.desktop")
        info.size = len(entry)
        tar.addfile(info, io.BytesIO(entry))
    q = shlex.quote
    script = (
        "set -e; t=$(mktemp -d); trap 'rm -rf \"$t\"' EXIT; tar xmf - -C \"$t\"; "
        f"mkdir -p {q(apps)} {q(icon_dir)} {q(home + '/.local/share/applications')}; "
        f"install -m755 \"$t/app\" {q(apps + '/' + project.id)}; "
        f"install -m644 \"$t/icon\" {q(icon)}; "
        # a new warm runtime replaces the old one (the next app starts it again)
        # (the warm runtime's path is put together here, so this command's own
        # text never contains it: pkill and pgrep then find only the runtime)
        "zy=zygote; z=$HOME/.local/share/ztos/$zy.py; "
        "l=$HOME/.local/share/ztos/lib/ztos; new=0; "
        "cmp -s \"$t/$zy.py\" \"$z\" || new=1; "
        "for f in \"$t\"/lib/ztos/*; do cmp -s \"$f\" \"$l/$(basename \"$f\")\" || new=1; done; "
        "if [ $new = 1 ]; then install -Dm644 \"$t/$zy.py\" \"$z\"; mkdir -p \"$l\"; rm -rf \"$l\"/*; "
        "cp \"$t\"/lib/ztos/* \"$l/\"; python3 -m compileall -q \"$l\" >/dev/null 2>&1; "
        "pkill -f 'share/ztos/[z]ygote.py' || true; sleep 0.3; fi; "
        # warm now, so the next app starts from it
        "if ! pgrep -f 'share/ztos/[z]ygote.py' >/dev/null; then mkdir -p $HOME/.local/state/ztos; "
        "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} setsid -f python3 -I \"$z\" "
        "> $HOME/.local/state/ztos/$zy.log 2>&1 < /dev/null; fi; "
        f"install -m644 \"$t/app.desktop\" {q(home + '/.local/share/applications/' + project.id + '.desktop')}; "
        + SHELL_CTL + "tell apps || true")
    device.run(script, data=bundle.getvalue())


def cmd_install(args):
    project = find_project(args.project)
    device = device_of(args, project)
    found = ensure_ready(device, project)
    native_library("aarch64", device)
    binary = build(project, python=".".join(found.get("python", "").split(".")[:2]))
    say(f"installing {project.name} {project.version} on {device.spec}")
    install(project, device, binary)
    say(f"installed: {project.name} is in the app list")
    return project, device


def launch(project: Project, device: Device):
    """Open the app in the shell, and see that it is really running."""
    binary = f"{device.home()}/{APPS_DIR}/{project.id}"
    out = device.run(SHELL_CTL + f"if tell {shlex.quote('launch ' + binary)}; then echo shell; "
                     f"else echo none; fi")
    if "shell" not in out:
        raise Fail("the ztMETRO shell is not running on the phone (nothing to open the app in)")
    say(f"opening {project.name} on the phone...")
    # the shell gives an app 4 s to show a window; look a little after that.
    # (the path is put together by the shell, so pgrep does not find this command)
    out = device.run(f"sleep 5; a={shlex.quote(project.id)}; d=$HOME/{APPS_DIR}; "
                     f"if pgrep -f \"$d/$a\" >/dev/null; then echo running; else echo stopped; fi; "
                     f"tail -n 25 \"$HOME/{LOG_DIR}/$a.log\" 2>/dev/null", check=False)
    lines = out.splitlines()
    state, log = (lines[0] if lines else "stopped"), "\n".join(lines[1:]).strip()
    if state != "running":
        raise Fail(f"{project.name} did not stay open on the phone. What it printed:\n"
                   + ("  " + log.replace("\n", "\n  ") if log else "  (nothing)"))
    say(f"{project.name} is running on the phone")


def cmd_launch(args):
    project = find_project(args.project)
    device = device_of(args, project)
    ensure_ready(device, project, quiet=True)
    launch(project, device)


def cmd_deploy(args):
    project, device = cmd_install(args)
    launch(project, device)
    if args.logs:
        time.sleep(2)
        cmd_logs(args)


def cmd_logs(args):
    project = find_project(args.project)
    device = device_of(args, project)
    log = f"{LOG_DIR}/{project.id}.log"
    if getattr(args, "follow", False):
        device.run(f"tail -n 50 -F $HOME/{log}", check=False, capture=False)
    else:
        print(device.run(f"cat $HOME/{log} 2>/dev/null || echo '(no log yet: the app has not run)'"), end="")


def cmd_uninstall(args):
    project = find_project(args.project) if not args.id else None
    app_id = args.id or project.id
    if not re.fullmatch(r"[a-z0-9._-]+", app_id):
        raise Fail("not an app id")
    device = device_of(args, project)
    # the path is put together by the shell, so this command's own text never
    # contains it and pkill finds only the running app
    device.run(
        f"a={shlex.quote(app_id)}; d=$HOME/{APPS_DIR}; pkill -f \"$d/$a\" || true; "
        f"rm -f \"$d/$a\" \"$HOME/.local/share/applications/$a.desktop\" "
        f"$HOME/.local/share/icons/hicolor/*/apps/\"$a\".* \"$HOME/{LOG_DIR}/$a.log\"*; "
        + SHELL_CTL + "tell apps || true")
    say(f"removed {app_id} from the phone")


def cmd_list(args):
    device = device_of(args)
    out = device.run(
        "for f in $HOME/.local/share/applications/*.desktop; do "
        "grep -q '^X-ZtOS-Id=' \"$f\" 2>/dev/null || continue; "
        "printf '%s\\t%s\\t%s\\n' \"$(sed -n 's/^X-ZtOS-Id=//p' \"$f\")\" \"$(sed -n 's/^Name=//p' \"$f\")\" "
        "\"$(sed -n 's/^X-ZtOS-Version=//p' \"$f\")\"; done", check=True)
    rows = [line.split("\t") for line in out.splitlines() if line.strip()]
    if not rows:
        say("no SDK apps on the phone yet")
    for app_id, name, version in rows:
        print(f"  {name:<24} {version:<10} {app_id}")


# What every app needs on the phone. Qt's Wayland plugin is part of qt6-base
# from Qt 6.10 on; the shell needs no package of its own.
REQUIRED = ["python", "pyside6", "qt6-svg"]

CHECKS = r"""
printf 'arch\t%s\n' "$(uname -m)"
printf 'python\t%s\n' "$(python3 -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])' 2>/dev/null || echo missing)"
printf 'pyside6\t%s\n' "$(python3 -c 'import PySide6, PySide6.QtWidgets; print(PySide6.__version__)' 2>/dev/null || echo missing)"
printf 'qtsvg\t%s\n' "$(python3 -c 'import PySide6.QtSvg; print("ok")' 2>/dev/null || echo missing)"
printf 'qtopengl\t%s\n' "$(python3 -c 'import PySide6.QtOpenGL; print("ok")' 2>/dev/null || echo missing)"
printf 'wayland\t%s\n' "$(ls /usr/lib/qt6/plugins/platforms/libqwayland*.so >/dev/null 2>&1 && echo ok || echo missing)"
printf 'shell\t%s\n' "$([ -p "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ztmetro.ctl" ] && echo running || echo 'not running')"
printf 'space\t%s\n' "$(df -h "$HOME" | awk 'NR==2 {print $4 " free"}')"
# Qt for Python ships without compiled files, and an app cannot write them
# under /usr: without them every app start compiles PySide6 (~2 s here)
d=$(python3 -c 'import PySide6, os; print(os.path.dirname(PySide6.__file__))' 2>/dev/null)
c=$(ls "$d"/__pycache__/__init__.cpython-*.pyc 2>/dev/null | head -1)
printf 'qtcache\t%s\n' "$( [ -n "$d" ] && [ -n "$c" ] && [ "$c" -nt "$d/__init__.py" ] && echo ok || echo missing)"
for p in $PKGS; do printf 'pkg\t%s\t%s\n' "$p" "$(pacman -Q "$p" 2>/dev/null | cut -d' ' -f2 || true)"; done
"""
COMPILE = ("python3 -m compileall -q -j 4 $(python3 -c 'import PySide6, shiboken6, os; "
           "print(os.path.dirname(PySide6.__file__), os.path.dirname(shiboken6.__file__))') >/dev/null")
LABELS = {"qtcache": "Qt for Python compiled", "arch": "processor", "python": "Python", "pyside6": "Qt for Python (PySide6)", "qtsvg": "Qt SVG",
          "qtopengl": "Qt OpenGL (smooth motion)", "wayland": "Qt Wayland plugin", "shell": "ztMETRO shell",
          "space": "storage"}


def survey(device: Device, packages: list) -> tuple:
    """What the phone has: (checks, {package: installed version or ""})."""
    out = device.run(f"PKGS={shlex.quote(' '.join(packages))}; " + CHECKS)
    found, installed = {}, {}
    for line in out.splitlines():
        parts = line.split("\t")
        if parts[0] == "pkg" and len(parts) >= 2:
            installed[parts[1]] = parts[2].strip() if len(parts) > 2 else ""
        elif len(parts) == 2:
            found[parts[0]] = parts[1]
    return found, installed


def plan(device: Device, packages: list) -> list:
    """What pacman would install for `packages`, as [(name, version)]. Refuses
    a plan that would change any package already there: on Arch, upgrading
    some packages and not the rest (a partial upgrade) breaks the system, so
    that is left to a full `pacman -Syu`."""
    out = device.run(f"pacman -Sp --needed --print-format '%n %v' {' '.join(map(shlex.quote, packages))} 2>&1; "
                     "echo \"exit=$?\"", check=False)
    lines = [l for l in out.splitlines() if l.strip()]
    if not lines or lines[-1] != "exit=0":
        detail = "\n".join(l for l in lines if not l.startswith("exit="))
        raise Fail("pacman cannot find what the apps need in the phone's package lists:\n  "
                   + detail.replace("\n", "\n  ")
                   + "\nThe lists are probably old. Bring the whole phone up to date first (as root, with internet):"
                   "\n  pacman -Syu")
    wanted = [tuple(l.split(" ", 1)) for l in lines[:-1] if " " in l and not l.startswith(("::", "warning"))]
    names = " ".join(shlex.quote(n) for n, _ in wanted)
    have = {}
    if names:
        for line in device.run(f"pacman -Q {names} 2>/dev/null || true", check=False).splitlines():
            if " " in line:
                n, v = line.split(" ", 1)
                have[n] = v.strip()
    changes = [f"{n} {have[n]} -> {v}" for n, v in wanted if n in have and have[n] != v]
    if changes:
        raise Fail("installing what the apps need would also upgrade packages already on the phone:\n  "
                   + "\n  ".join(changes)
                   + "\nThat is a partial upgrade, which can break Arch Linux. Update the whole phone first "
                   "(as root, with internet):\n  pacman -Syu")
    return wanted


def as_root(device: Device, command: str) -> int:
    """Run `command` as root on the phone: su asks for root's password on
    this terminal, or takes it from $ZTOS_ROOT_PASSWORD."""
    root_pw = os.environ.get("ZTOS_ROOT_PASSWORD")
    if root_pw:
        out, code = pty_run(device, f"su -c {shlex.quote(command)}", root_pw)
        print(out[-3000:], end="")
        return code
    if not sys.stdin.isatty():
        raise Fail("this needs root on the phone: run it in a terminal (su asks for root's password), "
                   "or set $ZTOS_ROOT_PASSWORD")
    say("root's password on the phone, for su:")
    ssh, env = device._ssh()
    return subprocess.call([*ssh[:-1], "-t", device.spec, f"su -c {shlex.quote(command)}"], env=env)


def ensure_ready(device: Device, project: "Project | None" = None, fix: bool = True, quiet: bool = False) -> dict:
    """Make sure the phone can run SDK apps (and this app's own packages):
    checked every time, and what is missing installed, safely."""
    extra = list(project.requires) if project else []
    packages = REQUIRED + [p for p in extra if p not in REQUIRED]
    found, installed = survey(device, packages)
    missing = [p for p in packages if not installed.get(p)]
    uncompiled = found.get("qtcache") == "missing"
    if found.get("arch") not in (None, "aarch64"):
        raise Fail(f"the phone reports a {found.get('arch')} processor; the SDK builds for aarch64")
    if missing:
        if not fix:
            return found
        say(f"the phone needs: {', '.join(missing)}")
        wanted = plan(device, missing)
        size_note = ", ".join(f"{n} {v}" for n, v in wanted)
        say(f"installing on the phone: {size_note} (nothing already there changes)")
        code = as_root(device, f"pacman -S --needed --noconfirm {' '.join(map(shlex.quote, missing))} && {COMPILE}")
        found, installed = survey(device, packages)
        still = [p for p in packages if not installed.get(p)]
        if code != 0 or still:
            raise Fail(f"could not install {', '.join(still or missing)} on the phone "
                       "(is it online? was root's password right?)")
        say("installed")
        uncompiled = found.get("qtcache") == "missing"
    if uncompiled and fix and found.get("pyside6") not in (None, "missing"):
        say("compiling Qt for Python once, so apps start without it (about a minute)")
        if as_root(device, COMPILE) != 0:
            raise Fail("could not compile Qt for Python on the phone")
        found, installed = survey(device, packages)
    broken = [LABELS[k] for k in ("python", "pyside6", "wayland") if found.get(k) == "missing"]
    if broken:
        raise Fail(f"the packages are there, but {', '.join(broken)} does not work on the phone; "
                   "check with: ztos-sdk doctor")
    if not quiet:
        say(f"phone ready: Python {found.get('python')}, Qt for Python {found.get('pyside6')}")
    return found


def cmd_doctor(args):
    device = device_of(args, None)
    found, installed = survey(device, REQUIRED)
    for key, label in LABELS.items():
        value = found.get(key, "?")
        mark = "!!" if value in ("missing", "not running") else "ok"
        print(f"  {mark}  {label:<28} {value}")
    for name in REQUIRED:
        print(f"  {'ok' if installed.get(name) else '!!'}  {'package ' + name:<28} {installed.get(name) or 'missing'}")
    missing = [p for p in REQUIRED if not installed.get(p)]
    if not missing and found.get("qtcache") == "ok":
        say("the phone is ready for ztOS SDK apps")
        return
    if not args.fix:
        say(f"missing: {', '.join(missing) or 'Qt for Python compiled files'}. `ztos-sdk doctor --fix` sets it up "
            "(install and deploy do it by themselves too)")
        raise SystemExit(1)
    ensure_ready(device)


def pty_run(device: Device, command: str, root_password: str) -> tuple:
    """Run `command` with a terminal on the phone, answering su's password
    prompt (an ssh login prompt gets $ZTOS_PASSWORD). Returns (output, exit)."""
    import pty
    import select
    ssh, env = device._ssh()
    pid, fd = pty.fork()
    if pid == 0:
        os.execvpe("ssh", [*ssh[:-1], "-tt", device.spec, command], env)
    out, cursor = b"", 0
    while True:
        ready, _, _ = select.select([fd], [], [], 900)
        if not ready:
            break
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
        prompt = out[cursor:].lower()
        if b"password" in prompt and prompt.rstrip().endswith(b":"):
            login = b"'s password" in prompt
            answer = os.environ.get("ZTOS_PASSWORD", "") if login else root_password
            os.write(fd, answer.encode() + b"\n")
            cursor = len(out)
    _, status = os.waitpid(pid, 0)
    return out.decode(errors="replace"), os.waitstatus_to_exitcode(status)


# ---- the command line ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(prog="ztos-sdk", description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__.split("\n\n", 1)[1])
    sub = parser.add_subparsers(dest="command", required=True)

    def command(name, func, help_text, project=True, device=False):
        p = sub.add_parser(name, help=help_text)
        p.set_defaults(func=func)
        if project:
            p.add_argument("-C", "--project", help="the app's folder (default: here)")
        if device:
            p.add_argument("--device", help=f"user@host of the phone (default {DEFAULT_DEVICE})")
        return p

    p = command("new", cmd_new, "make a new app", project=False)
    p.add_argument("name", help='its name, as the app list shows it ("Notes")')
    p.add_argument("--id", help="its id (default com.example.NAME)")
    p.add_argument("--dir", help="where (default: a folder named after it)")
    p = command("run", cmd_run, "try the app on this computer")
    p.add_argument("--shot", help="save a picture of it to this PNG after it starts, and quit")
    p.add_argument("--offscreen", action="store_true", help="with --shot: no window at all")
    p.add_argument("args", nargs="*", help="passed to the app")
    p = command("designer", cmd_designer, "open the app's pages in Qt Designer")
    p.add_argument("form", nargs="?", help="one .ui file (default: all of them)")
    p = command("build", cmd_build, "make the app's binary in dist/")
    p.add_argument("--target", default="aarch64", choices=["aarch64", "host"],
                   help="aarch64 for the phone (default), host to run the binary here")
    command("install", cmd_install, "build it and install it on the phone", device=True)
    p = command("deploy", cmd_deploy, "install it, and open it on the phone", device=True)
    p.add_argument("--logs", action="store_true", help="then show what it printed")
    command("launch", cmd_launch, "open the installed app on the phone", device=True)
    p = command("logs", cmd_logs, "what the app printed on the phone", device=True)
    p.add_argument("-f", "--follow", action="store_true", help="keep showing it")
    p = command("uninstall", cmd_uninstall, "take the app off the phone", device=True)
    p.add_argument("--id", help="another app, by id")
    command("list", cmd_list, "the SDK's apps on the phone", project=False, device=True)
    p = command("doctor", cmd_doctor, "check the phone has what the apps need", project=False, device=True)
    p.add_argument("--fix", action="store_true", help="install what is missing (pacman, as root; never upgrades)")

    args = parser.parse_args(argv)
    try:
        result = args.func(args)
        return result if isinstance(result, int) else 0
    except Fail as e:
        print(f"ztos-sdk: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
