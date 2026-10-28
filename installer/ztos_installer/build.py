# SPDX-License-Identifier: GPL-2.0-or-later
"""Building ztOS: an Arch Linux ARM system made into ztOS, then a disk.

Everything runs unprivileged. The system is put together inside a user
namespace (`unshare --map-auto --map-root-user`): there, files can belong to
root and the other system users, and an aarch64 chroot runs through the
host's qemu-user binfmt handler. The disk images are made in the same
namespace (mke2fs -d, mtools), so nothing is ever mounted for real.
"""
from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]          # the repository
OS = ROOT / "os"
WORK = ROOT / "build" / "ztos"
TARBALL = ROOT / "build" / "alarm.tar.gz"
TARBALL_URL = "http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz"
SDK = ROOT / "sdk"
PKGCACHE = ROOT / "build" / "pkg-cache"
EDK2 = Path("/usr/share/edk2/aarch64")


class Fail(Exception):
    pass


@dataclass
class Settings:
    target: str = "qemu"            # qemu | starlte | gsi (the generic system alone, as a tarball)
    user: str = "ztos"
    password: str = "ztos"
    hostname: str = "ztos"
    timezone: str = "UTC"
    size_gb: int = 10               # the root filesystem
    fresh: bool = False             # start from the tarball again

    @property
    def dir(self) -> Path:
        return WORK / self.target


def say(*words):
    print("ztos:", *words, flush=True)


# a target's device layer (devices/<dir>/: manifest, layer.sh, packages.txt);
# the GSI has none (design/ztOSIAL.md)
DEVICES = {"starlte": "samsung-starlte", "qemu": "qemu-virt"}


def gsi_version() -> str:
    """0.2.0 for "0.2.0-BETA-1": the GSI's version is the release without its stage."""
    return (ROOT / "VERSION").read_text().split()[0].split("-")[0]


def packages(target: str) -> list:
    names = []
    paths = [OS / "packages" / "common"]
    if target in DEVICES:
        paths.append(ROOT / "devices" / DEVICES[target] / "packages.txt")
    for path in paths:
        if path.exists():
            for line in path.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if line:
                    names.append(line)
    return names


def check_host():
    """What building needs on this computer."""
    missing = []
    for tool in ("unshare", "newuidmap", "bsdtar", "mkfs.fat", "mcopy", "mmd", "sfdisk", "python3"):
        if shutil.which(tool) is None:
            missing.append(tool)
    if not Path("/usr/bin/mke2fs").exists() and not Path("/usr/sbin/mke2fs").exists():
        missing.append("mke2fs (e2fsprogs)")
    if not Path("/proc/sys/fs/binfmt_misc/qemu-aarch64").exists():
        missing.append("qemu-user-static + qemu-user-static-binfmt (to run aarch64 programs)")
    user = os.environ.get("USER", "")
    subuid = Path("/etc/subuid").read_text() if Path("/etc/subuid").exists() else ""
    if os.geteuid() != 0 and not re.search(rf"^{re.escape(user)}:", subuid, re.M):
        missing.append(f"a /etc/subuid range for {user} (usermod --add-subuids 100000-165535 {user})")
    if missing:
        raise Fail("this computer is missing what building ztOS needs:\n  " + "\n  ".join(missing) +
                   "\nOn Arch Linux: sudo pacman -S --needed util-linux libarchive dosfstools mtools e2fsprogs "
                   "qemu-user-static qemu-user-static-binfmt")


def fetch_tarball():
    if TARBALL.exists() and TARBALL.stat().st_size > 100_000_000:
        return
    say(f"downloading Arch Linux ARM ({TARBALL_URL})")
    TARBALL.parent.mkdir(parents=True, exist_ok=True)
    tmp = TARBALL.with_suffix(".part")
    subprocess.run(["curl", "-L", "--fail", "-o", str(tmp), TARBALL_URL], check=True)
    tmp.replace(TARBALL)


