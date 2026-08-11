.POSIX:

CC      = cc
CFLAGS  = -std=c99 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -isystem vendor
LDLIBS  = -lX11
BINDIR  = $(HOME)/.local/bin

all: htray

htray: htray.c vendor/stb_ds.h
	$(CC) $(CFLAGS) -o $@ htray.c $(LDLIBS)

install: htray
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/htray" $(BINDIR)/htray

uninstall:
	rm -f $(BINDIR)/htray

clean:
	rm -f htray

.PHONY: all install uninstall clean
