#ifndef WAT_WAV_H
#define WAT_WAV_H

/*
 * WAV/RIFF format constants. Public so tests and the fuzz harness can
 * generate valid headers without re-deriving the magic numbers.
 */
#define WAV_HEADER_SIZE     12          /* "RIFF" <size> "WAVE" */
#define WAV_CHUNK_HDR_SIZE   8          /* 4-byte id + 4-byte size */
#define WAV_FMT_MIN_SIZE    16          /* PCM fmt chunk payload */
#define WAV_FMT_PCM          1          /* WAVE_FORMAT_PCM */
#define WAV_PCM_BITS        16          /* only depth we accept */
#define WAV_BYTES_PER_S16   (WAV_PCM_BITS / 8)
#define WAV_MAX_CHANNELS     8
#define WAV_S16_SCALE       32768.0f    /* signed 16-bit full-scale */

/* Memory cap on a single load: 30 minutes at 16kHz mono. */
#define WAV_MAX_SAMPLES (16000 * 60 * 30)

/*
 * Load a 16-bit PCM WAV file as float32 mono samples in [-1, 1].
 *
 * On success: returns malloc'd buffer of *n samples (caller frees).
 *             *out_rate is set to the file's sample rate.
 * On failure: returns NULL, prints diagnostic to stderr, *n = 0.
 *
 * Multi-channel files are mixed down to mono.
 * Caps total samples at WAV_MAX_SAMPLES to bound memory.
 */
float *wav_load(const char *path, int *n, int *out_rate);

#endif
