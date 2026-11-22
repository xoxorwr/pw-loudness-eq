/*
 * limiter.c - the Windows audio engine's output limiter (CAudioLimiter).
 * See limiter.h.
 *
 * Windows processes whole buffers: it computes every incoming frame's peak
 * first, then runs the gain over the delayed frames. Per frame that is the
 * same as what is done here: the frame entering the delay line decides the
 * gain, the frame leaving it is scaled by it.
 */
#include "limiter.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define THRESHOLD  0.985f     /* 0x3f7c28f6 */
#define GARBAGE    128.0f     /* peaks above this: the buffer is dropped */
#define RELEASE    2.205      /* nepers per second */

int limiter_init(limiter_t *l, unsigned rate, int nch)
{
	memset(l, 0, sizeof(*l));
	if (rate == 0 || nch <= 0)
		return -1;
	l->nch = nch;
	/* CAudioLimiter::AllocateDelayLine */
	l->L = rate <= 16000 ? 16 : rate <= 32000 ? 32 : rate <= 64000 ? 64 : 128;
	l->invL = 1.0f / (float)l->L;
	/* CAudioLimiter::Initialize */
	l->rise = exp(RELEASE / (double)(float)rate);
	l->fall = exp(-RELEASE / (double)(float)rate);
	l->delay = calloc((size_t)l->L * nch, sizeof(float));
	if (l->delay == NULL)
		return -1;
	limiter_reset(l);
	return 0;
}

void limiter_free(limiter_t *l)
{
	free(l->delay);
	l->delay = NULL;
}

/* CAudioLimiter::Reset */
void limiter_reset(limiter_t *l)
{
	l->gain = 1.0f;
	l->target = 1.0f;
	l->step = 0.0f;
	l->peak = THRESHOLD;
	l->count = 0;
	l->dpos = 0;
	if (l->delay)
		memset(l->delay, 0, (size_t)l->L * l->nch * sizeof(float));
}

/* One frame of CAudioLimiter::ProcessLimiterBuffer: p is the peak of the
 * frame entering the delay line. Returns the gain for the frame leaving it,
 * or a negative value for garbage input. */
static float update_gain(limiter_t *l, float p)
{
	float g = l->gain, ref = l->peak;

	if (p > ref) {
		if (p > GARBAGE)
			return -1.0f;
		float t = THRESHOLD / p;
		if (t < g) {
			if (l->count == 0) {
				/* ramp down over the look-ahead, reaching t on time */
				l->count = l->L;
				l->step = (g - t) * l->invL;
			} else {
				float s = (g - t) * l->invL;
				if (l->target < g - (float)l->count * s)
					/* the running ramp is steeper: keep it, run it longer */
					l->count += (unsigned)(long long)ceilf((l->target - t) / l->step);
				else {
					l->count = l->L;
					l->step = s;
				}
			}
			l->target = t;
		}
		l->peak = ref = p;
	}

	if (l->count == 0) {
		/* release, only once the signal is 6 dB under the tracked peak */
		if (p + p <= ref && g < 1.0f) {
			g = (float)((double)g * l->rise);
			l->peak = (float)((double)ref * l->fall);
			if (g >= 1.0f || l->peak <= THRESHOLD) {
				g = 1.0f;
				l->peak = THRESHOLD;
			}
		}
	} else {
		g -= l->step;
		l->count--;
	}
	l->gain = g;
	return g;
}

void limiter_process(limiter_t *l, const float *in, float *out, int nframes,
		     const float *volume)
{
	const int nch = l->nch;
	const float *x = in;
	float *y = out;
	bool drop = false;

	for (int n = 0; n < nframes; n++, x += nch, y += nch) {
		/* CAudioLimiter::ComputeMaxFrameValsInBuffer, on the signal as it
		 * is after the volume */
		float p = 0.0f;
		for (int c = 0; c < nch; c++)
			p = fmaxf(p, fabsf(volume ? x[c] * volume[c] : x[c]));
		float g = update_gain(l, p);
		if (g < 0.0f)
			drop = true;

		float *d = l->delay + (size_t)l->dpos * nch;
		for (int c = 0; c < nch; c++) {
			float leaving = d[c];
			d[c] = x[c];          /* read x before y: in and out may alias */
			y[c] = leaving * g;
		}
		if (++l->dpos == l->L)
			l->dpos = 0;
	}
	/* Windows marks the whole buffer silent (APOProcess) */
	if (drop)
		memset(out, 0, (size_t)nframes * nch * sizeof(float));
}
