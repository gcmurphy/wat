/*
 * Structured WAV corpus generator for AFL.
 *
 * Builds a small set of distinct WAV files, each crafted to land on a
 * different branch in wav_load() — valid mono/stereo, every supported
 * sample rate, plus near-valid files that are 1 byte / 1 field away
 * from triggering each error branch. Mutating these gives AFL much
 * better starting coverage than mutating a single minimal seed.
 *
 * Build:  make fuzz-corpus
 * Output: fuzz/seeds/<name>.wav  (one file per seed())
 *
 * Each seed is a single function call below; tweak/add as new branches
 * appear in the parser.
 */
#include "wav.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *g_outdir;
static int         g_count;

static void put_u16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void put_u32(uint8_t *p, uint32_t v)
{
	p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}

/*
 * Write `bytes[0..n)` to fuzz/seeds/<name>.wav. We deliberately don't
 * abort on failure — if one seed can't be written, others may still be
 * useful — but we count failures so the Makefile can detect total loss.
 */
static void emit(const char *name, const uint8_t *bytes, size_t n)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s.wav", g_outdir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "make_corpus: %s: %s\n", path, strerror(errno));
		return;
	}
	if (fwrite(bytes, 1, n, f) != n)
		fprintf(stderr, "make_corpus: %s: short write\n", path);
	fclose(f);
	g_count++;
}

/*
 * Build a complete (RIFF + fmt + data) WAV in `buf`. Returns total
 * bytes written. The caller may then mutate any byte before emitting.
 *   bits      : usually 16 (the only depth wat accepts), but can be
 *               anything for hostile seeds
 *   samples   : payload size = samples * channels * bits/8
 */
static size_t build_wav(uint8_t *buf, size_t cap,
                        uint16_t fmt, uint16_t channels, uint32_t rate,
                        uint16_t bits, uint32_t samples)
{
	uint32_t byte_rate   = rate * channels * (bits / 8);
	uint16_t block_align = (uint16_t)(channels * (bits / 8));
	uint32_t data_sz     = samples * block_align;
	uint32_t total       = WAV_HEADER_SIZE + WAV_CHUNK_HDR_SIZE +
	                       WAV_FMT_MIN_SIZE + WAV_CHUNK_HDR_SIZE + data_sz;
	if (total > cap) return 0;

	size_t o = 0;
	memcpy(buf+o, "RIFF", 4); o += 4;
	put_u32(buf+o, total - 8);   o += 4;
	memcpy(buf+o, "WAVE", 4);    o += 4;

	memcpy(buf+o, "fmt ", 4);    o += 4;
	put_u32(buf+o, WAV_FMT_MIN_SIZE); o += 4;
	put_u16(buf+o, fmt);          o += 2;
	put_u16(buf+o, channels);     o += 2;
	put_u32(buf+o, rate);         o += 4;
	put_u32(buf+o, byte_rate);    o += 4;
	put_u16(buf+o, block_align);  o += 2;
	put_u16(buf+o, bits);         o += 2;

	memcpy(buf+o, "data", 4);    o += 4;
	put_u32(buf+o, data_sz);     o += 4;

	/* Fill the PCM payload with a low-amplitude triangle so the file is
	 * acoustically benign and compresses well in AFL's queue. */
	for (uint32_t i = 0; i < samples; i++) {
		for (uint16_t c = 0; c < channels; c++) {
			int16_t s = (int16_t)((i * 37) % 4096);
			put_u16(buf+o, (uint16_t)s);
			o += 2;
		}
	}
	return o;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: make_corpus <out-dir>\n");
		return 1;
	}
	g_outdir = argv[1];
	mkdir(g_outdir, 0755);  /* ignore EEXIST */

	uint8_t buf[8192];
	size_t  n;

	/* ── valid seeds: every supported rate, mono and stereo ── */
	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 1,  8000, 16, 64);
	emit("valid_8k_mono",   buf, n);
	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 1, 16000, 16, 64);
	emit("valid_16k_mono",  buf, n);
	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 1, 22050, 16, 64);
	emit("valid_22k_mono",  buf, n);
	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 1, 44100, 16, 64);
	emit("valid_44k_mono",  buf, n);
	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 1, 48000, 16, 64);
	emit("valid_48k_mono",  buf, n);
	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 2, 16000, 16, 64);
	emit("valid_16k_stereo", buf, n);
	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 2, 44100, 16, 64);
	emit("valid_44k_stereo", buf, n);
	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, WAV_MAX_CHANNELS,
	              16000, 16, 16);
	emit("valid_8ch_max",   buf, n);

	/* tiny payload: exercises the nframes==0/edge boundary */
	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 1, 16000, 16, 1);
	emit("valid_one_sample", buf, n);

	/* ── near-valid: each lands on a distinct error branch ── */
	n = build_wav(buf, sizeof buf, /*fmt=*/3, 1, 16000, 16, 32);
	emit("bad_fmt_float",   buf, n);  /* non-PCM format code */

	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 1, 16000, /*bits=*/8, 32);
	emit("bad_bits_8",      buf, n);  /* unsupported depth */

	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, 1, 16000, /*bits=*/24, 16);
	emit("bad_bits_24",     buf, n);

	n = build_wav(buf, sizeof buf, WAV_FMT_PCM, /*ch=*/9, 16000, 16, 8);
	emit("bad_channels_9",  buf, n);  /* > WAV_MAX_CHANNELS */

	/* odd-size unknown chunk to exercise the pad-byte branch */
	{
		uint8_t b[64] = {0};
		size_t o = 0;
		memcpy(b+o, "RIFF", 4); o += 4;
		put_u32(b+o, sizeof b - 8); o += 4;
		memcpy(b+o, "WAVE", 4); o += 4;
		memcpy(b+o, "JUNK", 4); o += 4;
		put_u32(b+o, 3);         o += 4;       /* odd size */
		b[o++]='X'; b[o++]='Y'; b[o++]='Z';
		b[o++]=0;                              /* pad byte */
		memcpy(b+o, "fmt ", 4); o += 4;
		put_u32(b+o, WAV_FMT_MIN_SIZE); o += 4;
		put_u16(b+o, WAV_FMT_PCM); o += 2;
		put_u16(b+o, 1);            o += 2;
		put_u32(b+o, 16000);        o += 4;
		put_u32(b+o, 32000);        o += 4;
		put_u16(b+o, 2);            o += 2;
		put_u16(b+o, 16);           o += 2;
		memcpy(b+o, "data", 4); o += 4;
		put_u32(b+o, 4);         o += 4;
		put_u16(b+o, 0);         o += 2;
		put_u16(b+o, 0);         o += 2;
		emit("odd_chunk_pad", b, o);
	}

	/* truncated header: only "RIFF" + size, no "WAVE" */
	{
		uint8_t b[8] = { 'R','I','F','F', 0,0,0,0 };
		emit("truncated_header", b, sizeof b);
	}

	/* empty file: shortest possible (zero bytes triggers fopen-OK,
	 * read-fail in read_exact). */
	emit("empty", (const uint8_t *)"", 0);

	fprintf(stderr, "make_corpus: wrote %d seeds to %s/\n",
	        g_count, g_outdir);
	return g_count > 0 ? 0 : 1;
}
