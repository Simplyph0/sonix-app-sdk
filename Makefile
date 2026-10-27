# Sonix Plus app SDK: the examples, built for the player.
#
#   make            builds dist/Hello and dist/FrontendTest, ready to copy to <card>/Apps/
#   make host-test  builds the frontend for this computer and runs it for 90 frames
#                   against the test core, writing the last frame to build-host/frame.raw
#
# The player's CPU is a 1.1 GHz MIPS32r2, so the programs are built static for it. The
# compiler is the MIPS toolchain that `make target` in the Sonix Plus repository builds;
# point CROSS at its prefix:
#
#   make CROSS=/path/to/sonix-plus/sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-

CROSS  ?= mipsel-rockbox-linux-gnu-
CC     := $(CROSS)gcc
CFLAGS := -O2 -march=mips32r2 -mno-madd4 -Wall -Wextra -Iinclude

.PHONY: all host-test clean

all: dist/Hello/hello dist/FrontendTest/frontend-test

dist/Hello/hello: examples/hello/hello.c include/sonixapp.h
	mkdir -p dist/Hello
	$(CC) $(CFLAGS) -static -o $@ examples/hello/hello.c -lm
	cp examples/hello/manifest.ini examples/hello/icon.png dist/Hello/

# The frontend with the test core: an "emulator" that plays no game. It is the quickest way to
# see that the screen, the touch panel and the sound all work on a player.
dist/FrontendTest/frontend-test: frontend/sonix_libretro.c frontend/testcore/testcore.c include/sonixapp.h
	mkdir -p dist/FrontendTest
	$(CC) $(CFLAGS) -static -Ifrontend -DSONIX_SYSTEM='"FrontendTest"' -DSONIX_LAYOUT=LAYOUT_SNES \
		-o $@ frontend/sonix_libretro.c frontend/testcore/testcore.c -lm
	cp examples/frontend-test/manifest.ini examples/frontend-test/icon.png dist/FrontendTest/

host-test:
	mkdir -p build-host
	gcc -O2 -Wall -Iinclude -Ifrontend -DSONIX_SYSTEM='"Test"' -o build-host/frontend-test \
		frontend/sonix_libretro.c frontend/testcore/testcore.c -lm
	echo test > build-host/game.test
	SONIX_SD=build-host SONIX_APP_DIR=build-host SONIX_FB=build-host/frame.raw SONIX_TEST_FRAMES=90 \
		build-host/frontend-test build-host/game.test

clean:
	rm -rf dist build-host
