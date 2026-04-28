#include "wav.h"
#include "cleanup.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	WAV_TAG_BYTES   = 4,             /* fourcc length */
	WAV_SKIP_BUFSZ  = 4096,          /* read-and-discard buffer */
};

static int read_exact(FILE *f, void *buf, size_t n)
{
	return fread(buf, 1, n, f) == n;
}

/*
 * Read a little-endian uint16/uint32 explicitly, regardless of host
 * endianness. WAV files are always little-endian.
 */
static int read_u16(FILE *f, uint16_t *out)
{
	unsigned char b[2];
	if (!read_exact(f, b, 2)) return 0;
	*out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
	return 1;
}

static int read_u32(FILE *f, uint32_t *out)
{
	unsigned char b[4];
	if (!read_exact(f, b, 4)) return 0;
	*out = (uint32_t)b[0]        | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 1;
}

/*
 * Skip n bytes forward. Use fseek when n fits in long; otherwise
 * read-and-discard. Both refuse to move backward.
 */
static int skip(FILE *f, uint32_t n)
{
	if (n == 0) return 1;
#if LONG_MAX >= UINT32_MAX
	if (fseek(f, (long)n, SEEK_CUR) == 0) return 1;
#else
	if (n <= (uint32_t)LONG_MAX && fseek(f, (long)n, SEEK_CUR) == 0)
		return 1;
#endif
	/* fseek may fail on pipes or huge offsets; fall back to read-and-discard */
	char buf[WAV_SKIP_BUFSZ];
	while (n > 0) {
		size_t take = n > sizeof buf ? sizeof buf : n;
		if (fread(buf, 1, take, f) != take) return 0;
		n -= (uint32_t)take;
	}
	return 1;
}

float *wav_load(const char *path, int *n, int *out_rate)
{
	*n = 0;
	if (out_rate) *out_rate = 0;

	_autofclose FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "wat: %s: %s\n", path, strerror(errno));
		return NULL;
	}

	/* RIFF header: "RIFF" <size> "WAVE" */
	char hdr[WAV_HEADER_SIZE];
	if (!read_exact(f, hdr, sizeof hdr) ||
	    memcmp(hdr,                "RIFF", WAV_TAG_BYTES) ||
	    memcmp(hdr + WAV_TAG_BYTES + 4, "WAVE", WAV_TAG_BYTES)) {
		fprintf(stderr, "wat: %s: not a WAV file\n", path);
		return NULL;
	}

	uint16_t fmt = 0, channels = 0, bits = 0;
	uint32_t rate = 0;
	int have_fmt = 0;

	for (;;) {
		char id[WAV_TAG_BYTES];
		uint32_t sz;
		if (!read_exact(f, id, sizeof id) || !read_u32(f, &sz))
			break;

		if (!memcmp(id, "fmt ", WAV_TAG_BYTES)) {
			if (sz < WAV_FMT_MIN_SIZE) break;
			uint16_t block_align;
			uint32_t byte_rate;
			if (!read_u16(f, &fmt) ||
			    !read_u16(f, &channels) ||
			    !read_u32(f, &rate) ||
			    !read_u32(f, &byte_rate) ||
			    !read_u16(f, &block_align) ||
			    !read_u16(f, &bits)) break;
			(void)byte_rate;
			(void)block_align;
			if (sz > WAV_FMT_MIN_SIZE &&
			    !skip(f, sz - WAV_FMT_MIN_SIZE)) break;
			have_fmt = 1;
		} else if (!memcmp(id, "data", WAV_TAG_BYTES)) {
			if (!have_fmt) {
				fprintf(stderr, "wat: %s: data before fmt\n", path);
				return NULL;
			}
			if (fmt != WAV_FMT_PCM) {
				fprintf(stderr, "wat: %s: unsupported format %u "
				                "(only PCM is supported)\n", path, fmt);
				return NULL;
			}
			if (bits != WAV_PCM_BITS) {
				fprintf(stderr, "wat: %s: %u-bit samples (need %d)\n",
				        path, bits, WAV_PCM_BITS);
				return NULL;
			}
			if (channels < 1 || channels > WAV_MAX_CHANNELS) {
				fprintf(stderr, "wat: %s: %u channels not supported\n",
				        path, channels);
				return NULL;
			}
			uint32_t frame_sz = (uint32_t)WAV_BYTES_PER_S16 * channels;
			uint32_t nframes  = sz / frame_sz;
			if (nframes == 0 || nframes > WAV_MAX_SAMPLES) {
				fprintf(stderr, "wat: %s: audio length out of range\n",
				        path);
				return NULL;
			}

			_autofree float *out = malloc((size_t)nframes * sizeof(float));
			if (!out) {
				fprintf(stderr, "wat: out of memory\n");
				return NULL;
			}
			for (uint32_t i = 0; i < nframes; i++) {
				int32_t sum = 0;
				for (uint16_t c = 0; c < channels; c++) {
					uint16_t u;
					if (!read_u16(f, &u))
						return NULL;  /* _autofree releases out */
					sum += (int16_t)u;
				}
				out[i] = (float)sum / channels / WAV_S16_SCALE;
			}
			*n = (int)nframes;
			if (out_rate) *out_rate = (int)rate;
			float *ret = out;
			out = NULL;  /* defuse cleanup: ownership transferred */
			return ret;
		} else {
			if (!skip(f, sz)) break;
		}
		/* WAV chunks are word-aligned: skip pad byte if size is odd */
		if (sz & 1u) {
			char pad;
			if (fread(&pad, 1, 1, f) != 1) break;
		}
	}

	fprintf(stderr, "wat: %s: no usable audio data\n", path);
	return NULL;
}
