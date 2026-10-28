#!/usr/bin/env bash
# Build a new ext4 system image. Never modify the source image or Android data.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run as root.' >&2; exit 1; }
source_image=${1:-/usr/share/waydroid-extra/images/system.img.stock}
output=${2:-/usr/share/waydroid-extra/images/system.img.lean}
[[ -f $source_image && ! -e $output ]] || { echo 'Source missing or output already exists.' >&2; exit 1; }
for tool in mount umount e2fsck resize2fs tune2fs; do command -v "$tool" >/dev/null; done
work=$(mktemp -d "$(dirname "$output")/.strip.XXXXXX")
cleanup() {
    if mountpoint -q "$work/root"; then umount "$work/root" || return; fi
    rm -rf -- "$work"
}
trap cleanup EXIT
cp --reflink=auto --sparse=always --preserve=all "$source_image" "$work/system.img"
# Stock images can have zero free blocks; grow the copy before creating init files.
rc=0
e2fsck -fy "$work/system.img" || rc=$?
(( rc <= 1 ))
initial_blocks=$(tune2fs -l "$work/system.img" | awk '/^Block count:/ {print $3}')
initial_size=$(tune2fs -l "$work/system.img" | awk '/^Block size:/ {print $3}')
resize2fs "$work/system.img" "$((initial_blocks + 64 * 1024 * 1024 / initial_size))"
mkdir "$work/root"
mount -o loop,rw "$work/system.img" "$work/root"
root=$work/root
[[ -d $root/system/framework && -d $root/system/product/app/LatinIME ]] || {
    echo 'Unexpected image layout; refusing to strip.' >&2; exit 1;
}
# This image ships only init.zygote64_32.rc. A property alone cannot start zygote64.
zygote_dir=$root/system/etc/init/hw
if [[ ! -f $zygote_dir/init.zygote64.rc ]]; then
    [[ -f $zygote_dir/init.zygote64_32.rc ]]
    sed '/^service zygote_secondary /,$d' "$zygote_dir/init.zygote64_32.rc" > "$zygote_dir/init.zygote64.rc"
    chmod --reference="$zygote_dir/init.zygote64_32.rc" "$zygote_dir/init.zygote64.rc"
    chown --reference="$zygote_dir/init.zygote64_32.rc" "$zygote_dir/init.zygote64.rc"
    # Preserve the SELinux label when the source filesystem has one.
    if command -v getfattr >/dev/null && command -v setfattr >/dev/null; then
        label=$(getfattr --only-values -n security.selinux "$zygote_dir/init.zygote64_32.rc" 2>/dev/null | tr -d '\000') || label=
        [[ -z $label ]] || setfattr -n security.selinux -v "$label" "$zygote_dir/init.zygote64.rc"
    fi
fi
# HOME is com.android.launcher3/.uioverrides.QuickstepLauncher on the tested image.
# Keep Launcher3: disabling it leaves HOME unresolved. Keep SystemUI too: hiding
# its bars in multi-window mode does not prove its framework services are unused.
# Also keep MmsService (telephony dependencies), providers and all framework files.
removed=0
for path in \
    system/product/app/{Aperture,ApertureLensLauncher,Backgrounds,DeskClock,Etar,ExactCalculator,Gallery2,Jelly,LocalContactsBackup,PhotoTable,Recorder} \
    system/product/priv-app/{Eleven,Contacts} \
    system/app/{BasicDreams,EasterEgg,PrintRecommendationService,NfcNci,SecureElement,WallpaperBackup,HTMLViewer,CameraExtensionsProxy} \
    system/priv-app/{LiveWallpapersPicker,ManagedProvisioning,BuiltInPrintService,DynamicSystemInstallationService,MtpService}; do
    if [[ -d $root/$path && ! -L $root/$path ]]; then
        du -sk "$root/$path"
        rm -rf -- "$root/$path"
        removed=$((removed + 1))
    else
        echo "Absent: $path"
    fi
done
for path in system/framework system/product/app/LatinIME system/product/app/webview; do
    [[ -d $root/$path ]] || { echo "Missing required $path" >&2; exit 1; }
done
sync
umount "$root"
# e2fsck returns 1 when it corrects filesystem errors.
check() { local rc=0; e2fsck -fy "$1" || rc=$?; (( rc <= 1 )); }
check "$work/system.img"
resize2fs -M "$work/system.img"
blocks=$(tune2fs -l "$work/system.img" | awk '/^Block count:/ {print $3}')
block_size=$(tune2fs -l "$work/system.img" | awk '/^Block size:/ {print $3}')
# Leave 64 MiB slack for filesystem bookkeeping and small future edits.
resize2fs "$work/system.img" "$((blocks + 64 * 1024 * 1024 / block_size))"
check "$work/system.img"
chmod --reference="$source_image" "$work/system.img"
chown --reference="$source_image" "$work/system.img"
mv -T "$work/system.img" "$output"
echo "Removed $removed directories. Image bytes: $(stat -c %s "$source_image") -> $(stat -c %s "$output")"
