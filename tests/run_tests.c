/*
 * Unit tests for wat.
 *
 * Self-contained: builds a few crafted WAV files in /tmp and feeds
 * them through wav_load(). No framework, no external deps.
 *
 *   run_tests               — plain build
 *   run_tests_asan          — ASan + UBSan
 *   run_tests_ubsan         — UBSan only
 */
#include "wav.h"
#include "vad.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed;
static int total;

#define OK(cond) do {                                         \
	total++;                                              \
	if (!(cond)) {                                        \
		fprintf(stdout, "  FAIL %s:%d  %s\n",         \
		        __FILE__, __LINE__, #cond);           \
		failed++;                                     \
	}                                                     \
} while (0)

/* ── WAV test helpers ────────────────────────────────────── */

/*
 * Write a minimal PCM WAV to a tmp file.
 *   rate, channels, bits: format fields
 *   samples_per_channel:  number of frames
 *   pcm:                  n_channels * samples_per_channel int16 samples
 *                         (interleaved), or NULL for silence
 */
static char *write_wav(int rate, int channels, int bits,
                       int samples_per_channel, const int16_t *pcm)
{
	static char path[] = "/tmp/wat_test_XXXXXX.wav";
	strcpy(path, "/tmp/wat_test_XXXXXX.wav");
	int fd = mkstemps(path, 4);
	if (fd < 0) return NULL;
	FILE *f = fdopen(fd, "wb");
	if (!f) return NULL;

	uint32_t data_sz = (uint32_t)samples_per_channel * channels * (bits / 8);
	uint32_t riff_sz = 36 + data_sz;

	fwrite("RIFF", 4, 1, f);
	fwrite(&riff_sz, 4, 1, f);
	fwrite("WAVE", 4, 1, f);

	fwrite("fmt ", 4, 1, f);
	uint32_t fmt_sz = 16; fwrite(&fmt_sz, 4, 1, f);
	uint16_t fmt = 1;     fwrite(&fmt, 2, 1, f);
	uint16_t ch = channels; fwrite(&ch, 2, 1, f);
	uint32_t r = rate;    fwrite(&r, 4, 1, f);
	uint32_t byte_rate = rate * channels * (bits / 8);
	fwrite(&byte_rate, 4, 1, f);
	uint16_t block_align = channels * (bits / 8);
	fwrite(&block_align, 2, 1, f);
	uint16_t b = bits;    fwrite(&b, 2, 1, f);

	fwrite("data", 4, 1, f);
	fwrite(&data_sz, 4, 1, f);
	if (pcm)
		fwrite(pcm, 2, samples_per_channel * channels, f);
	else {
		int16_t zero = 0;
		for (int i = 0; i < samples_per_channel * channels; i++)
			fwrite(&zero, 2, 1, f);
	}
	fclose(f);
	return path;
}

/* ── Tests: VAD ──────────────────────────────────────────── */

static void test_rms_zero(void)
{
	float buf[100] = {0};
	OK(rms_energy(buf, 100) == 0.0f);
}

static void test_rms_ones(void)
{
	float buf[100];
	for (int i = 0; i < 100; i++) buf[i] = 1.0f;
	OK(fabsf(rms_energy(buf, 100) - 1.0f) < 1e-6);
}

static void test_rms_halves(void)
{
	float buf[4] = { 0.5f, -0.5f, 0.5f, -0.5f };
	OK(fabsf(rms_energy(buf, 4) - 0.5f) < 1e-6);
}

static void test_rms_empty(void)
{
	float buf[1];
	OK(rms_energy(buf, 0) == 0.0f);
	OK(rms_energy(buf, -1) == 0.0f);
}

/* ── Tests: WAV loader ───────────────────────────────────── */

