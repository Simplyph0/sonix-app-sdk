/*
 * sonix_libretro - a libretro frontend for Sonix Plus apps.
 *
 * It plays whatever libretro core is linked into it: the game on the top of the
 * screen, an on-screen pad below, sound through the player. One emulator app is
 * this file, a core, and a layout chosen at build time:
 *
 *     -DSONIX_SYSTEM="NES"        the name, and the folder under Saves/ on the card
 *     -DSONIX_LAYOUT=LAYOUT_NES   which pad to draw (LAYOUT_NES, LAYOUT_GBA, LAYOUT_SNES)
 *
 * The player starts it with the file to open as its argument. Battery saves are
 * written to <card>/Saves/<system>/<game>.srm, save states to <game>.st0, and
 * core options may be overridden in core.opt in the app's folder (key = value).
 *
 * MIT licence, like the rest of the SDK. The cores it is linked with have their
 * own.
 */

#define SONIXAPP_IMPLEMENTATION
#include "sonixapp.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/mman.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libretro.h"

#ifndef SONIX_SYSTEM
#define SONIX_SYSTEM "Game"
#endif

#define LAYOUT_NES 0
#define LAYOUT_GBA 1
#define LAYOUT_SNES 2
#ifndef SONIX_LAYOUT
#define SONIX_LAYOUT LAYOUT_NES
#endif

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

static sonix_env_t env;
static sonix_video_t *video;
static sonix_touch_t *touch;
static sonix_audio_t *audio;
static uint16_t *screen; /* the panel's frame */
static int panel_w;
static int panel_h;

static char save_dir[600];
static char system_dir[600];
static char rom_base[300]; /* the game's file name, without folder or extension */

static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static struct retro_game_geometry geometry;
static double core_fps = 60.0;
static double core_rate = 44100.0;

/* The sound always goes to the player at this rate. Cores run at whatever their machine did
 * (48000 for the NES, 32768 for the GBA, 32040 for the SNES), and not every rate opens on the
 * output; 44100 is the one that does everywhere. */
#define OUT_RATE 44100

/* the last frame the core gave */
static const void *frame_data;
static unsigned frame_w;
static unsigned frame_h;
static size_t frame_pitch;

/* sound gathered during one retro_run */
static int16_t *audio_buf;
static size_t audio_frames;
static size_t audio_capacity;

static uint32_t pad_mask; /* RETRO_DEVICE_ID_JOYPAD_* bits held down */
static volatile sig_atomic_t quit_requested;

/* For testing off the player: SONIX_TEST_FRAMES=N stops after N frames, and
 * SONIX_TEST_TOUCH="x,y x,y" holds fingers down at those places. */
static long test_frames;
static sonix_touches_t test_touch;
static bool have_test_touch;

/* ---------------------------------------------------------------------------
 * Core options
 * ------------------------------------------------------------------------- */

#define MAX_OPTIONS 128

typedef struct {
	char key[64];
	char value[64];
} option_t;

static option_t options[MAX_OPTIONS];
static int option_count;
static bool options_dirty;

static void option_set(const char *key, const char *value) {
	for (int i = 0; i < option_count; i++) {
		if (!strcmp(options[i].key, key)) {
			snprintf(options[i].value, sizeof(options[i].value), "%s", value);
			return;
		}
	}
	if (option_count < MAX_OPTIONS) {
		snprintf(options[option_count].key, sizeof(options[0].key), "%s", key);
		snprintf(options[option_count].value, sizeof(options[0].value), "%s", value);
		option_count++;
	}
}

static const char *option_get(const char *key) {
	for (int i = 0; i < option_count; i++) {
		if (!strcmp(options[i].key, key)) {
			return options[i].value;
		}
	}
	return NULL;
}

/* SET_VARIABLES: "Label; first|second|third" - the first is the default. */
static void options_from_variables(const struct retro_variable *vars) {
	for (; vars && vars->key; vars++) {
		if (option_get(vars->key) || !vars->value) {
			continue;
		}
		const char *semi = strchr(vars->value, ';');
		if (!semi) {
			continue;
		}
		const char *p = semi + 1;
		while (*p == ' ') {
			p++;
		}
		char value[64];
		size_t n = strcspn(p, "|");
		if (n >= sizeof(value)) {
			n = sizeof(value) - 1;
		}
		memcpy(value, p, n);
		value[n] = '\0';
		option_set(vars->key, value);
	}
}

/* core.opt in the app's folder overrides what the core would default to. */
static void options_load_overrides(void) {
	FILE *f = fopen("core.opt", "r");
	if (!f) {
		return;
	}
	char line[200];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == ';') {
			continue;
		}
		*eq = '\0';
		char *key = line;
		char *value = eq + 1;
		while (*key == ' ') {
			key++;
		}
		for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); e--) {
			e[-1] = '\0';
		}
		while (*value == ' ') {
			value++;
		}
		for (char *e = value + strlen(value); e > value && isspace((unsigned char)e[-1]); e--) {
			e[-1] = '\0';
		}
		if (key[0] && value[0]) {
			option_set(key, value);
			options_dirty = true; /* tell the core when it asks */
		}
	}
	fclose(f);
}

/* ---------------------------------------------------------------------------
 * libretro callbacks
 * ------------------------------------------------------------------------- */

