# ztwhatsapp-notify. Standalone: nothing else in the tree includes this yet.
CXX ?= g++
CXXFLAGS ?= -O2 -g
WARN = -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
PREFIX ?= /usr

all: ztwhatsapp-notify

ztwhatsapp-notify: main.cpp policy.cpp json.cpp policy.hpp json.hpp
	$(CXX) $(CXXFLAGS) $(WARN) $(shell pkg-config --cflags libsystemd) -o $@ main.cpp policy.cpp json.cpp \
		$(shell pkg-config --libs libsystemd)

tests/policy-test: tests/policy-test.cpp policy.cpp json.cpp policy.hpp json.hpp
	$(CXX) $(CXXFLAGS) $(WARN) -fsanitize=address,undefined -I. -o $@ tests/policy-test.cpp policy.cpp json.cpp

check: ztwhatsapp-notify tests/policy-test
	tests/policy-test
	python3 -I tests/e2e.py

install: ztwhatsapp-notify
	install -Dm755 ztwhatsapp-notify $(DESTDIR)$(PREFIX)/bin/ztwhatsapp-notify
	install -Dm644 zrn-ztwhatsapp-notify.service $(DESTDIR)$(PREFIX)/lib/systemd/user/zrn-ztwhatsapp-notify.service

clean:
	rm -f ztwhatsapp-notify tests/policy-test

.PHONY: all check install clean
