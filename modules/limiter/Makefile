# audio-limiter - the Windows audio engine's output limiter on every PipeWire playback device
PREFIX     ?= /usr
LIBDIR     ?= $(PREFIX)/lib
DATADIR    ?= $(PREFIX)/share
MODULEDIR  ?= $(shell pkg-config --variable=moduledir libpipewire-0.3 2>/dev/null || echo $(LIBDIR)/pipewire-0.3)
LADSPADIR  ?= $(LIBDIR)/ladspa
CONFDIR    ?= $(DATADIR)/pipewire/pipewire.conf.d
DESTDIR    ?=

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -fPIC -fvisibility=hidden
PW_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3)
PW_LIBS   := $(shell pkg-config --libs libpipewire-0.3)

MODULE := build/libpipewire-module-audio-limiter.so
PLUGIN := build/pw_audio_limiter.so
LOUDNESS := ../loudness/src

all: $(MODULE) $(PLUGIN)

build:
	mkdir -p build

# PipeWire module: attaches the plugin to every playback device
$(MODULE): src/module-audio-limiter.c | build
	$(CC) $(CFLAGS) $(PW_CFLAGS) -shared -o $@ $< $(LDFLAGS) $(PW_LIBS) -lm

# LADSPA plugin: the DSP, run by the device's audioconvert
$(PLUGIN): src/ladspa-limiter.c src/ladspa-abi.h src/limiter.c src/limiter.h | build
	$(CC) $(CFLAGS) -shared -o $@ src/ladspa-limiter.c src/limiter.c $(LDFLAGS) -lm -lpthread

# also checks it against Loudness Equalization's output (whistles)
build/test-limiter: tests/test-limiter.c src/limiter.c src/limiter.h $(LOUDNESS)/leq.c $(LOUDNESS)/leq.h | build
	$(CC) $(CFLAGS) -o $@ tests/test-limiter.c src/limiter.c $(LOUDNESS)/leq.c $(LDFLAGS) -lm

build/test-ladspa: tests/test-ladspa.c src/ladspa-abi.h src/limiter.c src/limiter.h $(PLUGIN) | build
	$(CC) $(CFLAGS) -o $@ tests/test-ladspa.c src/limiter.c $(LDFLAGS) -ldl -lm

check: build/test-limiter build/test-ladspa
	./build/test-limiter
	./build/test-ladspa $(PLUGIN)

install: $(MODULE) $(PLUGIN)
	install -Dm755 $(MODULE) $(DESTDIR)$(MODULEDIR)/libpipewire-module-audio-limiter.so
	install -Dm755 $(PLUGIN) $(DESTDIR)$(LADSPADIR)/pw_audio_limiter.so
	install -Dm644 conf/61-audio-limiter.conf $(DESTDIR)$(CONFDIR)/61-audio-limiter.conf
	@echo "Installed. Apply with: systemctl --user restart pipewire pipewire-pulse wireplumber"

uninstall:
	rm -f $(DESTDIR)$(MODULEDIR)/libpipewire-module-audio-limiter.so
	rm -f $(DESTDIR)$(LADSPADIR)/pw_audio_limiter.so
	rm -f $(DESTDIR)$(CONFDIR)/61-audio-limiter.conf
	@echo "Removed. Apply with: systemctl --user restart pipewire pipewire-pulse wireplumber"

clean:
	rm -rf build

.PHONY: all check install uninstall clean