static void log_cb(enum retro_log_level level, const char *fmt, ...) {
	if (level < RETRO_LOG_INFO) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void make_dir_path(const char *path) {
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static bool environment_cb(unsigned cmd, void *data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format fmt = *(const enum retro_pixel_format *)data;
		if (fmt == RETRO_PIXEL_FORMAT_RGB565 || fmt == RETRO_PIXEL_FORMAT_XRGB8888 ||
			fmt == RETRO_PIXEL_FORMAT_0RGB1555) {
			pixel_format = fmt;
			return true;
		}
		return false;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = system_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = save_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_cb;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 0; /* the older SET_VARIABLES way is enough */
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		options_from_variables((const struct retro_variable *)data);
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *var = data;
		const char *value = var->key ? option_get(var->key) : NULL;
		var->value = value;
		return value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = options_dirty;
		options_dirty = false;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		geometry = *(const struct retro_game_geometry *)data;
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
		const struct retro_system_av_info *info = data;
		geometry = info->geometry;
		core_fps = info->timing.fps;
		core_rate = info->timing.sample_rate;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_ROTATION:
	case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
		return true;
	default: {
		/* said once each, so that a core that needs something is found in the log */
		static unsigned said[16];
		static int said_count;
		unsigned id = cmd & 0xFFFF;
		for (int i = 0; i < said_count; i++) {
			if (said[i] == id) {
				return false;
			}
		}
		if (said_count < 16) {
			said[said_count++] = id;
			fprintf(stderr, SONIX_SYSTEM ": the core asked for environment %u, not supported\n", id);
		}
		return false;
	}
	}
}

static void video_refresh_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!data) {
		return; /* a duplicated frame: what is there stays */
	}
	frame_data = data;
	frame_w = width;
	frame_h = height;
	frame_pitch = pitch;
}

static void audio_batch_grow(size_t frames) {
	if (audio_frames + frames <= audio_capacity) {
		return;
	}
	size_t cap = audio_capacity ? audio_capacity : 4096;
	while (cap < audio_frames + frames) {
		cap *= 2;
	}
	int16_t *grown = realloc(audio_buf, cap * 2 * sizeof(int16_t));
	if (grown) {
		audio_buf = grown;
		audio_capacity = cap;
	}
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
	audio_batch_grow(frames);
	if (audio_frames + frames <= audio_capacity) {
		memcpy(audio_buf + audio_frames * 2, data, frames * 2 * sizeof(int16_t));
		audio_frames += frames;
	}
	return frames;
}

static void audio_sample_cb(int16_t left, int16_t right) {
	int16_t pair[2] = {left, right};
	audio_batch_cb(pair, 1);
}

/* Linear resampling of the core's sound to OUT_RATE. `pos` is where the next output sample falls, in
 * input frames from the start of the chunk being read; -1 to 0 means between the last sample of
 * the chunk before and the first of this one. */
static double rs_pos;
static int16_t rs_last[2];
static int16_t *out_buf;
static size_t out_capacity;

static size_t resample(const int16_t *in, size_t n_in) {
	if (n_in == 0) {
		return 0;
	}
	double step = core_rate / (double)OUT_RATE;
	size_t need = (size_t)((double)n_in / step) + 4;
	if (need > out_capacity) {
		int16_t *grown = realloc(out_buf, need * 2 * sizeof(int16_t));
		if (!grown) {
			return 0;
		}
		out_buf = grown;
		out_capacity = need;
	}
	size_t n_out = 0;
	while (rs_pos < (double)n_in - 1.0 && n_out < out_capacity) {
		long i = (long)floor(rs_pos);
		double frac = rs_pos - (double)i;
		for (int c = 0; c < 2; c++) {
			double a = i < 0 ? rs_last[c] : in[i * 2 + c];
			double b = in[(i + 1) * 2 + c];
			out_buf[n_out * 2 + c] = (int16_t)(a + (b - a) * frac);
		}
		n_out++;
		rs_pos += step;
	}
	rs_pos -= (double)n_in;
	rs_last[0] = in[(n_in - 1) * 2];
	rs_last[1] = in[(n_in - 1) * 2 + 1];
	return n_out;
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void)index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD) {
		return 0;
	}
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
		return (int16_t)(pad_mask & 0xFFFF);
	}
	return (pad_mask >> id) & 1;
}

/* ---------------------------------------------------------------------------
 * Picture: the game scaled into the top of the panel
 * ------------------------------------------------------------------------- */

static int view_w;
static int view_h;
static int view_x;

static void compute_view(void) {
	unsigned w = geometry.base_width ? geometry.base_width : 256;
	unsigned h = geometry.base_height ? geometry.base_height : 224;
	double aspect = geometry.aspect_ratio > 0 ? geometry.aspect_ratio : (double)w / (double)h;
	view_w = panel_w;
	view_h = (int)(panel_w / aspect + 0.5);
	view_x = 0;
	if (view_h > panel_h * 62 / 100) {
		/* a tall picture leaves the pad too little room: fit the height instead */
		view_h = panel_h * 62 / 100;
		view_w = (int)(view_h * aspect + 0.5);
		view_x = (panel_w - view_w) / 2;
	}
}

static void draw_game(void) {
	if (!frame_data || frame_w == 0 || frame_h == 0) {
		return;
	}
	static int *xmap;
	static int xmap_for_w, xmap_for_vw;
	if (!xmap || xmap_for_w != (int)frame_w || xmap_for_vw != view_w) {
		free(xmap);
		xmap = malloc(sizeof(int) * (size_t)view_w);
		if (!xmap) {
			return;
		}
		for (int x = 0; x < view_w; x++) {
			xmap[x] = (int)((long long)x * (long long)frame_w / view_w);
		}
		xmap_for_w = (int)frame_w;
		xmap_for_vw = view_w;
	}

	for (int y = 0; y < view_h; y++) {
		int sy = (int)((long long)y * (long long)frame_h / view_h);
		uint16_t *dst = screen + (size_t)y * panel_w + view_x;
		const uint8_t *row = (const uint8_t *)frame_data + (size_t)sy * frame_pitch;
		if (pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
			const uint16_t *src = (const uint16_t *)row;
			for (int x = 0; x < view_w; x++) {
				dst[x] = src[xmap[x]];
			}
		} else if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
			const uint32_t *src = (const uint32_t *)row;
			for (int x = 0; x < view_w; x++) {
				uint32_t p = src[xmap[x]];
				dst[x] = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
			}
		} else {
			const uint16_t *src = (const uint16_t *)row;
			for (int x = 0; x < view_w; x++) {
				uint16_t p = src[xmap[x]];
				dst[x] = (uint16_t)(((p & 0x7C00) << 1) | ((p & 0x03E0) << 1) | (p & 0x001F));
			}
		}
	}
	/* what the picture does not cover, on either side, is black */
	if (view_x > 0) {
		sonix_fill_rect(screen, panel_w, panel_h, 0, 0, view_x, view_h, 0);
		sonix_fill_rect(screen, panel_w, panel_h, view_x + view_w, 0, panel_w - view_x - view_w, view_h, 0);
	}
}

