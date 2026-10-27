CC ?= cc
ARCH ?= $(shell uname -m)
HOST_ARCH := $(shell uname -m)
OUT = prebuilt/$(ARCH)/libztos-render.so
SHARED = ../../ztmetro/core/zrnmetrorender.h

CFLAGS ?= $(shell pkg-config --cflags glesv2 2>/dev/null)
ifeq ($(ARCH),$(HOST_ARCH))
  LIBS ?= $(shell pkg-config --libs glesv2 2>/dev/null || echo "-lGLESv2")
else
  ifeq ($(wildcard /usr/include/GLES2/gl2.h),/usr/include/GLES2/gl2.h)
    CFLAGS += -I/usr/include
  endif
  LIBS ?=
endif

all: $(OUT)
$(OUT): renderer.c $(SHARED)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared -I../../ztmetro/core $(CFLAGS) -o $@ renderer.c $(LIBS) -lm
	cat renderer.c $(SHARED) | sha256sum | cut -d " " -f1 > $(OUT:.so=.sha256)
clean:
	rm -rf prebuilt
.PHONY: all clean
