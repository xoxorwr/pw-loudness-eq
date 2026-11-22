# pw-loudness-eq app - control and monitor window for the module (GTK3)
PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
DESTDIR ?=

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter
PKGS    := gtk+-3.0 libpipewire-0.3
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=build/%.o)
BIN  := build/pw-loudness-eq
DESKTOP := io.github.pw_loudness_eq.desktop

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(PKG_LIBS) -lm

build/%.o: src/%.c src/model.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

check:

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/pw-loudness-eq
	install -Dm644 $(DESKTOP) $(DESTDIR)$(DATADIR)/applications/$(DESKTOP)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/pw-loudness-eq
	rm -f $(DESTDIR)$(DATADIR)/applications/$(DESKTOP)

clean:
	rm -rf build

.PHONY: all check install uninstall clean