/* ---------------------------------------------------------------------------
 * The pad
 * ------------------------------------------------------------------------- */

#define UI_EXIT 100
#define UI_SAVE 101
#define UI_LOAD 102
#define UI_MENU 103

typedef enum {
	SHAPE_RECT,
	SHAPE_ROUND,
	SHAPE_CIRCLE,
} shape_t;

typedef struct {
	int id; /* RETRO_DEVICE_ID_JOYPAD_* or UI_* */
	shape_t shape;
	int x, y, w, h;
	const char *label;
	bool down;
} button_t;

#define MAX_BUTTONS 20
static button_t buttons[MAX_BUTTONS];
static int button_count;

/* the cross */
static int dpad_cx, dpad_cy, dpad_r;
static bool dpad_active[4]; /* up, down, left, right */

static void add_button(int id, shape_t shape, int x, int y, int w, int h, const char *label) {
	if (button_count < MAX_BUTTONS) {
		button_t *b = &buttons[button_count++];
		b->id = id;
		b->shape = shape;
		b->x = x;
		b->y = y;
		b->w = w;
		b->h = h;
		b->label = label;
		b->down = false;
	}
}

static void build_pad(void) {
	button_count = 0;
	int top = view_h + 6;
	int bar_h = 44;

	/* the bar between the picture and the pad: SAVE, LOAD, MENU, EXIT */
	int bw = 100;
	int gap = (panel_w - 4 * bw) / 5;
	add_button(UI_SAVE, SHAPE_ROUND, gap, top, bw, bar_h, "SAVE");
	add_button(UI_LOAD, SHAPE_ROUND, gap * 2 + bw, top, bw, bar_h, "LOAD");
	add_button(UI_MENU, SHAPE_ROUND, gap * 3 + bw * 2, top, bw, bar_h, "MENU");
	add_button(UI_EXIT, SHAPE_ROUND, gap * 4 + bw * 3, top, bw, bar_h, "EXIT");

	int pad_top = top + bar_h + 8;
	int pad_h = panel_h - pad_top - 8;

	/* shoulders, for the cores that have them */
	if (SONIX_LAYOUT == LAYOUT_GBA || SONIX_LAYOUT == LAYOUT_SNES) {
		add_button(RETRO_DEVICE_ID_JOYPAD_L, SHAPE_ROUND, 10, pad_top, 150, 40, "L");
		add_button(RETRO_DEVICE_ID_JOYPAD_R, SHAPE_ROUND, panel_w - 160, pad_top, 150, 40, "R");
		pad_top += 46;
		pad_h -= 46;
	}

	dpad_r = pad_h / 2 - 14;
	if (dpad_r > 92) {
		dpad_r = 92;
	}
	dpad_cx = 20 + dpad_r + 10;
	dpad_cy = pad_top + pad_h / 2 - 10;

	int face_r = dpad_r * 36 / 100 + 10;
	int fx = panel_w - 30 - dpad_r - 10;
	int fy = dpad_cy;
	if (SONIX_LAYOUT == LAYOUT_SNES) {
		int d = dpad_r * 62 / 100 + 6;
		add_button(RETRO_DEVICE_ID_JOYPAD_X, SHAPE_CIRCLE, fx, fy - d, face_r * 2 - 14, face_r * 2 - 14, "X");
		add_button(RETRO_DEVICE_ID_JOYPAD_Y, SHAPE_CIRCLE, fx - d, fy, face_r * 2 - 14, face_r * 2 - 14, "Y");
		add_button(RETRO_DEVICE_ID_JOYPAD_A, SHAPE_CIRCLE, fx + d, fy, face_r * 2 - 14, face_r * 2 - 14, "A");
		add_button(RETRO_DEVICE_ID_JOYPAD_B, SHAPE_CIRCLE, fx, fy + d, face_r * 2 - 14, face_r * 2 - 14, "B");
	} else {
		int d = dpad_r * 52 / 100 + 8;
		add_button(RETRO_DEVICE_ID_JOYPAD_B, SHAPE_CIRCLE, fx - d, fy + d / 3, face_r * 2, face_r * 2, "B");
		add_button(RETRO_DEVICE_ID_JOYPAD_A, SHAPE_CIRCLE, fx + d, fy - d / 3, face_r * 2, face_r * 2, "A");
	}

	/* SELECT and START, together at the bottom */
	int sy = pad_top + pad_h - 30;
	add_button(RETRO_DEVICE_ID_JOYPAD_SELECT, SHAPE_ROUND, panel_w / 2 - 90, sy, 84, 34, "SELECT");
	add_button(RETRO_DEVICE_ID_JOYPAD_START, SHAPE_ROUND, panel_w / 2 + 6, sy, 84, 34, "START");
}

static bool in_button(const button_t *b, int x, int y) {
	if (b->shape == SHAPE_CIRCLE) {
		/* a circle's centre is x,y; its hit area is more generous than what is drawn */
		int dx = x - b->x;
		int dy = y - b->y;
		int r = b->w / 2 + 14;
		return dx * dx + dy * dy <= r * r;
	}
	int slop = 6;
	return x >= b->x - slop && x < b->x + b->w + slop && y >= b->y - slop && y < b->y + b->h + slop;
}

static const uint16_t COL_BG = 0;
static uint16_t col_idle, col_down, col_edge, col_text, col_pad;

static uint64_t exit_armed_until; /* EXIT was tapped: a second tap before this leaves */

static const char *toast_text;
static uint64_t toast_until;

