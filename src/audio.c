#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

enum {
	RING_SECONDS = 30,
};

/* Wake every AUDIO_POLL_NS so a Ctrl+C check happens between blocks. */
#define WAT_NS_PER_SEC   1000000000L
#define AUDIO_POLL_NS    100000000L   /* 100ms */

struct audio_ctx {
	ma_device       device;
	float          *ring;
	int             cap;
	int             head;
	int             tail;
	int             count;
	pthread_mutex_t mtx;
	pthread_cond_t  cond;
};

static void ring_write(audio_ctx *ctx, const float *src, int n)
{
	while (n > 0 && ctx->count < ctx->cap) {
		int span = ctx->cap - ctx->head;
		int avail = ctx->cap - ctx->count;
		int take = n < span ? n : span;
		if (take > avail) take = avail;
		memcpy(ctx->ring + ctx->head, src, take * sizeof(float));
		ctx->head = (ctx->head + take) % ctx->cap;
		ctx->count += take;
		src += take;
		n -= take;
	}
}

static int ring_read(audio_ctx *ctx, float *dst, int n)
{
	int got = 0;
	while (got < n && ctx->count > 0) {
		int span = ctx->cap - ctx->tail;
		int take = n - got;
		if (take > ctx->count) take = ctx->count;
		if (take > span) take = span;
		memcpy(dst + got, ctx->ring + ctx->tail, take * sizeof(float));
		ctx->tail = (ctx->tail + take) % ctx->cap;
		ctx->count -= take;
		got += take;
	}
	return got;
}

static void capture_cb(ma_device *dev, void *out, const void *in,
                        ma_uint32 frames)
{
	(void)out;
	audio_ctx *ctx = (audio_ctx *)dev->pUserData;

	pthread_mutex_lock(&ctx->mtx);
	ring_write(ctx, (const float *)in, (int)frames);
	pthread_cond_signal(&ctx->cond);
	pthread_mutex_unlock(&ctx->mtx);
}

audio_ctx *audio_open(int sample_rate, int channels)
{
	audio_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return NULL;

	ctx->cap = sample_rate * RING_SECONDS;
	ctx->ring = malloc(ctx->cap * sizeof(float));
	if (!ctx->ring) { free(ctx); return NULL; }

	pthread_mutex_init(&ctx->mtx, NULL);
	pthread_cond_init(&ctx->cond, NULL);

	ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
	cfg.capture.format   = ma_format_f32;
	cfg.capture.channels = channels;
	cfg.sampleRate       = sample_rate;
	cfg.dataCallback     = capture_cb;
	cfg.pUserData        = ctx;

	if (ma_device_init(NULL, &cfg, &ctx->device) != MA_SUCCESS)
		goto fail;
	if (ma_device_start(&ctx->device) != MA_SUCCESS) {
		ma_device_uninit(&ctx->device);
		goto fail;
	}
	return ctx;

fail:
	pthread_mutex_destroy(&ctx->mtx);
	pthread_cond_destroy(&ctx->cond);
	free(ctx->ring);
	free(ctx);
	return NULL;
}

int audio_read(audio_ctx *ctx, float *buf, int n)
{
	int got = 0;
	pthread_mutex_lock(&ctx->mtx);
	while (got < n) {
		if (ctx->count > 0) {
			got += ring_read(ctx, buf + got, n - got);
			continue;
		}
		/*
		 * Timed wait: wake every AUDIO_POLL_NS and return whatever
		 * we have. This lets the caller check quit_flag after
		 * Ctrl+C instead of blocking forever.
		 */
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += AUDIO_POLL_NS;
		if (ts.tv_nsec >= WAT_NS_PER_SEC) {
			ts.tv_sec++;
			ts.tv_nsec -= WAT_NS_PER_SEC;
		}
		pthread_cond_timedwait(&ctx->cond, &ctx->mtx, &ts);
		if (ctx->count == 0)
			break;  /* timeout with no new data: return short */
	}
	pthread_mutex_unlock(&ctx->mtx);
	return got;
}

void audio_close(audio_ctx *ctx)
{
	if (!ctx) return;
	ma_device_uninit(&ctx->device);
	pthread_mutex_destroy(&ctx->mtx);
	pthread_cond_destroy(&ctx->cond);
	free(ctx->ring);
	free(ctx);
}
