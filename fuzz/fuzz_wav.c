/*
 * AFL fuzz harness for wav_load().
 *
 * Build:  make fuzz
 * Run:    afl-fuzz -i fuzz/seeds -o fuzz/findings -- ./fuzz/fuzz_wav @@
 *
 * The harness reads a single WAV file path from argv[1] and passes
 * it to wav_load. Any crash, hang, or leak-detectable bug is found
 * by AFL. Combine with AFL's address sanitizer mode:
 *
 *   AFL_USE_ASAN=1 make fuzz
 */
#include "wav.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 2) return 1;

	/* Silence wav_load's diagnostics to avoid polluting AFL's output. */
	(void)!freopen("/dev/null", "w", stderr);

	int n = 0, rate = 0;
	float *out = wav_load(argv[1], &n, &rate);
	free(out);
	return 0;
}
