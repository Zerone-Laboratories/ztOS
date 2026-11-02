# ZroovMusic engine. Needs libmpv, libsystemd, json-c, libcurl.
CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=gnu11
PREFIX ?= /usr/local

all: zroovd sources/zroov-source-yt

zroovd: zroovd.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs mpv libsystemd json-c)

sources/zroov-source-yt: sources/zroov-source-yt.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs libcurl json-c)

install: all
	install -Dm755 zroovd $(DESTDIR)$(PREFIX)/bin/zroovd
	install -Dm755 sources/zroov-source-yt $(DESTDIR)$(PREFIX)/lib/zroovmusic/sources/zroov-source-yt

clean:
	rm -f zroovd sources/zroov-source-yt

.PHONY: all install clean
