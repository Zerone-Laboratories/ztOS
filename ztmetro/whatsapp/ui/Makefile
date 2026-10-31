# ztwhatsapp: the app's UI. Run from here, or through ../Makefile.
ROOT = ../..
PKGS = xkbcommon wayland-client wayland-egl egl glesv2 cairo pangocairo gdk-pixbuf-2.0 gio-2.0 json-c
CFLAGS ?= -O2 -g
CXXFLAGS ?= -O2 -g
IAL_FLAGS := $(shell pkg-config --exists ztos-ial 2>/dev/null && echo -DHAVE_ZTOS_IAL $$(pkg-config --cflags ztos-ial))
IAL_LIBS := $(shell pkg-config --exists ztos-ial 2>/dev/null && pkg-config --libs --static ztos-ial)
INC = -I$(ROOT)/core -I$(ROOT) $(shell pkg-config --cflags $(PKGS)) $(IAL_FLAGS)
CORE = core/zrnmetrocore.c core/zrnmetrogpu.c core/xdg-shell-protocol.c core/viewporter-protocol.c \
	retro/dotmatrix.c retro/dm-chrome.c retro/dm-wrap.c
OBJ = $(addprefix .obj/,$(notdir $(CORE:.c=.o)))
WRAP = -Wl,--wrap=cairo_fill,--wrap=cairo_fill_preserve,--wrap=cairo_stroke,--wrap=cairo_stroke_preserve,--wrap=cairo_paint

ztwhatsapp: ztwhatsapp.cpp qr.h $(OBJ)
	$(CXX) $(CXXFLAGS) -std=c++20 -Wall -Wno-unused-parameter $(INC) -c -o .obj/ztwhatsapp.o ztwhatsapp.cpp
	$(CXX) -o $@ .obj/ztwhatsapp.o $(OBJ) $(WRAP) $(shell pkg-config --libs $(PKGS)) $(IAL_LIBS) -lm -lpthread

.obj/%.o: $(ROOT)/core/%.c
	@mkdir -p .obj
	$(CC) $(CFLAGS) -std=c11 -Wno-unused-parameter $(INC) -c -o $@ $<
.obj/%.o: $(ROOT)/retro/%.c
	@mkdir -p .obj
	$(CC) $(CFLAGS) -std=c11 -Wno-unused-parameter $(INC) -c -o $@ $<

install: ztwhatsapp
	install -Dm755 ztwhatsapp $(DESTDIR)/usr/local/bin/ztwhatsapp
	install -Dm644 ztwhatsapp.desktop $(DESTDIR)/usr/local/share/applications/ztwhatsapp.desktop
	install -Dm644 ztwhatsapp.svg $(DESTDIR)/usr/local/share/icons/hicolor/scalable/apps/ztwhatsapp.svg

clean:
	rm -rf .obj ztwhatsapp

.PHONY: install clean
