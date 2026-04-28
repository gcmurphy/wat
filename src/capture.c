#include "capture.h"
#include "audio.h"
#include "tty.h"
#include "util.h"
#include "vad.h"

#include "whisper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
	INITIAL_CAPTURE_SECONDS = 30,    /* growable; 30s = ~1.9MB at 16kHz */
	METER_WIDTH             = 40,
	METER_SCALE             = 500,   /* full-scale at ~0.08 RMS */
};

/* PTT key-poll interval. 50ms feels responsive without burning CPU. */
#define PTT_POLL_NS 50000000L

/*
 * Render a live VU meter on stderr.
 *   level     : current RMS energy
 *   threshold : VAD threshold (drawn as a separator); 0 to omit
 *   state     : "waiting"/"SPEECH"/"silent", or NULL to omit
 *
 * The meter scale is empirical: METER_WIDTH / METER_SCALE = full scale
 * at ~0.08 RMS, which matches typical microphone gain for normal speech.
 */
static void draw_meter(float level, float threshold, const char *state)
{
	int bar = (int)(level * METER_SCALE);
	int thr = (int)(threshold * METER_SCALE);
	if (bar > METER_WIDTH) bar = METER_WIDTH;
	if (thr > METER_WIDTH) thr = METER_WIDTH;
	fprintf(stderr, "\rwat: [");
	for (int i = 0; i < METER_WIDTH; i++)
		fputc(i < bar ? '#' : (i == thr ? '|' : '.'), stderr);
	fprintf(stderr, "] %.4f %s  ", level, state ? state : "");
}

static int transcribe(struct whisper_context *ctx,
                      struct whisper_full_params *wp,
                      const float *pcm, int n)
{
	if (whisper_full(ctx, *wp, pcm, n) != 0)
		return -1;
	int segs = whisper_full_n_segments(ctx);
	for (int i = 0; i < segs; i++)
		fputs(whisper_full_get_segment_text(ctx, i), stdout);
	return 0;
}

float *capture_vad(int verbose, float vad_thold, int silence_ms, int *n)
{
	audio_ctx *ac = audio_open(WAT_SAMPLE_RATE, 1);
	if (!ac) {
		fprintf(stderr, "wat: cannot open audio device\n");
		return NULL;
	}

	int cap = WAT_SAMPLE_RATE * INITIAL_CAPTURE_SECONDS;
	float *buf = xmalloc((size_t)cap * sizeof(float));
	int len = 0, speech = 0, silent = 0;
	int max_silent = silence_ms / WAT_FRAME_MS;
	float frame[WAT_FRAME_SAMPLES];

	if (verbose) fprintf(stderr, "wat: listening...\n");

	while (!quit_flag) {
		if (audio_read(ac, frame, WAT_FRAME_SAMPLES) != WAT_FRAME_SAMPLES)
			break;

		float e = rms_energy(frame, WAT_FRAME_SAMPLES);

		if (verbose) {
			const char *st = !speech         ? "waiting" :
			                 (e > vad_thold) ? "SPEECH"  : "silent";
			draw_meter(e, vad_thold, st);
		}

		if (e > vad_thold) {
			speech = 1;
			silent = 0;
		} else if (speech && ++silent >= max_silent) {
			break;
		}

		if (len + WAT_FRAME_SAMPLES > cap) {
			cap *= 2;
			buf = xrealloc(buf, (size_t)cap * sizeof(float));
		}
		memcpy(buf + len, frame, WAT_FRAME_SAMPLES * sizeof(float));
		len += WAT_FRAME_SAMPLES;
	}

	if (verbose) {
		fputc('\n', stderr);
		fprintf(stderr, "wat: captured %.1fs\n",
		        (float)len / WAT_SAMPLE_RATE);
	}

	audio_close(ac);
	*n = len;
	return buf;
}

float *capture_ptt(int verbose, int *n)
{
	if (tty_raw() < 0) {
		fprintf(stderr, "wat: push-to-talk requires a TTY on stdin\n");
		return NULL;
	}

	fprintf(stderr, "press any key to record, again to stop (ctrl+c to abort)\n");

	/* wait for start keypress (or quit) */
	while (!quit_flag) {
		int k = tty_peek();
		if (k >= 0) break;
		struct timespec ts = { .tv_sec = 0, .tv_nsec = PTT_POLL_NS };
		nanosleep(&ts, NULL);
	}
	if (quit_flag) { tty_restore(); *n = 0; return NULL; }

	audio_ctx *ac = audio_open(WAT_SAMPLE_RATE, 1);
	if (!ac) {
		fprintf(stderr, "wat: cannot open audio device\n");
		tty_restore();
		return NULL;
	}

	fprintf(stderr, "recording... press any key to stop\n");

	int cap = WAT_SAMPLE_RATE * INITIAL_CAPTURE_SECONDS;
	float *buf = xmalloc((size_t)cap * sizeof(float));
	int len = 0;
	float frame[WAT_FRAME_SAMPLES];

	while (!quit_flag) {
		if (audio_read(ac, frame, WAT_FRAME_SAMPLES) != WAT_FRAME_SAMPLES)
			break;

		if (verbose)
			draw_meter(rms_energy(frame, WAT_FRAME_SAMPLES), 0, NULL);

		if (len + WAT_FRAME_SAMPLES > cap) {
			cap *= 2;
			buf = xrealloc(buf, (size_t)cap * sizeof(float));
		}
		memcpy(buf + len, frame, WAT_FRAME_SAMPLES * sizeof(float));
		len += WAT_FRAME_SAMPLES;

		/* check for stop keypress */
		if (tty_peek() >= 0) break;
	}

	if (verbose) fputc('\n', stderr);
	audio_close(ac);
	tty_restore();

	if (verbose)
		fprintf(stderr, "wat: captured %.1fs\n",
		        (float)len / WAT_SAMPLE_RATE);

	*n = len;
	return buf;
}

int capture_stream(struct whisper_context *ctx,
                   struct whisper_full_params *wp, int verbose)
{
	audio_ctx *ac = audio_open(WAT_SAMPLE_RATE, 1);
	if (!ac) {
		fprintf(stderr, "wat: cannot open audio device\n");
		return 1;
	}

	float *window = xmalloc(WAT_STREAM_LEN * sizeof(float));
	float *step   = xmalloc(WAT_STREAM_STEP * sizeof(float));
	int filled = 0;

	memset(window, 0, WAT_STREAM_LEN * sizeof(float));
	if (verbose) fprintf(stderr, "wat: streaming (ctrl+c to stop)\n");

	while (!quit_flag) {
		if (audio_read(ac, step, WAT_STREAM_STEP) != WAT_STREAM_STEP)
			break;

		if (filled < WAT_STREAM_LEN) {
			int take = WAT_STREAM_LEN - filled;
			if (take > WAT_STREAM_STEP) take = WAT_STREAM_STEP;
			memcpy(window + filled, step, take * sizeof(float));
			filled += take;
			if (filled < WAT_STREAM_LEN)
				continue;
		} else {
			memmove(window, window + WAT_STREAM_STEP,
			        (WAT_STREAM_LEN - WAT_STREAM_STEP) * sizeof(float));
			memcpy(window + WAT_STREAM_LEN - WAT_STREAM_STEP,
			       step, WAT_STREAM_STEP * sizeof(float));
		}

		if (transcribe(ctx, wp, window, filled) != 0) {
			fprintf(stderr, "wat: transcription failed\n");
			break;
		}
		fputc('\n', stdout);
		fflush(stdout);
	}

	free(step);
	free(window);
	audio_close(ac);
	return 0;
}
