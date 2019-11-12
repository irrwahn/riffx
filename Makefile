CFLAGS = -O2 -Wall -Wextra -Werror

.PHONY: all clean

all: ww2ogg-b revorb-b riffx-b

riffx-b: riffx
	strip riffx

riffx: riffx.c

ww2ogg-b:
	cd ww2ogg && $(MAKE) all

revorb-b:
	cd revorb-nix && ./build.sh

clean:
	rm -f *.o riffx
	cd ww2ogg && $(MAKE) clean
	cd revorb-nix && rm revorb 2>/dev/null ||:
