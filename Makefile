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
# Requires: a C compiler and the libwayland-client dev files. Nothing else —
# fonts, icons and SVGs are decoded in-tree, and the window is a Wayland
# layer-shell surface drawn by swov itself. On Debian and friends:
#   sudo apt install build-essential pkg-config libwayland-dev
#
# Written/modified by: Claude Opus 4.1, Claude Sonnet 4.5, Claude Opus 5

CC          ?= cc
PKG_CONFIG  ?= pkg-config
CFLAGS      ?= -std=c11 -O2
PREFIX      ?= /usr
bindir      ?= $(PREFIX)/bin
datadir     ?= $(PREFIX)/share/swov
shdatadir   ?= $(PREFIX)/share/sw

BUILD       := $(shell md5sum swov.c 2>/dev/null | cut -c1-8)
SRC         := swov.c
HDR         := sw_theme.h nanosvg.h nanosvgrast.h stb_image.h stb_truetype.h \
               sw_draw.h sw_wl.h sw_keymap.h
BIN         := swov

WL_CFLAGS   := $(shell $(PKG_CONFIG) --cflags wayland-client 2>/dev/null)
WL_LIBS     := $(shell $(PKG_CONFIG) --libs wayland-client 2>/dev/null || echo -lwayland-client)

.PHONY: all debug strict run install config link uninstall clean

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DSWOV_BUILD='"$(BUILD)"' $(WL_CFLAGS) $(SRC) -o $(BIN) $(WL_LIBS) -lm
	@./$(BIN) --version

debug: $(SRC) $(HDR)
	$(CC) -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	  -DSWOV_BUILD='"$(BUILD)-dbg"' $(WL_CFLAGS) $(SRC) -o $(BIN)-debug $(WL_LIBS) -lm

strict: $(SRC) $(HDR)
	$(CC) -std=c11 -O2 -Wall -Wextra -DSWOV_BUILD='"$(BUILD)"' \
	  $(WL_CFLAGS) $(SRC) -o $(BIN) $(WL_LIBS) -lm

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