static void draw_pad(void) {
	int pad_top = view_h + 6;
	sonix_fill_rect(screen, panel_w, panel_h, 0, pad_top, panel_w, panel_h - pad_top, COL_BG);

	/* the cross */
	int arm = dpad_r * 2 / 3;
	int thick = dpad_r * 2 * 36 / 100;
	sonix_round_rect(screen, panel_w, panel_h, dpad_cx - dpad_r, dpad_cy - thick / 2, dpad_r * 2, thick, thick / 3,
					 col_pad);
	sonix_round_rect(screen, panel_w, panel_h, dpad_cx - thick / 2, dpad_cy - dpad_r, thick, dpad_r * 2, thick / 3,
					 col_pad);
	(void)arm;
	if (dpad_active[0]) {
		sonix_round_rect(screen, panel_w, panel_h, dpad_cx - thick / 2, dpad_cy - dpad_r, thick, dpad_r - thick / 4, thick / 3, col_down);
	}
	if (dpad_active[1]) {
		sonix_round_rect(screen, panel_w, panel_h, dpad_cx - thick / 2, dpad_cy + thick / 4, thick, dpad_r - thick / 4, thick / 3, col_down);
	}
	if (dpad_active[2]) {
		sonix_round_rect(screen, panel_w, panel_h, dpad_cx - dpad_r, dpad_cy - thick / 2, dpad_r - thick / 4, thick, thick / 3, col_down);
	}
	if (dpad_active[3]) {
		sonix_round_rect(screen, panel_w, panel_h, dpad_cx + thick / 4, dpad_cy - thick / 2, dpad_r - thick / 4, thick, thick / 3, col_down);
	}
	sonix_circle(screen, panel_w, panel_h, dpad_cx, dpad_cy, thick / 4, col_edge);

	for (int i = 0; i < button_count; i++) {
		button_t *b = &buttons[i];
		if (b->id == UI_EXIT) {
			b->label = exit_armed_until > sonix_time_us() ? "SURE?" : "EXIT";
		}
		uint16_t fill = b->down ? col_down : col_idle;
		int scale = 2;
		if (b->shape == SHAPE_CIRCLE) {
			sonix_circle(screen, panel_w, panel_h, b->x, b->y, b->w / 2, fill);
			sonix_draw_text(screen, panel_w, panel_h, b->x - sonix_text_width(b->label, 3) / 2, b->y - 10, b->label,
							col_text, 3);
		} else {
			sonix_round_rect(screen, panel_w, panel_h, b->x, b->y, b->w, b->h, b->h / 2, fill);
			if (b->h < 36) {
				scale = 2;
			}
			sonix_draw_text(screen, panel_w, panel_h, b->x + (b->w - sonix_text_width(b->label, scale)) / 2,
							b->y + (b->h - 7 * scale) / 2, b->label, col_text, scale);
		}
	}

}

/* The options menu (the MENU key): the frame counter, the sound and how many pictures are skipped.
 * They are kept in a file in the saves folder (Saves/<system>/.options), so they stay as they were
 * left. The game is paused while the menu is up. */
static bool show_fps;
static bool sound_on = true;
static int frame_skip; /* pictures not drawn between two that are: 0, 1 or 2 */
static bool menu_open;
static bool menu_dirty;
static int fps_tenths; /* frames per second, times ten */
static int fps_frames;
static uint64_t fps_stamp;

static void options_path(char *out, size_t size, const char *name) { snprintf(out, size, "%s/%s", save_dir, name); }

static void options_load(void) {
	char path[640];
	/* the counter was a file of its own in the first version of the apps */
	options_path(path, sizeof(path), ".fps");
	show_fps = access(path, F_OK) == 0;
	options_path(path, sizeof(path), ".options");
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[64];
	while (fgets(line, sizeof(line), f)) {
		int v;
		if (sscanf(line, "fps=%d", &v) == 1) {
			show_fps = v != 0;
		} else if (sscanf(line, "sound=%d", &v) == 1) {
			sound_on = v != 0;
		} else if (sscanf(line, "skip=%d", &v) == 1) {
			frame_skip = v < 0 ? 0 : v > 2 ? 2 : v;
		}
	}
	fclose(f);
}

static void options_store(void) {
	char path[640];
	options_path(path, sizeof(path), ".options");
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "fps=%d%ssound=%d%sskip=%d%s", show_fps, "\n", sound_on, "\n", frame_skip, "\n");
		fclose(f);
	}
	options_path(path, sizeof(path), ".fps");
	remove(path); /* it is in the options file now */
}

/* Counts a frame, and every thirty works out what they came to in the time they took. */
static void fps_count(void) {
	uint64_t now = sonix_time_us();
	if (fps_stamp == 0) {
		fps_stamp = now;
		return;
	}
	if (++fps_frames >= 30) {
		fps_tenths = (int)((uint64_t)fps_frames * 10000000ull / (now - fps_stamp));
		fps_frames = 0;
		fps_stamp = now;
	}
}

static void draw_fps(void) {
	if (!show_fps || fps_tenths == 0) {
		return;
	}
	char text[32];
	snprintf(text, sizeof(text), "%d.%d FPS", fps_tenths / 10, fps_tenths % 10);
	int tw = sonix_text_width(text, 2);
	sonix_fill_rect(screen, panel_w, panel_h, 6, 6, tw + 12, 26, sonix_rgb(0, 0, 0));
	sonix_draw_text(screen, panel_w, panel_h, 12, 12, text, sonix_rgb(150, 255, 120), 2);
}

/* A short message over the bottom of the game, for as long as it lasts. */
static void draw_toast(void) {
	if (toast_text && sonix_time_us() < toast_until) {
		int tw = sonix_text_width(toast_text, 3);
		sonix_fill_rect(screen, panel_w, panel_h, (panel_w - tw) / 2 - 10, view_h - 40, tw + 20, 34, sonix_rgb(0, 0, 0));
		sonix_draw_text(screen, panel_w, panel_h, (panel_w - tw) / 2, view_h - 33, toast_text, sonix_rgb(255, 255, 255), 3);
	}
}

/* The options, over the picture. Three rows and a key to go back; a tap on a row changes it. */
#define MENU_ROWS 3

