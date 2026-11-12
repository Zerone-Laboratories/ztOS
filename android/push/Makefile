# zrn-pushd: builds on the phone itself (aarch64 Arch Linux ARM), like ztmetro.
CFLAGS ?= -O2 -g
WARN = -std=c11 -Wall -Wno-unused-parameter -Wno-format-truncation
PKGS = libsystemd openssl libcurl zlib
PREFIX ?= /usr/local

all: zrn-pushd

zrn-pushd: zrn-pushd.c apk.c apk.h pb.h
	$(CC) $(CFLAGS) $(WARN) $(shell pkg-config --cflags $(PKGS)) -o $@ zrn-pushd.c apk.c \
		$(shell pkg-config --libs $(PKGS)) -lpthread

# The daemon, its Waydroid helper, the user unit (started with the session)
# and the D-Bus name ATL apps send their REGISTER to. CAP_WAKE_ALARM lets
# the heartbeat wake a sleeping phone (CLOCK_BOOTTIME_ALARM).
install: zrn-pushd
	install -Dm755 zrn-pushd $(DESTDIR)$(PREFIX)/bin/zrn-pushd
	install -Dm755 zrn-pushd-waydroid $(DESTDIR)$(PREFIX)/lib/zrn-pushd/zrn-pushd-waydroid
	install -Dm644 zrn-pushd.service $(DESTDIR)/usr/lib/systemd/user/zrn-pushd.service
	install -Dm644 com.google.android.gms.service $(DESTDIR)/usr/share/dbus-1/services/com.google.android.gms.service
	-setcap cap_wake_alarm+ep $(DESTDIR)$(PREFIX)/bin/zrn-pushd

clean:
	rm -f zrn-pushd

.PHONY: all install clean
