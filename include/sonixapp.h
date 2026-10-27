/*
 * sonixapp.h - what an app for Sonix Plus needs to draw, to read the touch panel
 * and to make sound. One header, no dependencies beyond the C library and the
 * Linux headers.
 *
 *   MIT licence.
 *
 * In exactly one .c file:
 *
 *     #define SONIXAPP_IMPLEMENTATION
 *     #include "sonixapp.h"
 *
 * Anywhere else just include it.
 *
 * The player starts an app as a child process and hands it the framebuffer, the
 * touch panel and a pipe that carries sound back to the player. All of it is
 * described to the app by environment variables, which sonix_env() reads. The
 * whole arrangement is documented in APPS.md of the Sonix Plus repository.
 *
 * Testing on a computer: outside the player SONIX_FB may name an ordinary file;
 * the frame is then written into it as raw RGB565, width x height x 2 bytes,
 * instead of to a framebuffer, and a picture of it can be made with any tool.
 * Without SONIX_TOUCH there is no touch, and without SONIX_AUDIO_FD no sound.
 */

#ifndef SONIXAPP_H
#define SONIXAPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SONIX_MAX_TOUCH 5

/* ----------------------------------------------------------------------------
 * The environment the player gives an app
 * ------------------------------------------------------------------------- */

typedef struct {
	int width;			   /* the panel: 480 x 720 on the R3 Pro II, 480 x 800 on the R1 */
	int height;
	bool rotated;		   /* the picture is turned 180 degrees; present() does that itself */
	bool under_player;	   /* started by the player (SONIX_API is set) */
	const char *app_dir;   /* the app's own folder; the working directory too */
	const char *sd;		   /* the root of the card */
	const char *rom;	   /* the file the user chose, or NULL */
	const char *fb_path;   /* /dev/fb0 */
	const char *touch_path; /* the touch panel's evdev node, or "" */
	int audio_fd;		   /* the pipe for sound, or -1 */
} sonix_env_t;

/* Reads the environment and makes the app's folder the working directory. Fills
 * in a 480x720 panel when run outside the player. */
void sonix_env(sonix_env_t *env);

/* ----------------------------------------------------------------------------
 * Video
 * ------------------------------------------------------------------------- */

typedef struct sonix_video sonix_video_t;

/* Maps the framebuffer. NULL when it cannot be opened or is not 16 bits a pixel. */
sonix_video_t *sonix_video_open(const sonix_env_t *env);

int sonix_video_width(const sonix_video_t *v);
int sonix_video_height(const sonix_video_t *v);

/* The frame to draw into: width x height RGB565, row by row from the top left, as
 * the interface sees the panel (upright, whatever the rotation setting). Its
 * contents stay between calls. */
uint16_t *sonix_video_frame(sonix_video_t *v);

/* Shows the frame. With the player's double-height framebuffer this draws on the
 * page that is not on show and flips to it, so there is no tearing. */
void sonix_video_present(sonix_video_t *v);

void sonix_video_close(sonix_video_t *v);

/* ----------------------------------------------------------------------------
 * Touch
 * ------------------------------------------------------------------------- */

typedef struct {
	int count; /* fingers on the glass */
	int x[SONIX_MAX_TOUCH];
	int y[SONIX_MAX_TOUCH]; /* in the frame's pixels, rotation applied */
} sonix_touches_t;

typedef struct sonix_touch sonix_touch_t;

/* Opens the touch panel. NULL when there is none. */
sonix_touch_t *sonix_touch_open(const sonix_env_t *env);

/* A descriptor to poll() on. */
int sonix_touch_fd(const sonix_touch_t *t);

/* Reads what has arrived and puts the fingers now on the glass in `out`. Returns
 * true when anything arrived, false when there was nothing new (the last state
 * still stands; `out` is filled either way). Never blocks. */
bool sonix_touch_read(sonix_touch_t *t, sonix_touches_t *out);

void sonix_touch_close(sonix_touch_t *t);

/* ----------------------------------------------------------------------------
 * Audio
 * ------------------------------------------------------------------------- */

typedef struct sonix_audio sonix_audio_t;

/* Opens the pipe to the player's sound output. `rate` is the sample rate, `channels`
 * 1 or 2. NULL when there is no pipe. */
