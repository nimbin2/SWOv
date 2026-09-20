# swov — Makefile
#
#   make            build                                   -> ./swov
#   make debug      build with the address and UB sanitizers -> ./swov-debug
#   make strict     build with -Wall -Wextra
#   make run        build and run
#   make install    install to $(PREFIX)/bin           (default: /usr)
#                   examples go to $(PREFIX)/share/swov
#   make config     copy config.example to ~/.config/swov/config
#   make link       symlink ~/.config/swov/config at the installed example
#   make uninstall
#   make clean
#
# Requires: a C compiler, pkg-config, and the SDL3, SDL3_image and SDL3_ttf
# dev files. On Debian trixie and newer:
#   sudo apt install build-essential pkg-config \
#        libsdl3-dev libsdl3-image-dev libsdl3-ttf-dev

CC          ?= cc
PKG_CONFIG  ?= pkg-config
CFLAGS      ?= -std=c11 -O2
PREFIX      ?= /usr
bindir      ?= $(PREFIX)/bin
datadir     ?= $(PREFIX)/share/swov
shdatadir   ?= $(PREFIX)/share/sw

BUILD       := $(shell md5sum swov.c 2>/dev/null | cut -c1-8)
SRC         := swov.c
HDR         := sw_theme.h
BIN         := swov
PKGS        := sdl3 sdl3-image sdl3-ttf

SDL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags $(PKGS))
SDL_LIBS    := $(shell $(PKG_CONFIG) --libs $(PKGS))

.PHONY: all debug strict run install config link uninstall clean

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DSWOV_BUILD='"$(BUILD)"' $(SDL_CFLAGS) $(SRC) -o $(BIN) $(SDL_LIBS) -lm
	@./$(BIN) --version

debug: $(SRC) $(HDR)
	$(CC) -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	  -DSWOV_BUILD='"$(BUILD)-dbg"' $(SDL_CFLAGS) $(SRC) -o $(BIN)-debug $(SDL_LIBS) -lm

strict: $(SRC) $(HDR)
	$(CC) -std=c11 -O2 -Wall -Wextra -DSWOV_BUILD='"$(BUILD)"' \
	  $(SDL_CFLAGS) $(SRC) -o $(BIN) $(SDL_LIBS) -lm

run: all
	./$(BIN)

install: all
	install -Dm755 $(BIN) $(DESTDIR)$(bindir)/$(BIN)
	install -Dm644 config.example $(DESTDIR)$(datadir)/config.example
	install -Dm644 sw_config.example $(DESTDIR)$(shdatadir)/config.example
	@echo "installed to $(DESTDIR)$(bindir)/$(BIN)"
	@echo "examples     $(DESTDIR)$(datadir)/"
	@./$(BIN) --version

config:
	@mkdir -p $(HOME)/.config/swov
	@if [ -e $(HOME)/.config/swov/config ]; then \
	  echo "$(HOME)/.config/swov/config exists, leaving it alone"; \
	else \
	  cp config.example $(HOME)/.config/swov/config; \
	  echo "wrote $(HOME)/.config/swov/config"; \
	fi

# The shipped example, live: every install updates what you are running.
link:
	@mkdir -p $(HOME)/.config/swov
	ln -sfn $(datadir)/config.example $(HOME)/.config/swov/config
	@echo "$(HOME)/.config/swov/config -> $(datadir)/config.example"

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(BIN)
	rm -f $(DESTDIR)$(datadir)/config.example
	rm -f $(DESTDIR)$(shdatadir)/config.example
	-rmdir $(DESTDIR)$(datadir) $(DESTDIR)$(shdatadir) 2>/dev/null || true

clean:
	rm -f $(BIN) $(BIN)-debug