def build_apps(stage: Path):
    """Build the selected default system apps for aarch64."""
    out = stage / "apps"
    # A reused staging directory must not keep apps removed from the defaults.
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(SDK))
    from ztos_sdk import cli as sdk
    projects = (OS / "apps" / "welcome", OS / "apps" / "whatsapp", ROOT / "apps" / "music")
    for project in projects:
        p = sdk.find_project(str(project))
        python = "%d.%d" % sys.version_info[:2]
        binary = sdk.build(p, "aarch64", python=python)
        shutil.copy2(binary, out / p.id)
        shutil.copy2(project / p.icon, out / f"{p.id}.svg")
        (out / f"{p.id}.desktop").write_text("\n".join([
            "[Desktop Entry]", "Type=Application", f"Name={p.name}",
            f"Comment={p.manifest['description'] or p.name}",
            f"Exec=/usr/local/lib/ztos/apps/{p.id}",
            f"Icon=/usr/local/share/icons/hicolor/scalable/apps/{p.id}.svg",
            "Terminal=false", f"StartupWMClass=ztos.{p.id}", "Categories=System;", "",
        ]))


# The work inside the namespace, as root there. Each "step" line is what the
# installer shows; everything else goes to the log.
STAGE = r'''
set -eu
step() { echo "STEP $*"; }
R=$ROOTFS
umask 022

# A run stopped half way through the unpack leaves etc/arch-release without
# the rest (no var/lib/pacman): only a mark written after the whole tarball
# counts as unpacked.
if [ "$FRESH" = 1 ] || [ ! -e "$R/.ztos-unpacked" ]; then
	step "unpacking Arch Linux ARM"
	rm -rf "$R"; mkdir -p "$R"
	bsdtar -xpf "$TARBALL" -C "$R" --no-xattrs --no-acls --no-fflags
	touch "$R/.ztos-unpacked"
fi

mount --bind "$R" "$R"
mount --rbind /dev "$R/dev"
mount -t proc proc "$R/proc"
mount -t tmpfs tmpfs "$R/tmp"
mkdir -p "$PKGCACHE" "$R/var/cache/pacman/pkg"
mount --bind "$PKGCACHE" "$R/var/cache/pacman/pkg"
rm -f "$R/etc/resolv.conf"; cp -L /etc/resolv.conf "$R/etc/resolv.conf"
ch() { chroot "$R" /usr/bin/env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/bin HOME=/root LANG=C.UTF-8 "$@"; }
P="pacman --disable-sandbox --noconfirm"

if [ "$TARGET" = starlte ]; then
	# the kernel is ztOS's own boot image there, never a package
	sed -i 's/^#\?IgnorePkg *=.*/IgnorePkg   = linux-aarch64 linux-aarch64-headers/' "$R/etc/pacman.conf"
fi

step "setting up package signing keys"
ch sh -c 'pacman-key --init && pacman-key --populate archlinuxarm' > /dev/null

step "updating the base system"
ch $P -Syu

step "installing ztOS's packages (this is the long part)"
ch $P -S --needed $PACKAGES

# Ship the tested ARM64 runtime: OS installs do not compile ART or ATL.
ATL_PACKAGES="$SRC/prebuilts/ztOSAndroidRuntimeV1/packages"
if [ ! -f "$ATL_PACKAGES/SHA256SUMS" ]; then
    ATL_PACKAGES="$SRC/build/atl/packages"
fi
if [ -f "$ATL_PACKAGES/SHA256SUMS" ]; then
    step "installing ztOSAndroidRuntimeV1 (prebuilt ARM64 runtime)"
    mkdir -p "$R/var/tmp/ztos-atl-packages"
    cp -a "$ATL_PACKAGES/." "$R/var/tmp/ztos-atl-packages/"
    cp "$SRC/android/atl/install-runtime.sh" "$R/var/tmp/ztos-atl-install.sh"
    ch bash /var/tmp/ztos-atl-install.sh /var/tmp/ztos-atl-packages
    mkdir -p "$R/usr/local/share/ztos/android-runtime"
    rm -rf "$R/usr/local/share/ztos/android-runtime/packages"
    mv "$R/var/tmp/ztos-atl-packages" "$R/usr/local/share/ztos/android-runtime/packages"
    mv "$R/var/tmp/ztos-atl-install.sh" "$R/usr/local/share/ztos/android-runtime/install-runtime.sh"
    chown -R 0:0 "$R/usr/local/share/ztos/android-runtime"
else
    echo "Missing ztOSAndroidRuntimeV1; restore prebuilts/ztOSAndroidRuntimeV1 or build android/atl first" >&2
    exit 1
fi

step "removing the other desktops (ztMETRO is the only shell)"
for p in sddm gdm lightdm hyprland plasma-workspace kwin phosh phoc gnome-shell weston sway; do
	ch pacman -Qq "$p" > /dev/null 2>&1 && ch $P -Rns "$p" || true
done

# before ztMETRO: the shell links libztos-ial
step "installing ztOSIAL (the hardware interfaces)"
rm -rf "$R/usr/src/ztos-ial"; mkdir -p "$R/usr/src"
cp -a "$SRC/ial" "$R/usr/src/ztos-ial"
sed -i "s/\"version\": \"[^\"]*\"/\"version\": \"$GSIVERSION\"/" "$R/usr/src/ztos-ial/gsi.json"
ch sh -c 'cd /usr/src/ztos-ial && make clean >/dev/null 2>&1; make && make install PREFIX=/usr'
rm -rf "$R/usr/src/ztos-ial"

step "building ztMETRO and its apps"
rm -rf "$R/usr/src/ztmetro"; mkdir -p "$R/usr/src"
cp -a "$SRC/ztmetro" "$R/usr/src/ztmetro"
rm -rf "$R/usr/src/ztmetro/ref"
ch sh -c 'cd /usr/src/ztmetro && make clean >/dev/null 2>&1; make -j"$(nproc)" && make install'
ch sh -c 'cd /usr/src/ztmetro && make install-browser'
ch sh -c 'cd /usr/src/ztmetro && make clean >/dev/null 2>&1' || true

step "adding ztOS's files"
cp -a "$SRC/os/overlay/." "$R/"
cp -a "$SRC/rootfs/overlay/." "$R/"         # every phone's: USB link, session, memory and fs recovery
ch update-desktop-database /usr/share/applications

step "adding Android (Waydroid and the recipe for its image)"
W=$R/usr/share/ztos-waydroid
rm -rf "$W"; install -d "$W"
for f in install.sh strip-image.sh patch-vendor.sh patch-composer.py waydroid.cfg network.py recover.sh \
		audio-recover.py launch.sh power.sh ztos-waydroid-boot README.md \
		ztos-waydroid-network.service ztos-waydroid-session.service ztos-waydroid-boot.service \
		ztos-waydroid-audio-reconnect.service; do
	[ -e "$SRC/android/waydroid/$f" ] && cp -a "$SRC/android/waydroid/$f" "$W/"
done
install -Dm755 "$SRC/android/waydroid/ztos-waydroid-images" "$R/usr/lib/ztos/ztos-waydroid-images"
install -Dm644 "$SRC/android/waydroid/ztos-waydroid-images.service" \
	"$R/usr/lib/systemd/system/ztos-waydroid-images.service"

DEVICE_UNITS=
if [ "$TARGET" != gsi ]; then
	. "$DEVSRC/layer.sh"
fi
install -d "$R/usr/local/share/fonts/selawik"
install -m644 "$SRC"/os/fonts/selawik/*.ttf "$R/usr/local/share/fonts/selawik/"
ch fc-cache -f > /dev/null 2>&1 || true

step "installing the ztOS SDK"
rm -rf "$R/usr/local/lib/ztos-sdk"; mkdir -p "$R/usr/local/lib"
cp -a "$SRC/sdk" "$R/usr/local/lib/ztos-sdk"
ln -sf ../lib/ztos-sdk/bin/ztos-sdk "$R/usr/local/bin/ztos-sdk"
S=$R/etc/skel/.local/share/ztos
install -Dm644 "$SRC/sdk/runtime/zygote.py" "$S/zygote.py"
install -d "$S/lib/ztos"
install -m644 "$SRC"/sdk/runtime/ztos/*.py "$S/lib/ztos/"
ch python3 -m compileall -q /etc/skel/.local/share/ztos/lib > /dev/null || true

step "installing ztOS's apps (Welcome, Updates)"
install -d "$R/usr/local/lib/ztos/apps" "$R/usr/local/share/applications" "$R/usr/local/share/icons/hicolor/scalable/apps"
for f in "$STAGEDIR"/apps/*.desktop; do
	id=$(basename "$f" .desktop)
	install -m755 "$STAGEDIR/apps/$id" "$R/usr/local/lib/ztos/apps/$id"
	install -m644 "$f" "$R/usr/local/share/applications/$id.desktop"
	install -m644 "$STAGEDIR/apps/$id.svg" "$R/usr/local/share/icons/hicolor/scalable/apps/$id.svg"
done

if [ "$TARGET" != gsi ]; then	# the GSI has no user: its device layer's installer makes one
step "making your account"
if [ "$USERNAME" != alarm ] && ch id alarm > /dev/null 2>&1; then
	ch userdel -r alarm > /dev/null 2>&1 || ch userdel alarm
fi
if ! ch id "$USERNAME" > /dev/null 2>&1; then
	ch useradd -m -u 1000 -G wheel,video,audio,input -s /bin/bash "$USERNAME"
fi
printf '%s:%s\n' "$USERNAME" "$PASSWORD" | ch chpasswd
ch passwd -l root > /dev/null
install -Dm440 /dev/stdin "$R/etc/sudoers.d/10-wheel" <<'SUDO'
%wheel ALL=(ALL:ALL) ALL
SUDO
if [ -d "$R/etc/sudoers.d" ]; then
	chmod 440 "$R"/etc/sudoers.d/* 2>/dev/null || true
	if [ "$USERNAME" != alarm ] && [ -f "$R/etc/sudoers.d/99-ztos-alarm" ]; then
		sed -i "s/^alarm /$USERNAME /" "$R/etc/sudoers.d/99-ztos-alarm"
	fi
fi
# The one place the system learns who its user is (user.sh, ztos_user.py and
# ztos-user.h read it). ztos-user-dropins then writes what cannot read it:
# greetd's config, sudoers, polkit, the user manager's drop-in.
install -Dm644 /dev/stdin "$R/etc/ztos/user.conf" <<USERCONF
ZTOS_USER=$USERNAME
ZTOS_UID=$(ch id -u "$USERNAME")
ZTOS_GID=$(ch id -g "$USERNAME")
ZTOS_HOME=$(ch getent passwd "$USERNAME" | cut -d: -f6)
USERCONF
ch /usr/local/sbin/ztos-user-dropins > /dev/null
fi

step "time, language and name"
echo "$HOSTNAME" > "$R/etc/hostname"
ln -sf "/usr/share/zoneinfo/$TIMEZONE" "$R/etc/localtime"
sed -i 's/^#\(en_US.UTF-8 UTF-8\)/\1/' "$R/etc/locale.gen"
ch locale-gen > /dev/null
echo LANG=en_US.UTF-8 > "$R/etc/locale.conf"
: > "$R/etc/machine-id"

step "turning on ztOS's services"
# (systemd-resolved: names; NetworkManager hands it each network's DNS)
# (systemd-pstore: the kernel log of a crash or hang is kept in /var/lib/systemd/pstore)
# (zrnperfd: the CPU and GPU clocks from ztOSIAL's Perf1; without it the
# GPU rests at its lowest step and ztMETRO lags)
for u in greetd NetworkManager systemd-resolved sshd systemd-pstore ztos-pstore-keep ztoscoresensorframework ztos-update-check.timer \
		ztos-iald zrnperfd waydroid-container ztos-waydroid-images; do
	ch systemctl enable "$u" > /dev/null 2>&1
done
for u in systemd-networkd getty@tty1; do
	ch systemctl disable "$u" > /dev/null 2>&1 || true
done
for u in $DEVICE_UNITS; do
	ch systemctl enable "$u" > /dev/null 2>&1 || echo "(no $u)"
done

step "tidying up"
rm -f "$R/etc/resolv.conf"; ln -sf /run/systemd/resolve/stub-resolv.conf "$R/etc/resolv.conf"
rm -rf "$R/tmp/"* "$R/var/log/pacman.log"
install -d "$R/etc/ztos"
echo ztos > "$R/etc/ztos/built"
date -u +%Y-%m-%dT%H:%MZ >> "$R/etc/ztos/built"

if [ "$TARGET" = gsi ]; then
	# the kernel and its modules are each phone's (its device layer): not the GSI's
	if ch pacman -Qq linux-aarch64 > /dev/null 2>&1; then
		ch pacman --noconfirm -Rdd linux-aarch64 > /dev/null
	fi
	rm -f "$R/etc/ztos/user.conf"
	step "packing the GSI"
	umount -l "$R/var/cache/pacman/pkg" 2>/dev/null || true
	umount -l "$R/tmp" "$R/proc" 2>/dev/null || true
	umount -lR "$R/dev" 2>/dev/null || true
	umount -l "$R" 2>/dev/null || true
	rm -f "$GSITAR"
	bsdtar --zstd -cpf "$GSITAR" -C "$R" .
	echo STAGE-OK
	exit 0
fi

step "writing the system image"
# Re-arm first boot even when building from a previously used root tree.
# The image filesystem is smaller than the phone's USERDATA partition.
if [ "$TARGET" = starlte ]; then
	touch "$R/etc/zboot-firstboot"
fi
umount -l "$R/var/cache/pacman/pkg" 2>/dev/null || true
umount -l "$R/tmp" "$R/proc" 2>/dev/null || true
umount -lR "$R/dev" 2>/dev/null || true
umount -l "$R" 2>/dev/null || true
rm -f "$ROOTIMG"
"$MKE2FS" -q -t ext4 -L "$LABEL" -m 1 -O ^orphan_file -d "$R" "$ROOTIMG" "${SIZE}G"
echo STAGE-OK
'''


def run_stage(s: Settings, log: Path) -> None:
    """The system, built in the namespace; its steps shown as they go."""
    s.dir.mkdir(parents=True, exist_ok=True)
    stage_dir = s.dir / "stage"
    build_apps(stage_dir)
    mke2fs = "/usr/bin/mke2fs" if Path("/usr/bin/mke2fs").exists() else "/usr/sbin/mke2fs"
    env = {
        "ROOTFS": str(s.dir / "root"), "TARBALL": str(TARBALL), "SRC": str(ROOT), "STAGEDIR": str(stage_dir),
        "TARGET": s.target, "PACKAGES": " ".join(packages(s.target)), "USERNAME": s.user,
        "PASSWORD": s.password, "HOSTNAME": s.hostname, "TIMEZONE": s.timezone, "FRESH": "1" if s.fresh else "0",
        "ROOTIMG": str(s.dir / "root.img"), "SIZE": str(s.size_gb), "MKE2FS": mke2fs,
        "LABEL": "ZR_ARCH" if s.target == "starlte" else "ztos-root",
        "PKGCACHE": str(PKGCACHE),
        "DEVDIR": DEVICES.get(s.target, ""), "GSIVERSION": gsi_version(),
        # the device layer: devices/<dir> here; after the repository split, a
        # checkout of the device's own repository (ZTOS_DEVICE=<path>)
        "DEVSRC": os.environ.get("ZTOS_DEVICE") or str(ROOT / "devices" / DEVICES.get(s.target, "none")),
        "DEVROOT": os.environ.get("ZTOS_DEVICE") or str(ROOT),
        "GSITAR": str(s.dir / f"ztos-gsi-{gsi_version()}-aarch64.tar.zst"),
        "PATH": os.environ.get("PATH", "/usr/bin"),
    }
    script = s.dir / "stage.sh"
    script.write_text(STAGE)
    started = time.monotonic()
    total_pkgs = 0
    pkg_idx = 0
    in_progress = False
    with open(log, "w") as out:
        unshare_cmd = ["unshare", "--mount", "--fork", "--pid"] if os.geteuid() == 0 else [
            "unshare", "--map-auto", "--map-root-user", "--mount", "--fork", "--pid"
        ]
        proc = subprocess.Popen([*unshare_cmd, "sh", str(script)], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, bufsize=1)
        ok = False
        for line in proc.stdout:
            out.write(line)
            out.flush()
            if line.startswith("STEP "):
                if in_progress:
                    print()
                    in_progress = False
                minutes = (time.monotonic() - started) / 60
                say(f"[{minutes:4.1f} min] {line[5:].strip()}")
                total_pkgs = 0
                pkg_idx = 0
            elif line.strip() == "STAGE-OK":
                ok = True
            else:
                m = re.search(r"Packages \((\d+)\)", line)
                if m:
                    total_pkgs = int(m.group(1))
                    pkg_idx = 0
                elif line.startswith("installing "):
                    pkg_name = line[11:].rstrip(".\n")
                    pkg_idx += 1
                    pct = int((pkg_idx / total_pkgs) * 100) if total_pkgs > 0 else 0
                    bar_len = 16
                    filled = int(bar_len * pct / 100)
                    bar = "█" * filled + "░" * (bar_len - filled)
                    minutes = (time.monotonic() - started) / 60
                    status = f"[{bar}] {pct:3d}% ({pkg_idx}/{total_pkgs}) installing {pkg_name}"
                    print(f"\rztos: [{minutes:4.1f} min] {status[:60]:<60}", end="", flush=True)
                    in_progress = True
                elif " downloading...\n" in line:
                    dl_name = line.split()[0] if line.split() else ""
                    minutes = (time.monotonic() - started) / 60
                    status = f"downloading {dl_name}"
                    print(f"\rztos: [{minutes:4.1f} min] {status[:60]:<60}", end="", flush=True)
                    in_progress = True
                elif (line.startswith("cc ") or line.startswith("gcc ") or line.startswith("CC ") or
                      line.startswith("GEN ") or " -o " in line):
                    words = line.split()
                    target = words[-1] if words else ""
                    minutes = (time.monotonic() - started) / 60
                    status = f"compiling {target}"
                    print(f"\rztos: [{minutes:4.1f} min] {status[:60]:<60}", end="", flush=True)
                    in_progress = True
        if in_progress:
            print()
        proc.wait()
    if proc.returncode != 0 or not ok:
        tail = "".join(log.read_text().splitlines(True)[-25:])
        raise Fail(f"building the system failed (the whole log: {log}):\n{tail}")


