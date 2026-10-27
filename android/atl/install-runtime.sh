#!/usr/bin/env bash
# Run as root on ARM64 ztOS, or from the installer's ARM64 chroot.
set -euo pipefail
atl_packages=${1:?Usage: install-runtime.sh PACKAGE_DIRECTORY}
cd "$atl_packages"
test "$(uname -m)" = aarch64 || { echo 'ATL runtime requires ARM64 ztOS' >&2; exit 1; }
sha256sum --check SHA256SUMS
atl_archives=()
while read -r checksum archive; do
    case "$archive" in
        */*|*..*) echo 'Invalid archive name in SHA256SUMS' >&2; exit 1 ;;
        *.pkg.tar.xz|*.pkg.tar.zst|*.pkg.tar.gz) atl_archives+=("./$archive") ;;
        *) echo 'Invalid archive type in SHA256SUMS' >&2; exit 1 ;;
    esac
done < SHA256SUMS
[ "${#atl_archives[@]}" -gt 0 ] || { echo 'No ATL runtime packages found' >&2; exit 1; }
for archive in "${atl_archives[@]}"; do
    metadata=$(bsdtar -xOf "$archive" .PKGINFO)
    if ! printf '%s\n' "$metadata" | grep -Eq '^arch = (aarch64|any)$'; then
        echo "Refusing a package built for another CPU: $archive" >&2; exit 1
    fi
done
# Official repository dependencies; the custom ART/bionic packages follow together.
pacman --disable-sandbox -S --needed --noconfirm \
    alsa-lib bubblewrap expat ffmpeg fontconfig gcc-libs glibc gtk4 icu \
    jre8-openjdk libbsd libcap libdrm libglvnd libgudev libportal libpng \
    libsecret libunwind lz4 openxr openssl sqlite vulkan-icd-loader wayland \
    webkitgtk-6.0 xz zlib
pacman --disable-sandbox -U --needed --noconfirm "${atl_archives[@]}"

ldconfig
ldd /usr/bin/android-translation-layer