sonix_audio_t *sonix_audio_open(const sonix_env_t *env, int rate, int channels);

/* Plays `frames` frames of signed 16-bit interleaved samples. Blocks until the
 * pipe has taken them, which is what keeps an app in step with the sound card:
 * write one frame's worth of sound per frame drawn and the app runs at the speed
 * of the sound. False when the player has gone. */
bool sonix_audio_write(sonix_audio_t *a, const int16_t *samples, int frames);

void sonix_audio_close(sonix_audio_t *a);

/* ----------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

uint64_t sonix_time_us(void);
void sonix_sleep_us(uint64_t us);

/* ----------------------------------------------------------------------------
 * A little drawing, for an app that draws its own controls
 * ------------------------------------------------------------------------- */

static inline uint16_t sonix_rgb(uint8_t r, uint8_t g, uint8_t b) {
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void sonix_fill_rect(uint16_t *frame, int fw, int fh, int x, int y, int w, int h, uint16_t color);
void sonix_round_rect(uint16_t *frame, int fw, int fh, int x, int y, int w, int h, int r, uint16_t color);
void sonix_circle(uint16_t *frame, int fw, int fh, int cx, int cy, int r, uint16_t color);

/* Text in a built-in 5x7 face: capitals, digits and a few marks (lower case is
 * drawn as capitals). Each pixel is `scale` pixels square. */
void sonix_draw_text(uint16_t *frame, int fw, int fh, int x, int y, const char *text, uint16_t color, int scale);
int sonix_text_width(const char *text, int scale);

#ifdef __cplusplus
}
#endif

#endif /* SONIXAPP_H */

/* ============================================================================
 * Implementation
 * ========================================================================= */

#ifdef SONIXAPP_IMPLEMENTATION
#ifndef SONIXAPP_IMPLEMENTED
#define SONIXAPP_IMPLEMENTED

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- environment ---- */

static const char *sonix_getenv_(const char *name, const char *fallback) {
	const char *v = getenv(name);
	return (v && v[0]) ? v : fallback;
}

void sonix_env(sonix_env_t *env) {
	memset(env, 0, sizeof(*env));
	env->under_player = getenv("SONIX_API") != NULL;
	env->width = atoi(sonix_getenv_("SONIX_WIDTH", "480"));
	env->height = atoi(sonix_getenv_("SONIX_HEIGHT", "720"));
	env->rotated = atoi(sonix_getenv_("SONIX_ROTATED", "0")) != 0;
	env->app_dir = sonix_getenv_("SONIX_APP_DIR", ".");
	env->sd = sonix_getenv_("SONIX_SD", ".");
	env->rom = sonix_getenv_("SONIX_ROM", NULL);
	env->fb_path = sonix_getenv_("SONIX_FB", "/dev/fb0");
	env->touch_path = sonix_getenv_("SONIX_TOUCH", "");
	env->audio_fd = atoi(sonix_getenv_("SONIX_AUDIO_FD", "-1"));
	if (env->width <= 0) {
		env->width = 480;
	}
	if (env->height <= 0) {
		env->height = 720;
	}
	if (env->under_player && chdir(env->app_dir) != 0) {
		/* the player has already done it; nothing to add */
	}
}

/* ---- time ---- */