def make_qemu_disk(s: Settings) -> Path:
    """A GPT disk, as a PC or a phone has: an EFI system partition with
    systemd-boot and the kernel, and the ztOS root."""
    rootimg, esp = s.dir / "root.img", s.dir / "esp.img"
    boot = s.dir / "stage" / "boot"
    kernel, initrd, boot_efi = boot / "Image", boot / "initramfs.img", boot / "BOOTAA64.EFI"
    for need in (kernel, initrd, boot_efi):
        if not need.exists():
            raise Fail(f"the build left no {need.name} (see the log)")
    say("writing the boot partition (systemd-boot)")
    esp.unlink(missing_ok=True)
    subprocess.run(["truncate", "-s", "256M", str(esp)], check=True)
    subprocess.run(["mkfs.fat", "-F", "32", "-n", "ZTOS-ESP", str(esp)], check=True, stdout=subprocess.DEVNULL)
    m = ["-i", str(esp)]
    for d in ("::/EFI", "::/EFI/BOOT", "::/loader", "::/loader/entries", "::/ztos"):
        subprocess.run(["mmd", *m, d], check=True)
    loader = s.dir / "loader.conf"
    loader.write_text("default ztos.conf\ntimeout 0\nconsole-mode keep\n")
    entry = s.dir / "ztos.conf"
    entry.write_text("title ztOS\nlinux /ztos/Image\ninitrd /ztos/initramfs.img\n"
                     "options root=PARTLABEL=ztos-root rw quiet loglevel=3 console=ttyAMA0 console=tty1\n")
    for src, dst in ((boot_efi, "::/EFI/BOOT/BOOTAA64.EFI"), (loader, "::/loader/loader.conf"),
                     (entry, "::/loader/entries/ztos.conf"), (kernel, "::/ztos/Image"),
                     (initrd, "::/ztos/initramfs.img")):
        subprocess.run(["mcopy", *m, str(src), dst], check=True)
    say("putting the disk together")
    disk = s.dir / "ztos-qemu.img"
    mib = 1024 * 1024
    esp_size, root_size = esp.stat().st_size, rootimg.stat().st_size
    total = mib + esp_size + root_size + mib
    disk.unlink(missing_ok=True)
    subprocess.run(["truncate", "-s", str(total), str(disk)], check=True)
    layout = (f"label: gpt\n"
              f"start=2048, size={esp_size // 512}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=ztos-esp\n"
              f"start={2048 + esp_size // 512}, size={root_size // 512}, "
              f"type=B921B045-1DF0-41C3-AF44-4C6F280D3FAE, name=ztos-root\n")
    subprocess.run(["sfdisk", "-q", str(disk)], input=layout, text=True, check=True)
    subprocess.run(["dd", f"if={esp}", f"of={disk}", "bs=1M", f"seek=1", "conv=notrunc,sparse", "status=none"],
                   check=True)
    subprocess.run(["dd", f"if={rootimg}", f"of={disk}", "bs=1M", f"seek={1 + esp_size // mib}",
                    "conv=notrunc,sparse", "status=none"], check=True)
    esp.unlink()
    return disk


