#include "vad.h"

#include <math.h>

float rms_energy(const float *buf, int n)
{
	if (n <= 0) return 0.0f;
	float sum = 0;
	for (int i = 0; i < n; i++)
		sum += buf[i] * buf[i];
	return sqrtf(sum / n);
}
