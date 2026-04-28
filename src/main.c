#include "capture.h"
#include "util.h"
#include "wav.h"
#include "whisper.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* CLI bounds and defaults — single source of truth for option validation. */
enum {
	WAT_THREADS_DEFAULT = 4,
	WAT_THREADS_MIN     = 1,
	WAT_THREADS_MAX     = 64,
	WAT_SILENCE_DEFAULT_MS = 1500,
	WAT_SILENCE_MIN_MS  = 100,
	WAT_SILENCE_MAX_MS  = 60000,
	WAT_PATH_MAX        = 4096,
};

#define WAT_VAD_DEFAULT  0.01f
#define WAT_NS_PER_SEC   1000000000L

static int g_verbose;

static void on_sigint(int sig) { (void)sig; quit_flag = 1; }

/*
 * Parse a signed integer. On success writes to *out and returns 1.
 * Rejects non-numeric input, leading garbage, and out-of-range values.
 */
static int parse_int(const char *s, long min, long max, int *out)
{
	if (!s || !*s) return 0;
	char *end;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno || *end || v < min || v > max) return 0;
	*out = (int)v;
	return 1;
}

/* Parse a non-negative float. On failure returns 0 and leaves *out. */
static int parse_float(const char *s, float *out)
{
	if (!s || !*s) return 0;
	char *end;
	errno = 0;
	float v = strtof(s, &end);
	if (errno || *end || v < 0) return 0;
	*out = v;
	return 1;
}

static void whisper_log_cb(enum ggml_log_level level, const char *text,
                           void *user_data)
{
	(void)level;
	(void)user_data;
	if (g_verbose) fputs(text, stderr);
}

static void usage(void)
{
	fprintf(stderr,
	"usage: wat [options] [file.wav]\n"
	"\n"
	"  Transcribe speech to text.\n"
	"  With no file, captures from microphone.\n"
	"\n"
	"  -m PATH   model (default: $WAT_MODEL or ~/.local/share/wat/...)\n"
	"  -l LANG   language (default: en)\n"
	"  -t N      threads (default: %d, range %d-%d)\n"
	"  -s        streaming mode (continuous)\n"
	"  -p        push-to-talk (press key to start/stop)\n"
	"  -T FLOAT  VAD threshold (default: %g)\n"
	"  -S MS     silence before stop (default: %d, range %d-%d)\n"
	"  -v        verbose\n"
	"  -h        help\n",
	WAT_THREADS_DEFAULT, WAT_THREADS_MIN, WAT_THREADS_MAX,
	WAT_VAD_DEFAULT,
	WAT_SILENCE_DEFAULT_MS, WAT_SILENCE_MIN_MS, WAT_SILENCE_MAX_MS);
}

static const char *default_model(void)
{
	static char p[WAT_PATH_MAX];
	const char *xdg = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	if (xdg)
		snprintf(p, sizeof p, "%s/wat/ggml-tiny.en.bin", xdg);
	else if (home)
		snprintf(p, sizeof p, "%s/.local/share/wat/ggml-tiny.en.bin", home);
	else
		snprintf(p, sizeof p, "ggml-tiny.en.bin");
	return p;
}

static double elapsed_since(const struct timespec *t0)
{
	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0->tv_sec) +
	       (t1.tv_nsec - t0->tv_nsec) / (double)WAT_NS_PER_SEC;
}

static int run_transcribe(struct whisper_context *ctx,
                          struct whisper_full_params *wp,
                          const float *pcm, int n, int verbose)
{
	struct timespec t0;
	if (verbose) clock_gettime(CLOCK_MONOTONIC, &t0);

	if (whisper_full(ctx, *wp, pcm, n) != 0) {
		fprintf(stderr, "wat: transcription failed\n");
		return 1;
	}

	int segs = whisper_full_n_segments(ctx);
	for (int i = 0; i < segs; i++)
		fputs(whisper_full_get_segment_text(ctx, i), stdout);
	putchar('\n');

	if (verbose) fprintf(stderr, "wat: %.2fs\n", elapsed_since(&t0));
	return 0;
}

