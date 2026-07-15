CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11

FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3)
FUSE_LIBS := $(shell $(PKG_CONFIG) --libs fuse3)

.PHONY: all clean

all: wifiuart-tty

wifiuart-tty: wifiuart-tty.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUSE_CFLAGS) $< $(FUSE_LIBS) -o $@

clean:
	rm -f wifiuart-tty
