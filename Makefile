# swov — build and install
#
#   make                 build ./swov
#   make install         into ~/.local/bin (override with PREFIX=/usr/local)
#   make config          drop config.example into ~/.config/swov/config
#   make debug           build ./swov-debug with the sanitizers on
#   make clean uninstall

PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
CONFDIR ?= $(if $(XDG_CONFIG_HOME),$(XDG_CONFIG_HOME),$(HOME)/.config)/swov

PKGS       := sdl3 sdl3-image sdl3-ttf
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS) 2>/dev/null)

CC       ?= cc
CFLAGS   ?= -O2
CFLAGS   += -std=c11 -Wall -Wextra $(PKG_CFLAGS)
LDLIBS   += $(PKG_LIBS)

SAN := -fsanitize=address,undefined

all: swov

swov: swov.c | check
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

swov-debug: swov.c | check
	$(CC) -std=c11 -g -O1 -Wall -Wextra $(SAN) $(PKG_CFLAGS) -o $@ $< $(PKG_LIBS) $(SAN)

debug: swov-debug

# a missing SDL3 is the one failure worth explaining
check:
	@pkg-config --exists $(PKGS) || { \
	  echo "swov: SDL3 development files not found."; \
	  echo "  Debian trixie or newer:"; \
	  echo "    sudo apt install build-essential pkg-config \\"; \
	  echo "         libsdl3-dev libsdl3-image-dev libsdl3-ttf-dev"; \
	  echo "  Older releases: build SDL, SDL_image and SDL_ttf 3.2+ from source"; \
	  echo "  and point PKG_CONFIG_PATH at your install prefix."; \
	  exit 1; }

install: swov
	install -Dm755 swov $(DESTDIR)$(BINDIR)/swov
	@echo "installed $(DESTDIR)$(BINDIR)/swov"

# never overwrites an existing config
config:
	@if [ -e "$(CONFDIR)/config" ]; then \
	  echo "$(CONFDIR)/config exists, leaving it alone"; \
	else \
	  install -Dm644 config.example "$(CONFDIR)/config" && \
	  echo "wrote $(CONFDIR)/config"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/swov

clean:
	rm -f swov swov-debug

.PHONY: all debug check install config uninstall clean