uint64_t sonix_time_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void sonix_sleep_us(uint64_t us) {
	struct timespec ts;
	ts.tv_sec = (time_t)(us / 1000000u);
	ts.tv_nsec = (long)(us % 1000000u) * 1000;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

/* ---- video ---- */

struct sonix_video {
	int fd;
	uint8_t *mem;
	size_t mem_len;
	int w;
	int h;
	int line;	  /* bytes per row of the framebuffer */
	int pages;	  /* how many screens tall the memory is */
	int cur;	  /* the page on show */
	bool rotated;
	bool file_mode;
	uint16_t *frame;
	struct fb_var_screeninfo var;
};

sonix_video_t *sonix_video_open(const sonix_env_t *env) {
	sonix_video_t *v = calloc(1, sizeof(*v));
	if (!v) {
		return NULL;
	}
	v->fd = -1;
	v->rotated = env->rotated;

	struct stat st;
	bool is_device = stat(env->fb_path, &st) == 0 && S_ISCHR(st.st_mode);
	if (!is_device) {
		/* A plain file stands in for the panel, for testing on a computer. */
		v->file_mode = true;
		v->w = env->width;
		v->h = env->height;
		v->line = v->w * 2;
		v->pages = 1;
		v->mem_len = (size_t)v->line * v->h;
		v->fd = open(env->fb_path, O_RDWR | O_CREAT, 0644);
		if (v->fd < 0 || ftruncate(v->fd, (off_t)v->mem_len) != 0) {
			goto fail;
		}
		v->mem = mmap(NULL, v->mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, v->fd, 0);
		if (v->mem == MAP_FAILED) {
			v->mem = NULL;
			goto fail;
		}
	} else {
		struct fb_fix_screeninfo fix;
		v->fd = open(env->fb_path, O_RDWR);
		if (v->fd < 0 || ioctl(v->fd, FBIOGET_VSCREENINFO, &v->var) < 0 || ioctl(v->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
			goto fail;
		}
		if (v->var.bits_per_pixel != 16) {
			fprintf(stderr, "sonixapp: the framebuffer is %u bits a pixel, only 16 is supported\n",
					v->var.bits_per_pixel);
			goto fail;
		}
		v->w = (int)v->var.xres;
		v->h = (int)v->var.yres;
		v->line = (int)fix.line_length;
		v->pages = v->var.yres_virtual >= v->var.yres * 2 ? (int)(v->var.yres_virtual / v->var.yres) : 1;
		v->cur = v->var.yres ? (int)(v->var.yoffset / v->var.yres) : 0;
		if (v->cur >= v->pages) {
			v->cur = 0;
		}
		v->mem_len = fix.smem_len;
		v->mem = mmap(NULL, v->mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, v->fd, 0);
		if (v->mem == MAP_FAILED) {
			v->mem = NULL;
			goto fail;
		}
	}
	v->frame = calloc((size_t)v->w * v->h, sizeof(uint16_t));
	if (!v->frame) {
		goto fail;
	}
	return v;

fail:
	if (v->mem) {
		munmap(v->mem, v->mem_len);
	}
	if (v->fd >= 0) {
		close(v->fd);
	}
	free(v);
	return NULL;
}

int sonix_video_width(const sonix_video_t *v) { return v->w; }
int sonix_video_height(const sonix_video_t *v) { return v->h; }
uint16_t *sonix_video_frame(sonix_video_t *v) { return v->frame; }

void sonix_video_present(sonix_video_t *v) {
	int next = v->pages >= 2 ? (v->cur + 1) % v->pages : 0;
	uint8_t *page = v->mem + (size_t)next * v->h * v->line;
	if (!v->rotated) {
		for (int y = 0; y < v->h; y++) {
			memcpy(page + (size_t)y * v->line, v->frame + (size_t)y * v->w, (size_t)v->w * 2);
		}
	} else {
		for (int y = 0; y < v->h; y++) {
			const uint16_t *src = v->frame + (size_t)(v->h - 1 - y) * v->w;
			uint16_t *dst = (uint16_t *)(page + (size_t)y * v->line);
			for (int x = 0; x < v->w; x++) {
				dst[x] = src[v->w - 1 - x];
			}
		}
	}
	if (v->file_mode) {
		msync(v->mem, v->mem_len, MS_ASYNC);
		return;
	}
	if (v->pages >= 2) {
		v->var.yoffset = (uint32_t)next * (uint32_t)v->h;
		v->var.activate = FB_ACTIVATE_VBL;
		if (ioctl(v->fd, FBIOPAN_DISPLAY, &v->var) == 0) {
			v->cur = next;
		}
	}
}

void sonix_video_close(sonix_video_t *v) {
	if (!v) {
		return;
	}
	if (v->mem) {
		munmap(v->mem, v->mem_len);
	}
	if (v->fd >= 0) {
		close(v->fd);
	}
	free(v->frame);
	free(v);
}

/* ---- touch ---- */

/* The touch drivers of the two players differ, and this follows what the player's
 * own readers learned about them:
 *
 *  - Type A (the R3 Pro II's gt9xx): contacts come one after another, each closed
 *    by SYN_MT_REPORT, and the packet by SYN_REPORT. No identity.
 *  - Type B: numbered slots, ABS_MT_TRACKING_ID of -1 for a contact that has gone.
 *  - The R1's cst8xx ("hyn_ts") reports each finger on every other scan only, so
 *    a finger is kept by its tracking id until it has been missing from two
 *    packets in a row.
 *  - Positions come from ABS_X / ABS_Y, which on both are in pixels of the glass;
 *    the multitouch axes declare a range the chip does not use.
 */

typedef struct {
	int x;
	int y;
	int id;
	bool used;
} sonix_contact_;

typedef struct {
	int x;
	int y;
	int missed;
	bool used;
} sonix_held_;

struct sonix_touch {
	int fd;
	int w;
	int h;
	bool rotated;
	int min_x, max_x, min_y, max_y;
	bool hold_missing;

	sonix_contact_ slots[SONIX_MAX_TOUCH];
	sonix_contact_ pending[SONIX_MAX_TOUCH];
	int pending_count;
	int pending_x, pending_y, pending_id;
	sonix_held_ held[SONIX_MAX_TOUCH];
	int cur_slot;
	bool protocol_a;
	bool protocol_b;
	bool btn_down;
	bool saw_btn;
	sonix_touches_t state;
};

static int sonix_scale_(int value, int lo, int hi, int size) {
	if (hi <= lo) {
		return 0;
	}
	long long v = (long long)(value - lo) * (size - 1) / (hi - lo);
	if (v < 0) {
		v = 0;
	}
	if (v > size - 1) {
		v = size - 1;
	}
	return (int)v;
}

static void sonix_add_point_(sonix_touch_t *t, sonix_touches_t *out, int raw_x, int raw_y) {
	if (out->count >= SONIX_MAX_TOUCH) {
		return;
	}
	int x = sonix_scale_(raw_x, t->min_x, t->max_x, t->w);
	int y = sonix_scale_(raw_y, t->min_y, t->max_y, t->h);
	if (t->rotated) {
		x = t->w - 1 - x;
		y = t->h - 1 - y;
	}
	out->x[out->count] = x;
	out->y[out->count] = y;
	out->count++;
}

sonix_touch_t *sonix_touch_open(const sonix_env_t *env) {
	if (!env->touch_path || !env->touch_path[0]) {
		return NULL;
	}
	int fd = open(env->touch_path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		return NULL;
	}
	sonix_touch_t *t = calloc(1, sizeof(*t));
	if (!t) {
		close(fd);
		return NULL;
	}
	t->fd = fd;
	t->w = env->width;
	t->h = env->height;
	t->rotated = env->rotated;
	t->min_x = 0;
	t->max_x = t->w - 1;
	t->min_y = 0;
	t->max_y = t->h - 1;
	t->btn_down = true;
	t->pending_x = t->pending_y = t->pending_id = -1;

	struct input_absinfo info;
	if (ioctl(fd, EVIOCGABS(ABS_X), &info) == 0 && info.maximum > info.minimum) {
		t->min_x = info.minimum;
		t->max_x = info.maximum;
	}
	if (ioctl(fd, EVIOCGABS(ABS_Y), &info) == 0 && info.maximum > info.minimum) {
		t->min_y = info.minimum;
		t->max_y = info.maximum;
	}
	char name[64] = "";
	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
		name[0] = '\0';
	}
	t->hold_missing = strcmp(name, "hyn_ts") == 0;
	return t;
}

int sonix_touch_fd(const sonix_touch_t *t) { return t->fd; }

static void sonix_touch_packet_(sonix_touch_t *t) {
	sonix_touches_t out;
	out.count = 0;

	if (t->protocol_b) {
		for (int s = 0; s < SONIX_MAX_TOUCH; s++) {
			if (t->slots[s].used) {
				sonix_add_point_(t, &out, t->slots[s].x, t->slots[s].y);
			}
		}
	} else if (t->protocol_a) {
		if (t->hold_missing) {
			for (int h = 0; h < SONIX_MAX_TOUCH; h++) {
				t->held[h].missed++;
			}
			for (int s = 0; s < t->pending_count; s++) {
				int id = t->pending[s].id;
				if (id < 0 || id >= SONIX_MAX_TOUCH) {
					sonix_add_point_(t, &out, t->pending[s].x, t->pending[s].y);
					continue;
				}
				t->held[id].x = t->pending[s].x;
				t->held[id].y = t->pending[s].y;
				t->held[id].missed = 0;
				t->held[id].used = true;
			}
			for (int h = 0; h < SONIX_MAX_TOUCH; h++) {
				if (t->held[h].used && t->held[h].missed > 1) {
					t->held[h].used = false;
				}
				if (t->held[h].used) {
					sonix_add_point_(t, &out, t->held[h].x, t->held[h].y);
				}
			}
		} else {
			for (int s = 0; s < t->pending_count; s++) {
				sonix_add_point_(t, &out, t->pending[s].x, t->pending[s].y);
			}
		}
	} else {
		return; /* the protocol is not known yet: nothing to say */
	}

	/* The driver's own word that the glass is clear outranks the rest. */
	if (t->saw_btn && !t->btn_down) {
		out.count = 0;
		memset(t->held, 0, sizeof(t->held));
	}
	t->state = out;
	t->pending_count = 0;
	t->pending_x = t->pending_y = t->pending_id = -1;
}

bool sonix_touch_read(sonix_touch_t *t, sonix_touches_t *out) {
	bool any = false;
	struct input_event ev[64];
	for (;;) {
		ssize_t got = read(t->fd, ev, sizeof(ev));
		if (got < (ssize_t)sizeof(ev[0])) {
			break; /* nothing more now (EAGAIN), or the node is gone */
		}
		int count = (int)(got / (ssize_t)sizeof(ev[0]));
		for (int i = 0; i < count; i++) {
			const struct input_event *e = &ev[i];
			if (e->type == EV_ABS) {
				switch (e->code) {
				case ABS_MT_SLOT:
					t->protocol_b = true;
					t->cur_slot = e->value;
					if (t->cur_slot < 0 || t->cur_slot >= SONIX_MAX_TOUCH) {
						t->cur_slot = 0;
					}
					break;
				case ABS_MT_TRACKING_ID:
					if (t->protocol_b) {
						t->slots[t->cur_slot].used = e->value >= 0;
					} else {
						t->pending_id = e->value;
					}
					break;
				case ABS_MT_POSITION_X:
					if (t->protocol_b) {
						t->slots[t->cur_slot].x = e->value;
						t->slots[t->cur_slot].used = true;
					} else {
						t->pending_x = e->value;
					}
					break;
				case ABS_MT_POSITION_Y:
					if (t->protocol_b) {
						t->slots[t->cur_slot].y = e->value;
						t->slots[t->cur_slot].used = true;
					} else {
						t->pending_y = e->value;
					}
					break;
				default:
					break;
				}
			} else if (e->type == EV_KEY && e->code == BTN_TOUCH) {
				t->saw_btn = true;
				t->btn_down = e->value != 0;
			} else if (e->type == EV_SYN) {
				if (e->code == SYN_MT_REPORT) {
					t->protocol_a = true;
					if (t->pending_x >= 0 && t->pending_y >= 0 && t->pending_count < SONIX_MAX_TOUCH) {
						t->pending[t->pending_count].x = t->pending_x;
						t->pending[t->pending_count].y = t->pending_y;
						t->pending[t->pending_count].id = t->pending_id;
						t->pending[t->pending_count].used = true;
						t->pending_count++;
					}
					t->pending_x = t->pending_y = t->pending_id = -1;
				} else if (e->code == SYN_REPORT) {
					sonix_touch_packet_(t);
					any = true;
				}
			}
		}
	}
	*out = t->state;
	return any;
}

void sonix_touch_close(sonix_touch_t *t) {
	if (!t) {
		return;
	}
	close(t->fd);
	free(t);
}

/* ---- audio ---- */

struct sonix_audio {
	int fd;
	int channels;
};

sonix_audio_t *sonix_audio_open(const sonix_env_t *env, int rate, int channels) {
	if (env->audio_fd < 0 || channels < 1 || channels > 2) {
		return NULL;
	}
	sonix_audio_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	a->fd = env->audio_fd;
	a->channels = channels;
	uint8_t header[12];
	memcpy(header, "SNXA", 4);
	uint32_t r = (uint32_t)rate;
	uint32_t c = (uint32_t)channels;
	memcpy(header + 4, &r, 4);
	memcpy(header + 8, &c, 4);
	if (write(a->fd, header, sizeof(header)) != (ssize_t)sizeof(header)) {
		free(a);
		return NULL;
	}
	return a;
}

bool sonix_audio_write(sonix_audio_t *a, const int16_t *samples, int frames) {
	const uint8_t *p = (const uint8_t *)samples;
	size_t left = (size_t)frames * (size_t)a->channels * sizeof(int16_t);
	while (left > 0) {
		ssize_t w = write(a->fd, p, left);
		if (w < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		p += w;
		left -= (size_t)w;
	}
	return true;
}

void sonix_audio_close(sonix_audio_t *a) {
	if (a) {
		close(a->fd);
		free(a);
	}
}

/* ---- drawing ---- */

void sonix_fill_rect(uint16_t *frame, int fw, int fh, int x, int y, int w, int h, uint16_t color) {
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w > fw ? fw : x + w;
	int y1 = y + h > fh ? fh : y + h;
	for (int yy = y0; yy < y1; yy++) {
		uint16_t *row = frame + (size_t)yy * fw;
		for (int xx = x0; xx < x1; xx++) {
			row[xx] = color;
		}
	}
}

void sonix_round_rect(uint16_t *frame, int fw, int fh, int x, int y, int w, int h, int r, uint16_t color) {
	if (r * 2 > w) {
		r = w / 2;
	}
	if (r * 2 > h) {
		r = h / 2;
	}
	for (int yy = 0; yy < h; yy++) {
		int py = y + yy;
		if (py < 0 || py >= fh) {
			continue;
		}
		int inset = 0;
		int dy = -1;
		if (yy < r) {
			dy = r - yy - 1;
		} else if (yy >= h - r) {
			dy = yy - (h - r);
		}
		if (dy >= 0) {
			for (inset = 0; inset < r; inset++) {
				int dx = r - inset - 1;
				if (dx * dx + dy * dy <= r * r) {
					break;
				}
			}
		}
		sonix_fill_rect(frame, fw, fh, x + inset, py, w - 2 * inset, 1, color);
	}
}

void sonix_circle(uint16_t *frame, int fw, int fh, int cx, int cy, int r, uint16_t color) {
	sonix_round_rect(frame, fw, fh, cx - r, cy - r, r * 2, r * 2, r, color);
}

static const uint8_t sonix_font_[128][7] = {
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
	{0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
	{0x00, 0x11, 0x0a, 0x1f, 0x0a, 0x11, 0x00},
	{0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08},
	{0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c},
	{0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},
	{0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
	{0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
	{0x0e, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0e},
	{0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
	{0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
	{0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},
	{0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
	{0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
	{0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
	{0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
	{0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00},
	{0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
	{0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e},
	{0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e},
	{0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e},
	{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f},
	{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10},
	{0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e},
	{0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
	{0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e},
	{0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c},
	{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
	{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f},
	{0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11},
	{0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
	{0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},
	{0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10},
	{0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d},
	{0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11},
	{0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e},
	{0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
	{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},
	{0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04},
	{0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11},
	{0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11},
	{0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04},
	{0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
	{0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e},
	{0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e},
	{0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e},
	{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f},
	{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10},
	{0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e},
	{0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
	{0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e},
	{0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c},
	{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
	{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f},
	{0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11},
	{0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
	{0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},
	{0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10},
	{0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d},
	{0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11},
	{0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e},
	{0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
	{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},
	{0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04},
	{0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11},
	{0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11},
	{0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04},
	{0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

int sonix_text_width(const char *text, int scale) {
	int n = (int)strlen(text);
	return n > 0 ? (n * 6 - 1) * scale : 0;
}

void sonix_draw_text(uint16_t *frame, int fw, int fh, int x, int y, const char *text, uint16_t color, int scale) {
	for (const char *p = text; *p; p++, x += 6 * scale) {
		const uint8_t *glyph = sonix_font_[(unsigned char)*p & 127];
		for (int row = 0; row < 7; row++) {
			for (int col = 0; col < 5; col++) {
				if (glyph[row] & (0x10 >> col)) {
					sonix_fill_rect(frame, fw, fh, x + col * scale, y + row * scale, scale, scale, color);
				}
			}
		}
	}
}

#endif /* SONIXAPP_IMPLEMENTED */
#endif /* SONIXAPP_IMPLEMENTATION */
