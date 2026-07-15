CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11

FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3)
FUSE_LIBS := $(shell $(PKG_CONFIG) --libs fuse3)

.PHONY: all clean test

all: wifiuart-tty

wifiuart-tty: wifiuart-tty.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUSE_CFLAGS) $< $(FUSE_LIBS) -o $@

wifiuart-tty-test: wifiuart-tty.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function -Wno-unused-const-variable \
		-DWIFIUART_TTY_TEST $(FUSE_CFLAGS) $< $(FUSE_LIBS) -o $@

test: wifiuart-tty-test
	./wifiuart-tty-test

clean:
	rm -f wifiuart-tty wifiuart-tty-test
