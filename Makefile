CC      ?= gcc
CFLAGS  ?= -O3 -mcpu=native -flto -Wall -Wextra
LDFLAGS ?= -flto -s
PREFIX  ?= $(HOME)/.local

clean-cam: clean-cam.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lm

install: clean-cam
	install -Dm755 clean-cam $(PREFIX)/bin/clean-cam

clean:
	rm -f clean-cam

.PHONY: install clean
