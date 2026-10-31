# ztwhatsappd. Needs: libsodium openssl sqlite3 zlib libcurl, and nlohmann-json at build time.
CXX ?= g++
PKGS = libsodium openssl sqlite3 zlib libcurl
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wno-deprecated-declarations -pthread $(shell pkg-config --cflags $(PKGS) nlohmann_json)
LDLIBS = $(shell pkg-config --libs $(PKGS)) -pthread
BUILD ?= build
PREFIX ?= /usr

CORE = crypto node noise sigproto appstate
SRV = $(CORE) ws store ipc fake client wamsg
OBJS = $(SRV:%=$(BUILD)/%.o) $(BUILD)/main.o

all: $(BUILD)/ztwhatsappd

$(BUILD)/%.o: src/%.cpp $(wildcard src/*.h) src/tokens.inc
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/ztwhatsappd: $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_core: tests/test_core.cpp $(CORE:%=$(BUILD)/%.o)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/test_e2e: tests/test_e2e.cpp tests/fakeserver.h $(SRV:%=$(BUILD)/%.o)
	$(CXX) $(CXXFLAGS) tests/test_e2e.cpp $(SRV:%=$(BUILD)/%.o) -o $@ $(LDLIBS)

# GOFIX=path/to/gofix adds live interop against go-libsignal. The Go tool is test-only.
test: $(BUILD)/test_core $(BUILD)/test_e2e
	env -u LD_PRELOAD $(BUILD)/test_core tests/fixtures $(GOFIX)
	env -u LD_PRELOAD $(BUILD)/test_e2e

install: $(BUILD)/ztwhatsappd
	install -Dm755 $(BUILD)/ztwhatsappd $(DESTDIR)$(PREFIX)/bin/ztwhatsappd
	install -Dm644 dist/zrn-ztwhatsappd.service $(DESTDIR)$(PREFIX)/lib/systemd/user/zrn-ztwhatsappd.service

clean:
	rm -rf $(BUILD)

.PHONY: all test install clean
