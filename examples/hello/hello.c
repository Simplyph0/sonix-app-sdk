/* hello: the smallest app that uses everything - a screen, touch and sound.
 *
 * It shows colour bars, a label, a circle under every finger, and plays a tone
 * while the screen is touched. The EXIT key in the corner ends it.
 *
 *     hello            runs until EXIT is touched
 *     hello --frames N draws N frames and stops: for testing off the player
 */
#define SONIXAPP_IMPLEMENTATION
#include "sonixapp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100
#define FPS 60

int main(int argc, char **argv) {
	int max_frames = 0;
	for (int i = 1; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--frames")) {
			max_frames = atoi(argv[i + 1]);
		}
	}

	sonix_env_t env;
	sonix_env(&env);
	sonix_video_t *video = sonix_video_open(&env);
	if (!video) {
		fprintf(stderr, "hello: no framebuffer\n");
		return 1;
	}
	sonix_touch_t *touch = sonix_touch_open(&env);
	sonix_audio_t *audio = sonix_audio_open(&env, RATE, 2);

	int w = sonix_video_width(video);
	int h = sonix_video_height(video);
	uint16_t *frame = sonix_video_frame(video);
	const int exit_w = 120;
	const int exit_h = 56;

	sonix_touches_t fingers;
	memset(&fingers, 0, sizeof(fingers));
	float phase = 0;
	int16_t sound[RATE / FPS * 2];
	uint64_t next = sonix_time_us();

	for (int n = 0; max_frames == 0 || n < max_frames; n++) {
		if (touch) {
			sonix_touch_read(touch, &fingers);
		}
		bool leave = false;
		for (int i = 0; i < fingers.count; i++) {
			if (fingers.x[i] >= w - exit_w && fingers.y[i] < exit_h) {
				leave = true;
			}
		}
		if (leave) {
			break;
		}

		/* colour bars */
		static const uint16_t bars[6] = {0xF800, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F};
		for (int i = 0; i < 6; i++) {
			sonix_fill_rect(frame, w, h, i * w / 6, 0, w / 6 + 1, h, bars[i]);
		}
		sonix_fill_rect(frame, w, h, 0, h / 2 - 60, w, 120, sonix_rgb(16, 16, 24));
		sonix_draw_text(frame, w, h, (w - sonix_text_width("HELLO FROM AN APP", 4)) / 2, h / 2 - 14,
						"HELLO FROM AN APP", sonix_rgb(255, 255, 255), 4);

		/* EXIT */
		sonix_round_rect(frame, w, h, w - exit_w, 0, exit_w, exit_h, 12, sonix_rgb(200, 40, 40));
		sonix_draw_text(frame, w, h, w - exit_w + (exit_w - sonix_text_width("EXIT", 3)) / 2, 18, "EXIT",
						sonix_rgb(255, 255, 255), 3);

		for (int i = 0; i < fingers.count; i++) {
			sonix_circle(frame, w, h, fingers.x[i], fingers.y[i], 40, sonix_rgb(255, 255, 255));
		}
		sonix_video_present(video);

		/* a tone while there is a finger on the glass; silence otherwise */
		int frames = RATE / FPS;
		for (int i = 0; i < frames; i++) {
			float s = fingers.count > 0 ? sinf(phase) * 6000.0f : 0.0f;
			phase += 2.0f * 3.14159265f * 440.0f / RATE;
			if (phase > 6.2831853f) {
				phase -= 6.2831853f;
			}
			sound[i * 2] = sound[i * 2 + 1] = (int16_t)s;
		}
		if (audio) {
			sonix_audio_write(audio, sound, frames); /* the sound card sets the pace */
		} else {
			next += 1000000 / FPS;
			uint64_t now = sonix_time_us();
			if (next > now) {
				sonix_sleep_us(next - now);
			}
		}
	}

	sonix_audio_close(audio);
	sonix_touch_close(touch);
	sonix_video_close(video);
	return 0;
}
