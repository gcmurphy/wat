#ifndef WAT_VAD_H
#define WAT_VAD_H

/*
 * Root-mean-square energy of n float32 samples in [-1, 1].
 * Returns 0 if n <= 0. Used for energy-based voice activity detection.
 */
float rms_energy(const float *buf, int n);

#endif
