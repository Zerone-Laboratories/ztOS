#!/bin/bash
set -euo pipefail
pacman-key --init >/dev/null 2>&1
pacman-key --populate archlinuxarm >/dev/null 2>&1
sed -i '/^DownloadUser/d' /etc/pacman.conf
grep -q '^DisableSandbox' /etc/pacman.conf || sed -i '/^\[options\]/a DisableSandbox' /etc/pacman.conf
pacman -Syu --needed --noconfirm base-devel git sudo meson ninja python jdk8-openjdk
id builder >/dev/null 2>&1 || useradd -m builder
printf 'builder ALL=(ALL) NOPASSWD: ALL\n' > /etc/sudoers.d/ztos-atl-builder
printf 'MAKEFLAGS="-j8"\nOPTIONS=(!debug !lto strip docs !libtool !staticlibs emptydirs zipman purge)\n' > /home/builder/.makepkg.conf
archlinux-java set java-8-openjdk
mkdir -p /work/sources /work/packages /work/logs
chown -R builder:builder /work/sources /work/packages /work/logs
atl_current=()
for package in bsd-compat-headers wolfssl-jni bionic_translation vixl libopensles-standalone art_standalone android_translation_layer; do
    mkdir -p "/work/sources/$package"
    cp -a "/recipes/$package/." "/work/sources/$package/"
    chown -R builder:builder "/work/sources/$package"
    # Old build images can contain the non-git aliases. Replace them only in
    # this isolated builder; the pinned recipes provide their stable names.
    if [ "$package" = bionic_translation ] && pacman -Q bionic_translation >/dev/null 2>&1; then
        pacman -Rdd --noconfirm bionic_translation
    fi
    if [ "$package" = libopensles-standalone ] && pacman -Q libopensles-standalone >/dev/null 2>&1; then
        pacman -Rdd --noconfirm libopensles-standalone
    fi
    cd "/work/sources/$package"
    echo "Building ARM64 $package"
    sudo -u builder env PKGDEST=/work/packages makepkg --syncdeps --install --needed \
        --noconfirm --nocheck >"/work/logs/$package.log" 2>&1 || {
            tail -60 "/work/logs/$package.log" >&2; exit 1;
        }
    while IFS= read -r archive; do
        atl_current+=("$archive")
    done < <(sudo -u builder env PKGDEST=/work/packages makepkg --packagelist)
done
readelf -h /usr/bin/android-translation-layer | grep -q 'Machine:.*AArch64'
ldd /usr/bin/android-translation-layer > /work/packages/ldd.txt
if grep -q 'not found' /work/packages/ldd.txt; then cat /work/packages/ldd.txt; exit 1; fi
pacman -Q > /work/packages/build-packages.txt
cd /work/packages
sha256sum "${atl_current[@]}" | sed 's|  /work/packages/|  |' > SHA256SUMS
