# SPDX-License-Identifier: GPL-2.0-only
CC       ?= cc
PKGS     := fdisk blkid ext2fs com_err
CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64 $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(shell pkg-config --libs $(PKGS))
PREFIX   ?= /usr/local

SRC  := $(wildcard src/*.c)
OBJ  := $(SRC:src/%.c=build/%.o)
BIN  := copydisk

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c src/copydisk.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

test: $(BIN)
	./tests/run.sh

install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -rf build $(BIN)

.PHONY: all test install clean
