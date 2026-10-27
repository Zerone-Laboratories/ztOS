# ztOSIAL: the daemon, the CLI, the interface specs and the client libraries.
#   make            libztos-ial (C)
#   make install DESTDIR=/path    into a root (the GSI build does this)
PREFIX ?= /usr
CFLAGS ?= -O2 -g
SD_CFLAGS := $(shell pkg-config --cflags libsystemd 2>/dev/null)
SD_LIBS := $(shell pkg-config --libs libsystemd 2>/dev/null || echo -lsystemd)

all: lib/libztos-ial.so lib/libztos-ial.a

lib/libztos-ial.o: lib/ztos-ial.c lib/ztos-ial.h
	$(CC) $(CFLAGS) -fPIC -std=c11 -Wall -Wextra -Wno-unused-parameter $(SD_CFLAGS) -c -o $@ lib/ztos-ial.c

lib/libztos-ial.so: lib/libztos-ial.o
	$(CC) -shared -Wl,-soname,libztos-ial.so.1 -o $@ $< $(SD_LIBS)

lib/libztos-ial.a: lib/libztos-ial.o
	$(AR) rcs $@ $<

install: all
	install -Dm755 bin/ztos-iald $(DESTDIR)$(PREFIX)/bin/ztos-iald
	install -Dm755 bin/ztos-ial $(DESTDIR)$(PREFIX)/bin/ztos-ial
	for f in ztos_ial/*.py ztos_ial/backends/*.py; do install -Dm644 $$f $(DESTDIR)$(PREFIX)/lib/ztos-ial/$$f; done
	for f in spec/*.xml; do install -Dm644 $$f $(DESTDIR)$(PREFIX)/share/ztos-ial/$$f; done
	for f in spec/*.xml; do install -Dm644 $$f $(DESTDIR)$(PREFIX)/share/dbus-1/interfaces/$$(basename $$f); done
	install -Dm644 gsi.json $(DESTDIR)$(PREFIX)/share/ztos/gsi.json
	install -Dm644 systemd/ztos-iald.service $(DESTDIR)$(PREFIX)/lib/systemd/system/ztos-iald.service
	install -Dm644 dbus/org.ztos.ial.conf $(DESTDIR)$(PREFIX)/share/dbus-1/system.d/org.ztos.ial.conf
	install -Dm644 lib/ztos-ial.h $(DESTDIR)$(PREFIX)/include/ztos-ial.h
	install -Dm755 lib/libztos-ial.so $(DESTDIR)$(PREFIX)/lib/libztos-ial.so.1
	ln -sf libztos-ial.so.1 $(DESTDIR)$(PREFIX)/lib/libztos-ial.so
	install -Dm644 lib/libztos-ial.a $(DESTDIR)$(PREFIX)/lib/libztos-ial.a
	install -Dm644 lib/ztos-ial.pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/ztos-ial.pc
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/ztos-ial/README.md

clean:
	rm -f lib/*.o lib/*.so lib/*.a

.PHONY: all install clean