static void menu_geometry(int *first_y, int *row_h, int *resume_y, int *resume_h) {
	*resume_h = 52;
	*first_y = 56;
	int room = view_h - *first_y - *resume_h - 20;
	*row_h = room / MENU_ROWS;
	if (*row_h > 60) {
		*row_h = 60;
	}
	*resume_y = *first_y + *row_h * MENU_ROWS + 12;
}

static const char *menu_value(int row) {
	static const char *const skips[3] = {"OFF", "1", "2"};
	switch (row) {
	case 0:
		return show_fps ? "ON" : "OFF";
	case 1:
		return sound_on ? "ON" : "OFF";
	default:
		return skips[frame_skip];
	}
}

static void draw_menu(void) {
	static const char *const names[MENU_ROWS] = {"FPS COUNTER", "SOUND", "FRAME SKIP"};
	int first_y, row_h, resume_y, resume_h;
	menu_geometry(&first_y, &row_h, &resume_y, &resume_h);
	sonix_fill_rect(screen, panel_w, panel_h, 0, 0, panel_w, view_h, sonix_rgb(14, 18, 28));
	sonix_draw_text(screen, panel_w, panel_h, (panel_w - sonix_text_width("OPTIONS", 3)) / 2, 18, "OPTIONS",
					sonix_rgb(255, 255, 255), 3);
	for (int i = 0; i < MENU_ROWS; i++) {
		int y = first_y + i * row_h;
		sonix_round_rect(screen, panel_w, panel_h, 16, y + 3, panel_w - 32, row_h - 6, (row_h - 6) / 2, col_idle);
		sonix_draw_text(screen, panel_w, panel_h, 36, y + (row_h - 14) / 2, names[i], col_text, 2);
		const char *v = menu_value(i);
		int pw = 96;
		int px = panel_w - 16 - 10 - pw;
		bool on = !(strcmp(v, "OFF") == 0);
		sonix_round_rect(screen, panel_w, panel_h, px, y + 9, pw, row_h - 18, (row_h - 18) / 2,
						 on ? sonix_rgb(40, 110, 220) : col_pad);
		sonix_draw_text(screen, panel_w, panel_h, px + (pw - sonix_text_width(v, 2)) / 2, y + (row_h - 14) / 2, v,
						col_text, 2);
	}
	sonix_round_rect(screen, panel_w, panel_h, 16, resume_y, panel_w - 32, resume_h, resume_h / 2, sonix_rgb(40, 110, 220));
	sonix_draw_text(screen, panel_w, panel_h, (panel_w - sonix_text_width("RESUME", 3)) / 2, resume_y + (resume_h - 21) / 2,
					"RESUME", sonix_rgb(255, 255, 255), 3);
}

/* A finger arrived on the menu at x,y. */
static void menu_tap(int y) {
	int first_y, row_h, resume_y, resume_h;
	menu_geometry(&first_y, &row_h, &resume_y, &resume_h);
	if (y >= resume_y - 6 && y < resume_y + resume_h + 6) {
		menu_open = false;
		options_store();
	} else if (y >= first_y && y < first_y + row_h * MENU_ROWS) {
		switch ((y - first_y) / row_h) {
		case 0:
			show_fps = !show_fps;
			fps_tenths = 0;
			fps_frames = 0;
			fps_stamp = 0;
			break;
		case 1:
			sound_on = !sound_on;
			break;
		default:
			frame_skip = (frame_skip + 1) % 3;
			break;
		}
		options_store();
	} else {
		return;
	}
	menu_dirty = true;
}

/* What the pad looks like, as one number: it is redrawn only when this changes. */
static uint32_t pad_signature(void) {
	uint32_t sig = 2166136261u;
	for (int i = 0; i < button_count; i++) {
		sig = (sig ^ (uint32_t)(buttons[i].down ? 1 : 0)) * 16777619u;
	}
	for (int i = 0; i < 4; i++) {
		sig = (sig ^ (uint32_t)(dpad_active[i] ? 1 : 0)) * 16777619u;
	}
	sig = (sig ^ (exit_armed_until > sonix_time_us() ? 1u : 0u)) * 16777619u;
	return sig;
}

static void say(const char *text) {
	toast_text = text;
	toast_until = sonix_time_us() + 1200000;
}

/* ---------------------------------------------------------------------------
 * Saves
 * ------------------------------------------------------------------------- */

static void save_path(char *out, size_t size, const char *ext) {
	snprintf(out, size, "%s/%s.%s", save_dir, rom_base, ext);
}

static uint32_t crc_of(const uint8_t *p, size_t n) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 16777619u;
	}
	return h;
}

static uint32_t sram_hash;

static void sram_load(void) {
	size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!size || !data) {
		return;
	}
	char path[1024];
	save_path(path, sizeof(path), "srm");
	FILE *f = fopen(path, "rb");
	if (f) {
		size_t got = fread(data, 1, size, f);
		(void)got;
		fclose(f);
	}
	sram_hash = crc_of(data, size);
}

static void sram_save(bool force) {
	size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!size || !data) {
		return;
	}
	uint32_t h = crc_of(data, size);
	if (!force && h == sram_hash) {
		return;
	}
	char path[1024];
	char tmp[1040];
	save_path(path, sizeof(path), "srm");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		return;
	}
	bool ok = fwrite(data, 1, size, f) == size;
	ok = fclose(f) == 0 && ok;
	if (ok) {
		rename(tmp, path); /* a power cut leaves the old save, not half of a new one */
		sram_hash = h;
	}
}

static void state_save(void) {
	size_t size = retro_serialize_size();
	if (!size) {
		say("NO SAVES HERE");
		return;
	}
	void *buf = malloc(size);
	char path[1024];
	save_path(path, sizeof(path), "st0");
	FILE *f = buf && retro_serialize(buf, size) ? fopen(path, "wb") : NULL;
	bool ok = f && fwrite(buf, 1, size, f) == size;
	if (f) {
		ok = fclose(f) == 0 && ok;
	}
	free(buf);
	say(ok ? "SAVED" : "SAVE FAILED");
}

