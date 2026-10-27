# Builds on the phone itself (aarch64 Arch Linux ARM, wlroots 0.20).
PKGS = wlroots-0.20 wayland-server xkbcommon cairo pangocairo librsvg-2.0 gdk-pixbuf-2.0 pixman-1 libdrm egl glesv2 libinput
APP_PKGS = xkbcommon wayland-client wayland-egl egl glesv2 cairo pangocairo gdk-pixbuf-2.0
WP := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG := $(WP)/stable/xdg-shell/xdg-shell.xml
CFLAGS ?= -O2 -g
WARN = -std=c11 -Wall -Wno-unused-parameter

# Programs built on another machine (the laptop's x86-64 builds, copied over
# with the sources by tar) look up to date to make. Delete every executable in
# the tree that is not for this machine before make looks at anything, so it
# builds them again here; check-arch below still refuses to install one.
HOST_MACHINE := $(shell od -An -tx1 -j18 -N2 /bin/sh | tr -d ' \n')
FOREIGN := $(shell for f in $$(find . -type f -perm -u+x ! -name '*.sh' ! -name '*.py' ! -path './.git/*'); do \
	[ "$$(od -An -tc -j1 -N3 $$f 2>/dev/null | tr -d ' ')" = ELF ] || continue; \
	[ "$$(od -An -tx1 -j18 -N2 $$f | tr -d ' \n')" = "$(HOST_MACHINE)" ] || { rm -f $$f; echo $$f; }; done)
ifneq ($(FOREIGN),)
$(info removed programs built for another machine, to build them here: $(FOREIGN))
endif

all: auth/ztos-polkit-agent sounds/ztos-sounds ztmetro apps/ztapps settings/ztsettings devtest/zrndevtest devices/ztdevices perfd/zrnperfd sensord/ztoscsf-sensord modemd/ztoscorenetworkingd \
	csf/ztoscsf notifyd/ztnotifyd phone/ztphone messages/ztmessages contacts/ztcontacts splash/ztos-splash \
	store/ztstore

sounds/ztos-sounds: sounds/ztos-sounds.c
	$(CC) $(CFLAGS) $(WARN) $(shell pkg-config --cflags libpulse) -o $@ $< $(shell pkg-config --libs libpulse) -lm

ztmetro: ztmetro.c auth/prompt.c xdg-shell-protocol.h
	$(CC) $(CFLAGS) $(WARN) -DWLR_USE_UNSTABLE -I. $(shell pkg-config --cflags $(PKGS)) \
		-o $@ ztmetro.c $(shell pkg-config --libs $(PKGS)) -lm -lpthread

xdg-shell-protocol.h:
	wayland-scanner server-header $(XDG) $@

# the list of applications and their glyphs, kept for the shell
apps/ztapps: apps/ztapps.c
	$(CC) $(CFLAGS) $(WARN) $(shell pkg-config --cflags cairo gdk-pixbuf-2.0 librsvg-2.0) -o $@ apps/ztapps.c \
		$(shell pkg-config --libs cairo gdk-pixbuf-2.0 librsvg-2.0) -lm

# ZrnMetroCore: the window, motion and widgets our own apps share
VIEWPORTER := $(WP)/stable/viewporter/viewporter.xml
CORE = core/zrnmetrocore.c core/zrnmetrogpu.c core/xdg-shell-protocol.c core/viewporter-protocol.c
CORE_DEPS = $(CORE) core/zrnmetrocore.h core/zrnmetrogpu.h core/zrnmetrorender.h core/xdg-shell-client-protocol.h core/viewporter-client-protocol.h
APP_BUILD = $(CC) $(CFLAGS) $(WARN) -Icore $(shell pkg-config --cflags $(APP_PKGS))
APP_LIBS = $(shell pkg-config --libs $(APP_PKGS)) -lm

settings/ztsettings: settings/ztsettings.c $(CORE_DEPS)
	$(APP_BUILD) -o $@ settings/ztsettings.c $(CORE) $(APP_LIBS)

