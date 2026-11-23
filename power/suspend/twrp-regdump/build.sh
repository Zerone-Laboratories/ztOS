#!/bin/bash
# Build zrn_twrp_regdump.ko against TWRP's 4.9.118-g36e6ddff22b9 config, natively on the phone.
set -ex
export PATH=$HOME/zrn-twrp-regdump/bin:$PATH
K=~/zrn-twrp-regdump/k49
M=~/zrn-twrp-regdump
H='HOSTCFLAGS=-O2 -fcommon -std=gnu11 -w'
cd $K
cp ~/twrp-k49.config .config
printf %s -g36e6ddff22b9 > .scmversion
make ARCH=arm64 CROSS_COMPILE= CC=gcc "$H" olddefconfig
make ARCH=arm64 CROSS_COMPILE= CC=gcc "$H" -j2 modules_prepare
cat include/config/kernel.release
make ARCH=arm64 CROSS_COMPILE= CC=gcc "$H" KCFLAGS=-w M=$M modules
modinfo $M/zrn_twrp_regdump.ko || true