static void test_wav_valid_mono(void)
{
	int16_t pcm[4] = { 0, 16384, -16384, 0 };
	char *p = write_wav(16000, 1, 16, 4, pcm);
	int n = 0, rate = 0;
	float *out = wav_load(p, &n, &rate);
	OK(out != NULL);
	OK(n == 4);
	OK(rate == 16000);
	if (out) {
		OK(out[0] == 0.0f);
		OK(fabsf(out[1] - 0.5f) < 0.001f);
		OK(fabsf(out[2] + 0.5f) < 0.001f);
		free(out);
	}
	unlink(p);
}

static void test_wav_stereo_to_mono(void)
{
	/* stereo: [L=1.0, R=-1.0] -> mono 0, [L=0.5, R=0.5] -> mono 0.5 */
	int16_t pcm[4] = { 32767, -32768, 16384, 16384 };
	char *p = write_wav(16000, 2, 16, 2, pcm);
	int n = 0, rate = 0;
	float *out = wav_load(p, &n, &rate);
	OK(out != NULL);
	OK(n == 2);
	if (out) {
		OK(fabsf(out[0]) < 0.01f);      /* cancels */
		OK(fabsf(out[1] - 0.5f) < 0.01f);
		free(out);
	}
	unlink(p);
}

static void test_wav_not_riff(void)
{
	char path[] = "/tmp/wat_test_XXXXXX.wav";
	int fd = mkstemps(path, 4);
	FILE *f = fdopen(fd, "wb");
	fwrite("NOPE1234WAVE", 12, 1, f);
	fclose(f);

	int n = 0, rate = 0;
	float *out = wav_load(path, &n, &rate);
	OK(out == NULL);
	unlink(path);
}

static void test_wav_missing_file(void)
{
	int n = 0, rate = 0;
	float *out = wav_load("/nonexistent/nope.wav", &n, &rate);
	OK(out == NULL);
}

static void test_wav_empty_file(void)
{
	char path[] = "/tmp/wat_test_XXXXXX.wav";
	int fd = mkstemps(path, 4);
	close(fd);
	int n = 0, rate = 0;
	float *out = wav_load(path, &n, &rate);
	OK(out == NULL);
	unlink(path);
}

static void test_wav_truncated_riff(void)
{
	char path[] = "/tmp/wat_test_XXXXXX.wav";
	int fd = mkstemps(path, 4);
	FILE *f = fdopen(fd, "wb");
	fwrite("RIFF\x00\x00\x00\x00", 8, 1, f);  /* no WAVE */
	fclose(f);
	int n = 0, rate = 0;
	float *out = wav_load(path, &n, &rate);
	OK(out == NULL);
	unlink(path);
}

static void test_wav_8bit_rejected(void)
{
	/* we only support 16-bit; 8-bit should be rejected cleanly */
	char path[] = "/tmp/wat_test_XXXXXX.wav";
	int fd = mkstemps(path, 4);
	FILE *f = fdopen(fd, "wb");
	unsigned char hdr[] = {
		'R','I','F','F', 40,0,0,0, 'W','A','V','E',
		'f','m','t',' ', 16,0,0,0,
		1,0, 1,0,              /* PCM, mono */
		0x80,0x3E,0,0,         /* 16000 Hz */
		0x80,0x3E,0,0,         /* byte rate */
		1,0,                   /* block align */
		8,0,                   /* 8-bit */
		'd','a','t','a', 4,0,0,0, 0x80,0x80,0x80,0x80
	};
	fwrite(hdr, sizeof hdr, 1, f);
	fclose(f);
	int n = 0, rate = 0;
	float *out = wav_load(path, &n, &rate);
	OK(out == NULL);  /* should reject, not crash */
	unlink(path);
}

