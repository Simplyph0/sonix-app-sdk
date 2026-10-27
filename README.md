# Sonix Plus app SDK

What you need to write an app for [Sonix Plus](https://github.com/Simplyph0/sonix-plus), the
firmware for the HiBy R1 and R3 Pro II: a single header for the screen, the touch panel and
sound, a libretro frontend, and two examples.

An app is a program on the microSD card. The player lists it in **More**, **Games** or
**Emulators**, hands it the screen and the touch panel when it is tapped, and takes them back when
it exits. The format of the app's folder, and everything the player gives it, is in
[APPS.md](https://github.com/Simplyph0/sonix-plus/blob/main/APPS.md).

## `include/sonixapp.h`

One header, MIT licence, nothing to link against but the C library:

```c
#define SONIXAPP_IMPLEMENTATION      /* in exactly one .c file */
#include "sonixapp.h"

sonix_env_t env;
sonix_env(&env);                                  /* where am I, how big is the panel */
sonix_video_t *video = sonix_video_open(&env);    /* the framebuffer, double-buffered */
uint16_t *frame = sonix_video_frame(video);       /* 480 x 720 (or 800) RGB565, draw here */
sonix_video_present(video);                       /* show it, no tearing */

sonix_touch_t *touch = sonix_touch_open(&env);    /* up to five fingers, rotation applied */
sonix_touches_t fingers;
sonix_touch_read(touch, &fingers);                /* never blocks */

sonix_audio_t *audio = sonix_audio_open(&env, 44100, 2);
sonix_audio_write(audio, samples, frames);        /* blocks: the sound card sets your pace */
```

It handles what the two players do differently: the R3 Pro II's and R1's touch drivers speak
different protocols, the picture may be turned 180 degrees in Settings, and the framebuffer has
two pages. There are also a few drawing helpers and a built-in 5x7 text face, for an app that
draws its own controls.

Try it off the player: if `SONIX_FB` names an ordinary file, frames are written into it as raw
RGB565 instead of to a framebuffer. See `examples/hello/hello.c`, which runs for a few frames
with `--frames 3`.

## `frontend/`: the libretro frontend

`sonix_libretro.c` plays any [libretro](https://www.libretro.com/) core that is linked into it:
the game on the top of the screen, an on-screen pad below (D-pad, A B, shoulders where the system
has them, Select and Start, and SAVE, LOAD and EXIT keys), sound through the player, battery saves
in `<card>/Saves/<system>/` and one save state per game. An emulator app is this file, a core and
two defines:

```
-DSONIX_SYSTEM='"NES"'  -DSONIX_LAYOUT=LAYOUT_NES      (or LAYOUT_GBA, LAYOUT_SNES)
```

`testcore/` is a core that is not an emulator. It draws a moving picture, lights a square for each
button held, plays a tone for A and B and keeps a counter in its battery save and its save state.
It is how the frontend is tested.

## Examples

```
make CROSS=/path/to/mipsel-rockbox-linux-gnu-
```

builds `dist/Hello` and `dist/FrontendTest`. Copy either folder to `Apps/` on the card.
`make host-test` runs the frontend on your computer. The compiler is the MIPS toolchain that
`make target` builds in the Sonix Plus repository; a program has to be 32-bit little-endian MIPS,
and linking it statically means it does not depend on what the player has installed.

## Licences

The SDK is MIT. `frontend/libretro.h` is the libretro API header, also MIT, from
[libretro-common](https://github.com/libretro/libretro-common). A core you link in keeps its
own licence, and so does the program that results.
