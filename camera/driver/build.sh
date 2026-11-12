#!/bin/bash
# On the workstation, against the running kernel's tree (build-snd.sh pattern).
# The laptop's build/linux-7.1 is not the tree the phone's kernel came from.
set -euo pipefail
BASE=/home/itelasoft/ztos-kernel-waydroid
export PATH="$BASE/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"
export ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- LC_ALL=C KBUILD_MODPOST_WARN=1
make -C "$BASE/linux" O=out M="$(cd "$(dirname "$0")" && pwd)" modules
