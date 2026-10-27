#!/bin/bash
# audio/build.sh: build everything sound needs as modules for the running
# kernel (7.1.0-zboot), into audio/modules/.
#
# The boot image's kernel has no sound core. build/linux-7.1/out-snd is the
# same configuration with CONFIG_SOUND/SND/SND_PCM added as modules and
# nothing built-in changed, so its modules load on the kernel that is flashed.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
B=$ROOT/build K=$ROOT/build/linux-7.1
export PATH=$B/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH
export LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1
quiet() { grep -E "error|warning: [a-z]" || true; }

# out-snd follows out/.config: rebuilt whenever the kernel's configuration is
# newer. A stale one (12 Oct, before CONFIG_COMPAT) gave snd.ko and the ABOX
# driver another struct snd_card layout than the running kernel, and the first
# SoC sleep oopsed in snd_power_change_state (2026-10-26).
if [ ! -f "$K/out-snd/.config" ] || [ "$K/out/.config" -nt "$K/out-snd/.config" ]; then
	rm -rf "$K/out-snd"
	mkdir -p "$K/out-snd" && cp "$K/out/.config" "$K/out-snd/.config"
	(cd "$K" && scripts/config --file out-snd/.config -m SOUND -m SND -m SND_DUMMY -e SND_DRIVERS \
		-d SND_SOC -d SND_USB -d SND_HRTIMER -d SND_ALOOP -d SND_OSSEMUL -d SND_SEQUENCER -d SND_PCI \
		-d SND_HDA -d SND_SPI -d SND_ARM -d SND_VIRTIO)
	make -s -C "$K" O=out-snd olddefconfig
	make -s -j"$(nproc)" -C "$K" O=out-snd modules_prepare
fi
make -s -j"$(nproc)" -C "$K" O=out-snd sound/soundcore.ko sound/core/snd.ko sound/core/snd-timer.ko \
	sound/core/snd-pcm.ko drivers/i2c/algos/i2c-algo-bit.ko drivers/i2c/busses/i2c-gpio.ko 2>&1 | quiet
make -s -C "$K" O=out-snd M="$ROOT/audio/driver" modules 2>&1 | quiet
mkdir -p "$ROOT/audio/modules"
cp "$K"/out-snd/sound/soundcore.ko "$K"/out-snd/sound/core/snd{,-timer,-pcm}.ko \
	"$K"/out-snd/drivers/i2c/algos/i2c-algo-bit.ko "$K"/out-snd/drivers/i2c/busses/i2c-gpio.ko \
	"$ROOT/audio/driver/snd-zrn-abox.ko" "$ROOT/audio/modules/"
ls -l "$ROOT/audio/modules"