static void state_load(void) {
	size_t size = retro_serialize_size();
	char path[1024];
	save_path(path, sizeof(path), "st0");
	FILE *f = fopen(path, "rb");
	if (!f || !size) {
		if (f) {
			fclose(f);
		}
		say("NOTHING SAVED");
		return;
	}
	void *buf = malloc(size);
	bool ok = buf && fread(buf, 1, size, f) == size && retro_unserialize(buf, size);
	fclose(f);
	free(buf);
	say(ok ? "LOADED" : "LOAD FAILED");
}

/* ---------------------------------------------------------------------------
 * Touch to buttons
 * ------------------------------------------------------------------------- */

static bool exit_was_down;

static void apply_touches(const sonix_touches_t *t) {
	uint32_t mask = 0;
	bool save_hit = false;
	bool load_hit = false;
	bool exit_hit = false;
	bool menu_hit = false;
	for (int i = 0; i < button_count; i++) {
		buttons[i].down = false;
	}
	memset(dpad_active, 0, sizeof(dpad_active));

	for (int i = 0; i < t->count; i++) {
		int x = t->x[i];
		int y = t->y[i];

		/* the cross: a finger anywhere on it, and diagonals when it is in a corner */
		int dx = x - dpad_cx;
		int dy = y - dpad_cy;
		int reach = dpad_r + 26;
		if (dx * dx + dy * dy <= reach * reach) {
			int dead = dpad_r * 28 / 100;
			if (dy < -dead) {
				mask |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
				dpad_active[0] = true;
			}
			if (dy > dead) {
				mask |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
				dpad_active[1] = true;
			}
			if (dx < -dead) {
				mask |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
				dpad_active[2] = true;
			}
			if (dx > dead) {
				mask |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
				dpad_active[3] = true;
			}
		}
		for (int b = 0; b < button_count; b++) {
			if (!in_button(&buttons[b], x, y)) {
				continue;
			}
			buttons[b].down = true;
			if (buttons[b].id == UI_SAVE) {
				save_hit = true;
			} else if (buttons[b].id == UI_LOAD) {
				load_hit = true;
			} else if (buttons[b].id == UI_EXIT) {
				exit_hit = true;
			} else if (buttons[b].id == UI_MENU) {
				menu_hit = true;
			} else {
				mask |= 1u << buttons[b].id;
			}
		}
	}
	pad_mask = mask;

	/* MENU opens the options and, while they are up, closes them */
	static bool menu_was;
	if (menu_hit && !menu_was) {
		menu_open = !menu_open;
		menu_dirty = true;
		if (!menu_open) {
			options_store();
		}
	}
	menu_was = menu_hit;
	if (menu_open) {
		pad_mask = 0;
		memset(dpad_active, 0, sizeof(dpad_active));
		static bool menu_finger_was;
		bool menu_finger = false;
		int menu_y = 0;
		for (int i = 0; i < t->count; i++) {
			if (t->y[i] < view_h) {
				menu_finger = true;
				menu_y = t->y[i];
			}
		}
		if (menu_finger && !menu_finger_was) {
			menu_tap(menu_y);
		}
		menu_finger_was = menu_finger;
	}

	/* SAVE and LOAD act when the finger arrives, once. EXIT takes two taps, the first one turns the
	 * key into SURE? for two seconds, so that a hand passing over it cannot end the game */
	static bool save_was, load_was;
	if (save_hit && !save_was) {
		state_save();
	}
	if (load_hit && !load_was) {
		state_load();
	}
	save_was = save_hit;
	load_was = load_hit;
	if (exit_hit && !exit_was_down) {
		uint64_t now = sonix_time_us();
		if (exit_armed_until > now) {
			quit_requested = 1;
		} else {
			exit_armed_until = now + 2000000;
		}
	}
	exit_was_down = exit_hit;
}

/* ---------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

static void on_signal(int sig) {
	(void)sig;
	quit_requested = 1;
}

/* A crash leaves a line in the log and ends the program with the usual 128 + signal, so that
 * whatever started it (the player, or a script that tries something else) can tell. */
static void on_crash(int sig) {
	static const char msg[] = SONIX_SYSTEM ": crashed\n";
	if (write(2, msg, sizeof(msg) - 1) < 0) {
		/* nothing to do about it */
	}
	_exit(128 + sig);
}

/* The game file as the core is given it. It is mapped, not read: a 16 MB game read into memory and
 * then copied by the core takes 32 MB of a player that has 58, and the mapping is file pages that
 * the system can let go of. `*mapped` says it has to be unmapped, not freed. */
static uint8_t *read_all(const char *path, size_t *size, bool *mapped) {
	*mapped = false;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}
	off_t n = lseek(fd, 0, SEEK_END);
	if (n <= 0) {
		close(fd);
		return NULL;
	}
	*size = (size_t)n;
	void *m = mmap(NULL, (size_t)n, PROT_READ, MAP_PRIVATE, fd, 0);
	if (m != MAP_FAILED) {
		close(fd);
		*mapped = true;
		return m;
	}
	uint8_t *data = malloc((size_t)n);
	if (data && (lseek(fd, 0, SEEK_SET) != 0 || read(fd, data, (size_t)n) != n)) {
		free(data);
		data = NULL;
	}
	close(fd);
	return data;
}

/* What the system has left, for the log: this is what decides whether a big game starts. */
static void log_memory(const char *when) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return;
	}
	char line[96];
	long total = 0;
	long avail = 0;
	long cached = 0;
	long swap_free = 0;
	while (fgets(line, sizeof(line), f)) {
		sscanf(line, "MemTotal: %ld", &total);
		sscanf(line, "MemAvailable: %ld", &avail);
		sscanf(line, "Cached: %ld", &cached);
		sscanf(line, "SwapFree: %ld", &swap_free);
	}
	fclose(f);
	fprintf(stderr, SONIX_SYSTEM ": memory %s: %ld of %ld kB available, %ld kB cached, %ld kB swap free\n", when, avail,
			total, cached, swap_free);
}