devtest/zrndevtest: devtest/zrndevtest.c $(CORE_DEPS)
	$(APP_BUILD) -o $@ devtest/zrndevtest.c $(CORE) $(APP_LIBS)

# Devices and Services: what is running, crashed or missing
devices/ztdevices: devices/ztdevices.c $(CORE_DEPS)
	$(APP_BUILD) -o $@ devices/ztdevices.c $(CORE) $(APP_LIBS)

perfd/zrnperfd: perfd/zrnperfd.c perfd/tickle.c perfd/tickle.h
	$(CC) $(CFLAGS) $(WARN) -o $@ perfd/zrnperfd.c perfd/tickle.c

sensord/ztoscsf-sensord: sensord/ztoscsf-sensord.c
	$(CC) $(CFLAGS) $(WARN) -o $@ sensord/ztoscsf-sensord.c -lm

# the modem's state for the shell (signal, operator, data), and airplane mode, SMS
modemd/ztoscorenetworkingd: modemd/ztoscorenetworkingd.c
	$(CC) $(CFLAGS) $(WARN) -o $@ modemd/ztoscorenetworkingd.c

# notifications from other programs, what is playing, and push
notifyd/ztnotifyd: notifyd/ztnotifyd.c
	$(CC) $(CFLAGS) $(WARN) $(shell pkg-config --cflags libsystemd) -o $@ notifyd/ztnotifyd.c \
		$(shell pkg-config --libs libsystemd)

# the phone's own three: calls, messages, people (comm.h is what they share)
phone/ztphone: phone/ztphone.c comm/comm.h $(CORE_DEPS)
	$(APP_BUILD) -Icomm -o $@ phone/ztphone.c $(CORE) $(APP_LIBS)

messages/ztmessages: messages/ztmessages.c comm/comm.h $(CORE_DEPS)
	$(APP_BUILD) -Icomm -o $@ messages/ztmessages.c $(CORE) $(APP_LIBS)

contacts/ztcontacts: contacts/ztcontacts.c comm/comm.h $(CORE_DEPS)
	$(APP_BUILD) -Icomm -o $@ contacts/ztcontacts.c $(CORE) $(APP_LIBS)

# the App Store: the repos in pacman.conf and the AUR (see store/README.md)
STORE = store/ztstore.c store/util.c store/json.c store/pkg.c store/repo.c store/appstream.c store/aur.c \
	store/sections.c store/search.c store/jobs.c store/icons.c store/install.c
