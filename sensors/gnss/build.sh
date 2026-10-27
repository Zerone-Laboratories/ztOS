#!/bin/sh
# Build zrn-gnss-nmea for bionic (Android 10 arm64) with plain clang + lld, no NDK.
# The phone's libc.so/libdl.so are replaced at link time by stubs that only
# carry the symbol names; at run time /system/bin/linker64 in /run/android
# binds the real ones.  Output: ./zrn-gnss-nmea (arm64 PIE, interpreter
# /system/bin/linker64).
set -eu
cd "$(dirname "$0")"
T=--target=aarch64-linux-android29
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
F="-fPIC -fno-stack-protector -fno-builtin -ffreestanding -nostdinc -nostdlib"
cat >"$O/libc.c" <<'X'
void dprintf(){} void socket(){} void bind(){} void listen(){} void accept4(){}
void setsockopt(){} void send(){} void close(){} void unlink(){} void chmod(){}
void sleep(){} void strcmp(){} void strlen(){} void strncpy(){} void exit(){}
void __errno(){} void __libc_init(){} void pthread_create(){}
void pthread_mutex_lock(){} void pthread_mutex_unlock(){}
X
cat >"$O/libdl.c" <<'X'
void dlopen(){} void dlsym(){} void dlerror(){}
X
clang $T $F -shared -fuse-ld=lld -Wl,-soname,libc.so  -o "$O/libc.so"  "$O/libc.c"
clang $T $F -shared -fuse-ld=lld -Wl,-soname,libdl.so -o "$O/libdl.so" "$O/libdl.c"
clang $T $F -O2 -Wall -fPIE -pie -fuse-ld=lld \
	-Wl,--dynamic-linker=/system/bin/linker64 -Wl,-z,now -Wl,--hash-style=both \
	-Wl,-e,_start -o zrn-gnss-nmea zrn-gnss-nmea.c -L"$O" -lc -ldl
echo built "$(pwd)/zrn-gnss-nmea"