int main(int argc, char **argv)
{
	const char *model_path = getenv("WAT_MODEL");
	const char *lang = "en";
	int n_threads  = WAT_THREADS_DEFAULT;
	int streaming = 0, ptt = 0, verbose = 0;
	float vad_thold = WAT_VAD_DEFAULT;
	int silence_ms = WAT_SILENCE_DEFAULT_MS;
	int opt;

	while ((opt = getopt(argc, argv, "m:l:t:spT:S:vh")) != -1) {
		switch (opt) {
		case 'm': model_path = optarg; break;
		case 'l': lang = optarg; break;
		case 't':
			if (!parse_int(optarg, WAT_THREADS_MIN, WAT_THREADS_MAX,
			               &n_threads)) {
				fprintf(stderr, "wat: -t: expected integer in [%d,%d]\n",
				        WAT_THREADS_MIN, WAT_THREADS_MAX);
				return 1;
			}
			break;
		case 's': streaming = 1; break;
		case 'p': ptt = 1; break;
		case 'T':
			if (!parse_float(optarg, &vad_thold)) {
				fprintf(stderr, "wat: -T: expected non-negative float\n");
				return 1;
			}
			break;
		case 'S':
			if (!parse_int(optarg, WAT_SILENCE_MIN_MS, WAT_SILENCE_MAX_MS,
			               &silence_ms)) {
				fprintf(stderr, "wat: -S: expected ms in [%d,%d]\n",
				        WAT_SILENCE_MIN_MS, WAT_SILENCE_MAX_MS);
				return 1;
			}
			break;
		case 'v': verbose = 1; break;
		case 'h': usage(); return 0;
		default:  usage(); return 1;
		}
	}

	if (streaming && ptt) {
		fprintf(stderr, "wat: -s and -p are mutually exclusive\n");
		return 1;
	}

	if (!model_path) model_path = default_model();

	struct sigaction sa = { .sa_handler = on_sigint };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	g_verbose = verbose;
	whisper_log_set(whisper_log_cb, NULL);

	if (verbose) fprintf(stderr, "wat: loading %s\n", model_path);

	struct whisper_context_params cp = whisper_context_default_params();
	cp.use_gpu = false;

	struct whisper_context *ctx =
		whisper_init_from_file_with_params(model_path, cp);
	if (!ctx) {
		fprintf(stderr, "wat: cannot load model: %s\n", model_path);
		fprintf(stderr, "     run 'make model' to fetch one\n");
		return 1;
	}

	struct whisper_full_params wp =
		whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	wp.n_threads      = n_threads;
	wp.language       = lang;
	wp.no_timestamps  = true;
	wp.print_special  = false;
	wp.print_progress = false;
	wp.print_realtime = false;

	int rc = 0;

	if (optind < argc) {
		int n = 0, rate = 0;
		float *pcm = wav_load(argv[optind], &n, &rate);
		if (!pcm) { rc = 1; goto out; }
		if (rate && rate != WAT_SAMPLE_RATE)
			fprintf(stderr, "wat: warning: %s is %dHz (need %dHz)\n",
			        argv[optind], rate, WAT_SAMPLE_RATE);
		rc = run_transcribe(ctx, &wp, pcm, n, verbose);
		free(pcm);
	} else if (streaming) {
		wp.single_segment = true;
		rc = capture_stream(ctx, &wp, verbose);
	} else {
		int n = 0;
		float *pcm = ptt ? capture_ptt(verbose, &n)
		                 : capture_vad(verbose, vad_thold, silence_ms, &n);
		if (!pcm) { rc = 1; goto out; }
		if (n == 0) {
			fprintf(stderr, "wat: no audio captured\n");
			free(pcm);
			rc = 1;
			goto out;
		}
		rc = run_transcribe(ctx, &wp, pcm, n, verbose);
		free(pcm);
	}

out:
	whisper_free(ctx);
	return rc;
}
