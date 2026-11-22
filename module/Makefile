# pw-loudness-eq - Windows Loudness Equalization, per application, for PipeWire
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

MODULE := build/libpipewire-module-loudness-eq.so
PLUGIN := build/pw_loudness_eq.so

all: $(MODULE) $(PLUGIN) build/test-leq build/leq-wav

build:
	mkdir -p build

# PipeWire module: attaches the plugin to every application playback stream
$(MODULE): src/module-loudness-eq.c | build
	$(CC) $(CFLAGS) $(PW_CFLAGS) -shared -o $@ $< $(LDFLAGS) $(PW_LIBS)

# LADSPA plugin: the DSP, run by PipeWire's audioconvert inside each stream
$(PLUGIN): src/ladspa-leq.c src/ladspa-abi.h src/leq.c src/leq.h | build
	$(CC) $(CFLAGS) -shared -o $@ src/ladspa-leq.c src/leq.c $(LDFLAGS) -lm -lpthread

build/test-leq: tests/test-leq.c src/leq.c src/leq.h | build
	$(CC) $(CFLAGS) -o $@ tests/test-leq.c src/leq.c $(LDFLAGS) -lm

build/test-ladspa: tests/test-ladspa.c src/ladspa-abi.h src/leq.c src/leq.h $(PLUGIN) | build
	$(CC) $(CFLAGS) -o $@ tests/test-ladspa.c src/leq.c $(LDFLAGS) -ldl -lm

build/leq-wav: tools/leq-wav.c src/leq.c src/leq.h | build
	$(CC) $(CFLAGS) -o $@ tools/leq-wav.c src/leq.c $(LDFLAGS) -lm

check: build/test-leq build/test-ladspa
	./build/test-leq
	./build/test-ladspa $(PLUGIN)

install: $(MODULE) $(PLUGIN)
	install -Dm755 $(MODULE) $(DESTDIR)$(MODULEDIR)/libpipewire-module-loudness-eq.so
	install -Dm755 $(PLUGIN) $(DESTDIR)$(LADSPADIR)/pw_loudness_eq.so
	install -Dm644 conf/60-loudness-eq.conf $(DESTDIR)$(CONFDIR)/60-loudness-eq.conf
	@echo "Installed. Apply with: systemctl --user restart pipewire pipewire-pulse wireplumber"

uninstall:
	rm -f $(DESTDIR)$(MODULEDIR)/libpipewire-module-loudness-eq.so
	rm -f $(DESTDIR)$(LADSPADIR)/pw_loudness_eq.so
	rm -f $(DESTDIR)$(CONFDIR)/60-loudness-eq.conf
	@echo "Removed. Apply with: systemctl --user restart pipewire pipewire-pulse wireplumber"

clean:
	rm -rf build

.PHONY: all check install uninstall clean
