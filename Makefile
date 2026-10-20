INFOIDE ?= ./infoide
.DEFAULT_GOAL := all
CXX ?= g++
PREFIX ?= /usr/local
DESTDIR ?=
HOST_FLAGS := -std=c++20 -O2 -Wall -Wextra -Werror -Iinclude
HOST_SOURCES := $(wildcard src/common/*.cc src/linux/*.cc)
.PHONY: all package host check install uninstall

host: build/linux/inpkg

build/linux/inpkg: $(HOST_SOURCES) src/common/package.h include/inpkg/gzip.h Makefile
	@mkdir -p "$(@D)"
	@set -eu; temporary="$@.tmp.$$$$"; trap 'rm -f "$$temporary"' EXIT; \
		$(CXX) $(HOST_FLAGS) $(HOST_SOURCES) -o "$$temporary"; mv "$$temporary" "$@"

check: build/linux/inpkg
	sh tests/packages.sh "$(CURDIR)/build/linux/inpkg"

install: host
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m755 build/linux/inpkg "$(DESTDIR)$(PREFIX)/bin/inpkg"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/inpkg"

all: host
	"$(INFOIDE)" pkg $(if $(OSINFO_SDK),--sdk "$(OSINFO_SDK)") --project .