static void set_base_name(const char *path) {
	const char *slash = strrchr(path, '/');
	snprintf(rom_base, sizeof(rom_base), "%s", slash ? slash + 1 : path);
	char *dot = strrchr(rom_base, '.');
	if (dot) {
		*dot = '\0';
	}
}

int main(int argc, char **argv) {
	setvbuf(stderr, NULL, _IONBF, 0);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGSEGV, on_crash);
	signal(SIGBUS, on_crash);
	signal(SIGILL, on_crash);
	signal(SIGFPE, on_crash);
	signal(SIGABRT, on_crash);

	sonix_env(&env);
	if (env.under_player) {
		/* The emulator runs a little ahead of ordinary threads: a frame that is late is a gap in
		 * the sound. It sleeps or blocks every frame, so it cannot starve anything. */
		struct sched_param rt;
		memset(&rt, 0, sizeof(rt));
		rt.sched_priority = 5;
		if (sched_setscheduler(0, SCHED_RR, &rt) != 0) {
			fprintf(stderr, SONIX_SYSTEM ": runs at normal priority\n");
		}
	}
	if (getenv("SONIX_TEST_FRAMES")) {
		test_frames = atol(getenv("SONIX_TEST_FRAMES"));
	}
	if (getenv("SONIX_TEST_MENU")) {
		menu_open = true; /* the options, up from the start */
	}
	if (getenv("SONIX_TEST_TOUCH")) {
		have_test_touch = true;
		const char *p = getenv("SONIX_TEST_TOUCH");
		while (*p && test_touch.count < SONIX_MAX_TOUCH) {
			int x;
			int y;
			int used = 0;
			if (sscanf(p, "%d,%d%n", &x, &y, &used) < 2) {
				break;
			}
			test_touch.x[test_touch.count] = x;
			test_touch.y[test_touch.count] = y;
			test_touch.count++;
			p += used;
			while (*p == ' ') {
				p++;
			}
		}
	}
	const char *rom_path = argc > 1 ? argv[1] : env.rom;
	if (!rom_path) {
		fprintf(stderr, SONIX_SYSTEM ": no game given\n");
		return 2;
	}
	set_base_name(rom_path);

	snprintf(save_dir, sizeof(save_dir), "%s/Saves/%s", env.sd, SONIX_SYSTEM);
	snprintf(system_dir, sizeof(system_dir), "%s/system", env.app_dir);
	make_dir_path(save_dir);
	make_dir_path(system_dir);

	video = sonix_video_open(&env);
	if (!video) {
		fprintf(stderr, SONIX_SYSTEM ": no framebuffer\n");
		return 1;
	}
	panel_w = sonix_video_width(video);
	panel_h = sonix_video_height(video);
	screen = sonix_video_frame(video);
	touch = sonix_touch_open(&env);

	col_idle = sonix_rgb(44, 49, 64);
	col_down = sonix_rgb(96, 140, 255);
	col_edge = sonix_rgb(30, 34, 46);
	col_pad = sonix_rgb(54, 60, 78);
	col_text = sonix_rgb(225, 230, 245);

	/* the core */
	retro_set_environment(environment_cb);
	options_load_overrides();
	retro_init();
	retro_set_video_refresh(video_refresh_cb);
	retro_set_audio_sample(audio_sample_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);

	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	retro_get_system_info(&info);

	struct retro_game_info game;
	memset(&game, 0, sizeof(game));
	game.path = rom_path;
	uint8_t *rom_data = NULL;
	bool rom_mapped = false;
	size_t rom_size = 0;
	log_memory("at the start");
	if (!info.need_fullpath) {
		size_t size;
		rom_data = read_all(rom_path, &size, &rom_mapped);
		rom_size = size;
		if (!rom_data) {
			fprintf(stderr, SONIX_SYSTEM ": cannot read %s\n", rom_path);
			return 1;
		}
		game.data = rom_data;
		game.size = size;
	}
	if (!retro_load_game(&game)) {
		fprintf(stderr, SONIX_SYSTEM ": the core does not take %s\n", rom_path);
		return 1;
	}
	/* The core has its own copy by now: the game file is no longer needed in memory. */
	if (rom_data) {
		if (rom_mapped) {
			munmap(rom_data, rom_size);
		} else {
			free(rom_data);
		}
		rom_data = NULL;
	}
	log_memory("with the game loaded");

	struct retro_system_av_info av;
	memset(&av, 0, sizeof(av));
	retro_get_system_av_info(&av);
	geometry = av.geometry;
	core_fps = av.timing.fps > 1 ? av.timing.fps : 60.0;
	core_rate = av.timing.sample_rate > 1000 ? av.timing.sample_rate : 44100.0;
	retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

	audio = sonix_audio_open(&env, OUT_RATE, 2);
	sram_load();
	options_load();
	fprintf(stderr, SONIX_SYSTEM ": %s, %ux%u at %.2f fps, sound %.0f Hz%s\n", rom_base, geometry.base_width,
			geometry.base_height, core_fps, core_rate, audio ? "" : " (no sound pipe)");

	compute_view();
	build_pad();
	sonix_fill_rect(screen, panel_w, panel_h, 0, 0, panel_w, panel_h, COL_BG);
	draw_pad();
	fprintf(stderr, SONIX_SYSTEM ": running, sound %s\n", audio ? "through the player" : "off");

	uint64_t frame_us = (uint64_t)(1000000.0 / core_fps);
	uint64_t next = sonix_time_us();
	uint64_t last_sram_check = next;
	sonix_touches_t fingers;
	memset(&fingers, 0, sizeof(fingers));
	unsigned last_w = 0;
	unsigned last_h = 0;
	uint32_t last_sig = 0;
	bool pad_redraw = true;
	bool menu_was_open = false;
	/* how long each part of a frame takes, for the log: sum and longest, in microseconds */
	int64_t st_sum[4] = {0, 0, 0, 0};
	int64_t st_max[4] = {0, 0, 0, 0};
	int st_frames = 0;

	long frames_run = 0;
	while (!quit_requested) {
		if (touch) {
			sonix_touch_read(touch, &fingers);
		}
		if (have_test_touch) {
			/* SONIX_TEST_BLINK=1 lifts the fingers for 20 frames out of every 30, so that a test
			 * can tap */
			bool lifted = getenv("SONIX_TEST_BLINK") && (frames_run % 30) >= 10;
			fingers = test_touch;
			if (lifted) {
				fingers.count = 0;
			}
		}
		apply_touches(&fingers);

		audio_frames = 0;
		int64_t t_start = (int64_t)sonix_time_us();
		bool was_open = menu_was_open;
		menu_was_open = menu_open;
		if (menu_open) {
			/* paused: the menu is drawn when it changes, and the sound card is fed silence so that
			 * it does not click and the loop keeps its pace */
			if (menu_dirty || !was_open) {
				draw_menu();
				sonix_video_dirty(video, 0, view_h);
				menu_dirty = false;
			}
			uint32_t sig = pad_signature();
			if (sig != last_sig || pad_redraw) {
				draw_pad();
				sonix_video_dirty(video, view_h + 6, panel_h - view_h - 6);
				last_sig = sig;
				pad_redraw = false;
			}
			sonix_video_present_dirty(video);
			if (audio) {
				size_t quiet = (size_t)((double)OUT_RATE / core_fps);
				static int16_t silence[2 * 2048];
				if (quiet > 2048) {
					quiet = 2048;
				}
				if (!sonix_audio_write(audio, silence, (int)quiet)) {
					break;
				}
			}
			next += frame_us;
			uint64_t paced_now = sonix_time_us();
			if (next > paced_now) {
				sonix_sleep_us(next - paced_now);
			} else if (paced_now - next > 200000) {
				next = paced_now;
			}
			frames_run++;
			if (test_frames && frames_run >= test_frames) {
				break;
			}
			continue;
		}
		if (was_open) {
			/* the game is back: all of it is drawn again */
			pad_redraw = true;
			sonix_fill_rect(screen, panel_w, panel_h, 0, 0, panel_w, panel_h, COL_BG);
			sonix_video_dirty(video, 0, panel_h);
		}
		retro_run();

		if (frame_w != last_w || frame_h != last_h) {
			/* the core changed its picture: the pad moves with it */
			last_w = frame_w;
			last_h = frame_h;
			if (geometry.base_width == 0 || (int)frame_w != (int)geometry.base_width) {
				geometry.base_width = frame_w;
				geometry.base_height = frame_h;
				geometry.aspect_ratio = 0;
			}
			compute_view();
			build_pad();
			sonix_fill_rect(screen, panel_w, panel_h, 0, 0, panel_w, panel_h, COL_BG);
			sonix_video_dirty(video, 0, panel_h);
			pad_redraw = true;
		}
		int64_t t_run = (int64_t)sonix_time_us();
		if (frame_skip == 0 || frames_run % (frame_skip + 1) == 0) {
			draw_game();
			sonix_video_dirty(video, 0, view_h);
		}
		uint32_t sig = pad_signature();
		if (sig != last_sig || pad_redraw) {
			draw_pad();
			sonix_video_dirty(video, view_h + 6, panel_h - view_h - 6);
			last_sig = sig;
			pad_redraw = false;
		}
		fps_count();
		draw_fps();
		draw_toast();
		int64_t t_draw = (int64_t)sonix_time_us();
		sonix_video_present_dirty(video);
		int64_t t_present = (int64_t)sonix_time_us();

		int64_t t_audio = (int64_t)sonix_time_us();
		if (audio && audio_frames > 0) {
			size_t n = resample(audio_buf, audio_frames);
			if (!sound_on) {
				memset(out_buf, 0, n * 2 * sizeof(int16_t)); /* muted, but the card still sets the pace */
			}
			if (n > 0 && !sonix_audio_write(audio, out_buf, (int)n)) {
				break; /* the player has gone */
			}
		}
		int64_t t_wrote = (int64_t)sonix_time_us();

		/* every 600 frames the log says where the time went: the core, the picture, the
		 * framebuffer, and the wait on the sound (which is the sound card setting the pace) */
		int64_t parts[4] = {t_run - t_start, t_draw - t_run, t_present - t_draw, t_wrote - t_audio};
		for (int i = 0; i < 4; i++) {
			st_sum[i] += parts[i];
			if (parts[i] > st_max[i]) {
				st_max[i] = parts[i];
			}
		}
		if (++st_frames == 600) {
			fprintf(stderr,
					SONIX_SYSTEM ": 600 frames, ms average/longest: core %.1f/%.1f, picture %.1f/%.1f, "
								 "framebuffer %.1f/%.1f, sound wait %.1f/%.1f\n",
					st_sum[0] / 600000.0, st_max[0] / 1000.0, st_sum[1] / 600000.0, st_max[1] / 1000.0,
					st_sum[2] / 600000.0, st_max[2] / 1000.0, st_sum[3] / 600000.0, st_max[3] / 1000.0);
			memset(st_sum, 0, sizeof(st_sum));
			memset(st_max, 0, sizeof(st_max));
			st_frames = 0;
		}
		/* Paced by the clock as well as by the sound: the sound card slows the loop to the right
		 * speed, and when the player could not open the output (or the core makes no sound) the
		 * clock does it instead of the game running flat out. */
		next += frame_us;
		uint64_t paced = sonix_time_us();
		if (next > paced) {
			sonix_sleep_us(next - paced);
		} else if (paced - next > 200000) {
			next = paced; /* long behind: do not run flat out to catch up */
		}

		frames_run++;
		if (test_frames && frames_run >= test_frames) {
			break;
		}

		uint64_t now = sonix_time_us();
		if (now - last_sram_check > 20u * 1000000u) {
			last_sram_check = now;
			sram_save(false); /* the battery save is safe within twenty seconds of any change */
		}
	}

	sram_save(true);
	retro_unload_game();
	retro_deinit();
	sonix_audio_close(audio);
	sonix_touch_close(touch);
	sonix_video_close(video);
	return 0;
}
