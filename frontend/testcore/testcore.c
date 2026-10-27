/*
 * A libretro core that is not an emulator: it draws a moving picture, shows which
 * buttons are held, plays a tone, and keeps a counter in its battery save and in
 * its save state. It is what the frontend is tested with, off the player and on
 * it, without any game.
 *
 * MIT licence.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../libretro.h"

#define W 256
#define H 224
#define DEFAULT_RATE 44100
#define FPS 60

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static uint16_t frame[W * H];
static uint32_t frames;
static float phase;
static uint8_t save_ram[64];

/* TESTCORE_RATE=32768 makes the core run its sound at another rate, as the real ones do */
static int rate_now(void) {
	const char *r = getenv("TESTCORE_RATE");
	return r && atoi(r) > 8000 ? atoi(r) : DEFAULT_RATE;
}

void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void) {}
void retro_deinit(void) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_controller_port_device(unsigned port, unsigned device) {
	(void)port;
	(void)device;
}

void retro_get_system_info(struct retro_system_info *info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "Sonix test core";
	info->library_version = "1";
	info->valid_extensions = "bin|test";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = W;
	info->geometry.base_height = H;
	info->geometry.max_width = W;
	info->geometry.max_height = H;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = FPS;
	info->timing.sample_rate = rate_now();
}

bool retro_load_game(const struct retro_game_info *game) {
	(void)game;
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
		return false;
	}
	frames = 0;
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
	(void)type;
	(void)info;
	(void)num;
	return false;
}

void retro_unload_game(void) {}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void retro_reset(void) { frames = 0; }

static uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

void retro_run(void) {
	input_poll_cb();
	uint32_t held = 0;
	for (unsigned id = 0; id < 16; id++) {
		if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id)) {
			held |= 1u << id;
		}
	}

	/* a gradient that drifts, so that every frame differs */
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			frame[y * W + x] = rgb((x + frames) & 255, y, ((x ^ y) + frames / 2) & 255);
		}
	}
	/* a box that bounces */
	int bx = (int)(frames * 3 % (2 * (W - 32)));
	if (bx >= W - 32) {
		bx = 2 * (W - 32) - bx;
	}
	for (int y = 100; y < 132; y++) {
		for (int x = bx; x < bx + 32; x++) {
			frame[y * W + x] = rgb(255, 255, 255);
		}
	}
	/* sixteen squares along the top: lit for each button held */
	for (int i = 0; i < 16; i++) {
		for (int y = 4; y < 16; y++) {
			for (int x = 4 + i * 15; x < 4 + i * 15 + 12; x++) {
				frame[y * W + x] = (held >> i) & 1 ? rgb(255, 255, 0) : rgb(20, 20, 20);
			}
		}
	}
	/* a bar along the bottom that grows with the frame count */
	for (int x = 0; x < (int)(frames % W); x++) {
		frame[(H - 4) * W + x] = rgb(255, 255, 255);
		frame[(H - 3) * W + x] = rgb(255, 255, 255);
	}
	video_cb(frame, W, H, W * sizeof(uint16_t));

	/* a tone while A or B is held: 440 Hz for A, 660 Hz for B */
	int RATE = rate_now();
	int16_t sound[(48000 / FPS + 1) * 2];
	int n = RATE / FPS;
	float freq = (held & (1u << RETRO_DEVICE_ID_JOYPAD_A)) ? 440.0f : (held & (1u << RETRO_DEVICE_ID_JOYPAD_B)) ? 660.0f : 0.0f;
	for (int i = 0; i < n; i++) {
		float s = freq > 0 ? sinf(phase) * 5000.0f : 0.0f;
		phase += 2.0f * 3.14159265f * freq / RATE;
		if (phase > 6.2831853f) {
			phase -= 6.2831853f;
		}
		sound[i * 2] = sound[i * 2 + 1] = (int16_t)s;
	}
	audio_batch_cb(sound, (size_t)n);

	frames++;
	if (frames % FPS == 0) {
		save_ram[0]++; /* the battery save changes once a second */
	}
}

size_t retro_serialize_size(void) { return sizeof(frames); }

bool retro_serialize(void *data, size_t size) {
	if (size < sizeof(frames)) {
		return false;
	}
	memcpy(data, &frames, sizeof(frames));
	return true;
}

bool retro_unserialize(const void *data, size_t size) {
	if (size < sizeof(frames)) {
		return false;
	}
	memcpy(&frames, data, sizeof(frames));
	return true;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) {
	(void)index;
	(void)enabled;
	(void)code;
}

void *retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? save_ram : NULL; }
size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof(save_ram) : 0; }
