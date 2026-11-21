#!/bin/bash
# Build zrn-mobicore.ko and zrn-etspi.ko against the phone's running kernel tree
# (workstation ~/ztos-kernel-waydroid/linux, O=out: 7.1.0-zboot #10).
set -uo pipefail
BASE=/home/itelasoft/ztos-kernel-waydroid
export PATH="$BASE/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"
export ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- LC_ALL=C KBUILD_MODPOST_WARN=1
D=$(cd "$(dirname "$0")" && pwd)
rc=0
for m in mobicore etspi; do
	make -C "$BASE/linux" O=out M="$D/driver/$m" modules -j24 -k || rc=1
done
cd "$D" && sha256sum driver/*/*.ko 2>/dev/null > SHA256SUMS; cat SHA256SUMS
exit $rc
