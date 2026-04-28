#ifndef WAT_AUDIO_H
#define WAT_AUDIO_H

typedef struct audio_ctx audio_ctx;

/* Open the default capture device. Returns NULL on failure. */
audio_ctx *audio_open(int sample_rate, int channels);

/* Block until n_samples are available. Returns samples read, or -1 on error. */
int audio_read(audio_ctx *ctx, float *buf, int n_samples);

/* Stop capture and release resources. */
void audio_close(audio_ctx *ctx);

#endif
