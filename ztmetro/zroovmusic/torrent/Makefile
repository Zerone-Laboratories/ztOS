CXX ?= g++
CXXFLAGS ?= -O2 -g0
EXTRA_CXXFLAGS ?=
PKG_CFLAGS = $(shell pkg-config --cflags libtorrent-rasterbar)
LDLIBS = $(shell pkg-config --libs libtorrent-rasterbar) -lssl -lcrypto -lpthread
zrn-zroov-btstream: btstream.cpp
	$(CXX) -std=c++17 $(CXXFLAGS) $(PKG_CFLAGS) $(EXTRA_CXXFLAGS) $< -o $@ $(LDLIBS)
test:
	python3 -m unittest discover -s tests -v
clean:
	rm -f zrn-zroov-btstream
.PHONY: test clean
PREFIX ?= /usr/local
SRC = $(DESTDIR)$(PREFIX)/lib/zroovmusic/sources
install: zrn-zroov-btstream
	install -Dm755 zroov-source-torrent $(SRC)/zroov-source-torrent
	install -Dm644 zroov_torrent.py $(SRC)/zroov_torrent.py
	install -Dm755 zrn-zroov-btstream $(SRC)/zrn-zroov-btstream
.PHONY: install