static void test_wav_huge_data_size(void)
{
	/*
	 * Crafted WAV claiming a 4GB data chunk.
	 * Must not allocate or crash. The cap is WAV_MAX_SAMPLES.
	 */
	char path[] = "/tmp/wat_test_XXXXXX.wav";
	int fd = mkstemps(path, 4);
	FILE *f = fdopen(fd, "wb");
	unsigned char hdr[] = {
		'R','I','F','F', 40,0,0,0, 'W','A','V','E',
		'f','m','t',' ', 16,0,0,0,
		1,0, 1,0,
		0x80,0x3E,0,0,
		0x00,0x7D,0,0,
		2,0,
		16,0,
		'd','a','t','a', 0xFF,0xFF,0xFF,0xFF   /* ~4 GB */
	};
	fwrite(hdr, sizeof hdr, 1, f);
	/* no actual data follows */
	fclose(f);
	int n = 0, rate = 0;
	float *out = wav_load(path, &n, &rate);
	OK(out == NULL);
	unlink(path);
}

static void test_wav_zero_data(void)
{
	char *p = write_wav(16000, 1, 16, 0, NULL);
	int n = 0, rate = 0;
	float *out = wav_load(p, &n, &rate);
	/* zero frames: should return NULL (no usable audio) */
	OK(out == NULL);
	unlink(p);
}

static void test_wav_non_pcm_format(void)
{
	/* fmt=3 (IEEE float) — not PCM, should be rejected */
	char path[] = "/tmp/wat_test_XXXXXX.wav";
	int fd = mkstemps(path, 4);
	FILE *f = fdopen(fd, "wb");
	unsigned char hdr[] = {
		'R','I','F','F', 40,0,0,0, 'W','A','V','E',
		'f','m','t',' ', 16,0,0,0,
		3,0,                   /* fmt=3 IEEE float */
		1,0,
		0x80,0x3E,0,0,
		0x00,0xFA,0,0,
		4,0,
		32,0,                  /* 32-bit */
		'd','a','t','a', 4,0,0,0, 0,0,0,0
	};
	fwrite(hdr, sizeof hdr, 1, f);
	fclose(f);
	int n = 0, rate = 0;
	float *out = wav_load(path, &n, &rate);
	OK(out == NULL);
	unlink(path);
}

static void test_wav_too_many_channels(void)
{
	/* 255 channels — unreasonable, should be rejected */
	char path[] = "/tmp/wat_test_XXXXXX.wav";
	int fd = mkstemps(path, 4);
	FILE *f = fdopen(fd, "wb");
	unsigned char hdr[] = {
		'R','I','F','F', 40,0,0,0, 'W','A','V','E',
		'f','m','t',' ', 16,0,0,0,
		1,0,
		0xFF,0,                /* 255 channels */
		0x80,0x3E,0,0,
		0,0,0,0,
		0,0,
		16,0,
		'd','a','t','a', 8,0,0,0, 0,0,0,0,0,0,0,0
	};
	fwrite(hdr, sizeof hdr, 1, f);
	fclose(f);
	int n = 0, rate = 0;
	float *out = wav_load(path, &n, &rate);
	OK(out == NULL);
	unlink(path);
}

int main(void)
{
	fprintf(stdout, "== wat unit tests ==\n");

	/*
	 * Dup stderr to /dev/null during tests to suppress wav_load's
	 * diagnostics on the error-case tests, while keeping stdout for
	 * our PASS/FAIL output.
	 */
	FILE *saved_stderr = fdopen(dup(fileno(stderr)), "w");
	if (!freopen("/dev/null", "w", stderr)) {
		/* non-fatal: tests still run, just noisier */
	}

	test_rms_zero();
	test_rms_ones();
	test_rms_halves();
	test_rms_empty();

	test_wav_valid_mono();
	test_wav_stereo_to_mono();
	test_wav_not_riff();
	test_wav_missing_file();
	test_wav_empty_file();
	test_wav_truncated_riff();
	test_wav_8bit_rejected();
	test_wav_huge_data_size();
	test_wav_zero_data();
	test_wav_non_pcm_format();
	test_wav_too_many_channels();

	if (saved_stderr) fclose(saved_stderr);
	fprintf(stdout, "== %d/%d passed ==\n", total - failed, total);
	return failed ? 1 : 0;
}
