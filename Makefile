CC ?= gcc
CCX ?= g++
CFLAGS = -O2 -Wall -Wextra -Werror

.PHONY: all clean

all: ww2ogg/ww2ogg revorb-nix/revorb riffx unriffle

riffx: riffx.c
	$(CC) $(CFLAGS) -o riffx riffx.c
	strip riffx

unriffle: unriffle.c
	$(CC) -std=c99 -Wpedantic $(CFLAGS) -o unriffle unriffle.c
	strip unriffle

ww2ogg/ww2ogg:
	cd ww2ogg && $(MAKE) all

revorb-nix/revorb: revorb-nix/revorb.cpp
	$(CCX) revorb-nix/revorb.cpp -o revorb-nix/revorb -logg -lvorbis

clean:
	rm -f *.o riffx unriffle
	rm revorb-nix/revorb 2>/dev/null ||:
	cd ww2ogg && $(MAKE) clean
