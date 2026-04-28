#ifndef WAT_CAPTURE_H
#define WAT_CAPTURE_H

struct whisper_context;
struct whisper_full_params;

#define WAT_SAMPLE_RATE  16000
#define WAT_FRAME_MS     30
#define WAT_FRAME_SAMPLES (WAT_SAMPLE_RATE * WAT_FRAME_MS / 1000)

/* Streaming window: 10s capture, slide every 2s. */
#define WAT_STREAM_LEN   (WAT_SAMPLE_RATE * 10)
#define WAT_STREAM_STEP  (WAT_SAMPLE_RATE * 2)

/*
 * Capture from microphone with energy-based VAD auto-stop.
 * Records until silence_ms of below-threshold audio follows speech,
 * or until quit_flag is set. Returns malloc'd buffer of *n samples.
 * Returns NULL on device-open failure.
 */
float *capture_vad(int verbose, float vad_thold, int silence_ms, int *n);

/*
 * Push-to-talk capture: wait for keypress to start, press again (or
 * SIGINT) to stop. Requires stdin to be a TTY.
 *   verbose         show VU meter while recording
 *   *n              out: samples captured
 * Returns malloc'd buffer or NULL on failure (non-tty, device error).
 */
float *capture_ptt(int verbose, int *n);

/*
 * Continuous transcription with sliding window.
 * Loops until quit_flag is set. Returns 0 on clean exit, 1 on error.
 */
int capture_stream(struct whisper_context *ctx,
                   struct whisper_full_params *wp, int verbose);

#endif
