#!/usr/bin/env bash
# Run on the build machine. No phone access or credentials are needed here.
set -euo pipefail
atl_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
atl_work=${ATL_WORKDIR:-"$atl_source/../../build/atl"}
mkdir -p "$atl_work"
atl_work=$(cd "$atl_work" && pwd)
atl_image=${ATL_BASE_IMAGE:-alarm-aarch64:base}
atl_container=${ATL_CONTAINER:-ztos-atl-build}
if ! docker image inspect "$atl_image" >/dev/null 2>&1; then
    atl_rootfs=${ATL_ROOTFS:-"$atl_source/../../build/alarm.tar.gz"}
    if [ ! -f "$atl_rootfs" ]; then
        echo "Missing ARM64 rootfs: $atl_rootfs (set ATL_ROOTFS to an Arch Linux ARM aarch64 tarball)" >&2
        exit 1
    fi
    docker import --platform linux/arm64 "$atl_rootfs" "$atl_image"
fi
if ! docker run --rm --platform linux/arm64 "$atl_image" /usr/bin/true >/dev/null 2>&1; then
    docker run --privileged --rm tonistiigi/binfmt --install arm64
fi
if ! docker container inspect "$atl_container" >/dev/null 2>&1; then
    docker run -d --platform linux/arm64 --name "$atl_container" \
        --cpus="${ATL_CPUS:-12}" --memory="${ATL_MEMORY:-16g}" \
        -v "$atl_work:/work" -v "$atl_source/packages:/recipes:ro" "$atl_image" sleep infinity
else
    docker start "$atl_container" >/dev/null
fi
docker exec "$atl_container" bash -s < "$atl_source/build-container.sh"
printf '\nARM64 runtime packages: %s/packages\n' "$atl_work"
