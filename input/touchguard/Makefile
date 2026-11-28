PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
UNITDIR ?= /etc/systemd/system

zrn-touchguard: zrn-touchguard.c
	$(CC) $(CFLAGS) -o $@ $< -lm

install: zrn-touchguard
	install -Dm755 zrn-touchguard $(DESTDIR)$(PREFIX)/bin/zrn-touchguard
	sed 's|@BINDIR@|$(PREFIX)/bin|' zrn-touchguard.service > zrn-touchguard.service.out
	install -Dm644 zrn-touchguard.service.out $(DESTDIR)$(UNITDIR)/zrn-touchguard.service
	rm -f zrn-touchguard.service.out
	test -e $(DESTDIR)/etc/zrn-touchguard.conf || install -Dm644 zrn-touchguard.conf $(DESTDIR)/etc/zrn-touchguard.conf

clean:
	rm -f zrn-touchguard

.PHONY: install clean