STORE_PKGS = librsvg-2.0 zlib
store/ztstore: $(STORE) $(wildcard store/*.h) $(CORE_DEPS)
	$(APP_BUILD) -DHAVE_RSVG $(shell pkg-config --cflags $(STORE_PKGS)) -o $@ $(STORE) $(CORE) $(APP_LIBS) \
		$(shell pkg-config --libs $(STORE_PKGS)) -lpthread

# Superscan, the web browser: WPE WebKit in the WP8.1 IE look (see
# browser/README.md). Needs wpewebkit on the phone.
BROWSER = browser/superscan.c browser/webview.c
BROWSER_PKGS = wpe-webkit-2.0 wpe-platform-2.0 wpe-platform-headless-2.0 glib-2.0 gobject-2.0 libdrm
browser/superscan: $(BROWSER) browser/webview.h $(CORE_DEPS)
	$(APP_BUILD) $(shell pkg-config --cflags $(BROWSER_PKGS)) -o $@ $(BROWSER) $(CORE) $(APP_LIBS) \
		$(shell pkg-config --libs $(BROWSER_PKGS))

# kept out of `all`/`install` until WPE is confirmed on the phone, so a missing
# package cannot break the main build
.PHONY: install-browser
install-browser: browser/superscan
	install -Dm755 browser/superscan /usr/local/bin/superscan
	install -Dm644 browser/superscan.desktop /usr/local/share/applications/superscan.desktop
	install -Dm644 browser/superscan.svg /usr/local/share/icons/hicolor/scalable/apps/superscan.svg
	rm -f /usr/local/bin/ztbrowser /usr/local/share/applications/ztbrowser.desktop \
		/usr/local/share/icons/hicolor/scalable/apps/ztbrowser.svg	# its old name

# the App Store's parsers against a made-up pacman layout (no root, no network)
.PHONY: check-store
check-store:
	sh store/tests/make_fixture.sh /tmp/ztstore-fixture
	cd store && for t in json repo appstream aur sections; do \
		$(CC) -std=c11 -Wall -o /tmp/ztstore-test-$$t tests/test_$$t.c util.c json.c pkg.c repo.c appstream.c aur.c sections.c -lz && \
		/tmp/ztstore-test-$$t || exit 1; done

# ztOSCoreSensorFramework: starts every piece of hardware support with the
# system, watches them, and reports what fails
csf/ztoscsf: csf/ztoscsf.c
	$(CC) $(CFLAGS) $(WARN) -o $@ csf/ztoscsf.c

splash/ztos-splash: splash/ztos-splash.c
	$(CC) $(CFLAGS) $(WARN) $(shell pkg-config --cflags cairo pangocairo) -o $@ splash/ztos-splash.c \
		$(shell pkg-config --libs cairo pangocairo)

core/xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG) $@

core/xdg-shell-protocol.c:
	wayland-scanner private-code $(XDG) $@

# wp_viewport: the list's surface shows a part of the page (see zrnmetrocore.c)
core/viewporter-client-protocol.h:
	wayland-scanner client-header $(VIEWPORTER) $@

core/viewporter-protocol.c:
	wayland-scanner private-code $(VIEWPORTER) $@

# A program built on another machine (a laptop's x86-64 build copied over with
# the sources) looks up to date to make and would be installed as it is, and
# then fails with "Exec format error". Refuse it: the built programs must be for
# this machine.
HOST_ELF := $(shell od -An -tx1 -j18 -N2 /bin/sh | tr -d ' \n')
check-arch: all
	@bad=0; for f in $$(find . -type f -perm -u+x ! -name '*.sh' ! -name '*.py' ! -path './.git/*'); do \
		[ "$$(od -An -tc -j1 -N3 $$f 2>/dev/null | tr -d ' ')" = ELF ] || continue; \
		m=$$(od -An -tx1 -j18 -N2 $$f | tr -d ' \n'); \
		if [ "$$m" != "$(HOST_ELF)" ]; then echo "$$f: built for another machine; run make clean"; bad=1; fi; \
	done; exit $$bad

install: check-arch
	install -Dm755 auth/ztos-polkit-agent /usr/local/bin/ztos-polkit-agent
	install -Dm755 sounds/ztos-sounds /usr/local/bin/ztos-sounds
	install -Dm755 ztmetro /usr/local/bin/ztmetro
	install -Dm755 ztmetro-session /usr/local/bin/ztmetro-session
	install -Dm755 splash/ztos-splash /usr/local/bin/ztos-splash
	install -Dm755 apps/ztapps /usr/local/bin/ztapps
	install -Dm755 settings/ztsettings /usr/local/bin/ztsettings
	install -Dm755 devtest/zrndevtest /usr/local/bin/zrndevtest
	install -Dm755 devices/ztdevices /usr/local/bin/ztdevices
	install -Dm644 devices/ztdevices.desktop /usr/local/share/applications/ztdevices.desktop
	install -Dm644 devices/ztdevices.svg /usr/local/share/icons/hicolor/scalable/apps/ztdevices.svg
	install -Dm755 store/ztstore /usr/local/bin/ztstore
	install -Dm644 store/data/sections /etc/ztstore/sections
	install -Dm644 store/data/ztstore.desktop /usr/local/share/applications/ztstore.desktop
	install -Dm644 store/data/ztstore.svg /usr/local/share/icons/hicolor/scalable/apps/ztstore.svg
	install -Dm755 perfd/zrnperfd /usr/local/sbin/zrnperfd
	install -Dm644 perfd/zrnperfd.service /etc/systemd/system/zrnperfd.service
	install -Dm755 sensord/ztoscsf-sensord /usr/local/sbin/ztoscsf-sensord
	install -Dm644 sensord/ztoscsf-sensord.service /etc/systemd/system/ztoscsf-sensord.service
	install -Dm755 modemd/ztoscorenetworkingd /usr/local/sbin/ztoscorenetworkingd
	install -Dm644 modemd/ztoscorenetworking-modem.service /etc/systemd/system/ztoscorenetworking-modem.service
	install -Dm644 modemd/ztoscorenetworking.service /etc/systemd/system/ztoscorenetworking.service
	install -Dm755 csf/ztoscsf /usr/local/sbin/ztoscsf
	install -Dm755 notifyd/ztnotifyd /usr/local/bin/ztnotifyd
	install -Dm755 phone/ztphone /usr/local/bin/ztphone
	install -Dm755 messages/ztmessages /usr/local/bin/ztmessages
	install -Dm755 contacts/ztcontacts /usr/local/bin/ztcontacts
	install -Dm644 -t /usr/local/share/applications comm/ztphone.desktop comm/ztmessages.desktop comm/ztcontacts.desktop
	install -Dm644 -t /usr/local/share/icons/hicolor/scalable/apps comm/ztphone.svg comm/ztmessages.svg comm/ztcontacts.svg
	install -Dm644 csf/components /etc/ztoscoresensorframework/components
	install -Dm644 csf/ztoscoresensorframework.service /etc/systemd/system/ztoscoresensorframework.service

clean:
	rm -f auth/ztos-polkit-agent sounds/ztos-sounds ztmetro apps/ztapps xdg-shell-protocol.h settings/ztsettings devtest/zrndevtest devices/ztdevices perfd/zrnperfd sensord/ztoscsf-sensord modemd/ztoscorenetworkingd csf/ztoscsf notifyd/ztnotifyd splash/ztos-splash \
		phone/ztphone messages/ztmessages contacts/ztcontacts store/ztstore core/viewporter-protocol.c core/viewporter-client-protocol.h \
		core/xdg-shell-client-protocol.h core/xdg-shell-protocol.c

# Uses a private Wayland seat; does not touch the running compositor or display.
.PHONY: check-arch check-keyboard
check-keyboard: xdg-shell-protocol.h
	$(CC) -O0 -g $(WARN) -DWLR_USE_UNSTABLE -I. $(shell pkg-config --cflags $(PKGS)) \
		-o /tmp/ztmetro-keyboard-test tests/keyboard-trackpad.c $(shell pkg-config --libs $(PKGS)) -lm
	/tmp/ztmetro-keyboard-test

.PHONY: check-launch
check-launch: xdg-shell-protocol.h
	$(CC) -O0 -g $(WARN) -DWLR_USE_UNSTABLE -I. $(shell pkg-config --cflags $(PKGS)) \
		-o /tmp/ztmetro-launch-test tests/launch-state.c $(shell pkg-config --libs $(PKGS)) -lm
	/tmp/ztmetro-launch-test

.PHONY: check-auth
check-auth: xdg-shell-protocol.h
	$(CC) -O0 -g $(WARN) -DWLR_USE_UNSTABLE -I. $(shell pkg-config --cflags $(PKGS)) \
		-o /tmp/ztmetro-auth-test tests/auth-prompt.c $(shell pkg-config --libs $(PKGS)) -lm
	/tmp/ztmetro-auth-test

# The native agent uses polkit's async callback API directly.
auth/ztos-polkit-agent: auth/ztos-polkit-agent.c
	$(CC) $(CFLAGS) $(WARN) $(shell pkg-config --cflags polkit-agent-1 gio-unix-2.0) -o $@ $< $(shell pkg-config --libs polkit-agent-1 gio-unix-2.0)