def build(s: Settings) -> Path:
    check_host()
    fetch_tarball()
    log = s.dir / "build.log"
    rootimg = s.dir / ("ztos-gsi-%s-aarch64.tar.zst" % gsi_version() if s.target == "gsi" else "root.img")
    if rootimg.exists() and not s.fresh:
        say(f"using existing system image: {rootimg}")
    else:
        say(f"building ztOS for {s.target} (log: {log})")
        run_stage(s, log)
    if s.target == "gsi":
        tarball = s.dir / f"ztos-gsi-{gsi_version()}-aarch64.tar.zst"
        digest = subprocess.run(["sha256sum", tarball.name], cwd=s.dir, check=True,
                                capture_output=True, text=True).stdout
        (s.dir / (tarball.name + ".sha256")).write_text(digest)
        say(f"ready: {tarball} ({tarball.stat().st_size // (1024 ** 2)} MB), the GSI for ztConflo (--gsi)")
        return tarball
    if s.target == "qemu":
        disk = make_qemu_disk(s)
        (s.dir / "root.img").unlink(missing_ok=True)
        say(f"ready: {disk} ({disk.stat().st_size // (1024 ** 3)} GB)")
        return disk
    image = s.dir / "root.img"
    say(f"ready: {image} (the root filesystem, for USERDATA)")
    return image
