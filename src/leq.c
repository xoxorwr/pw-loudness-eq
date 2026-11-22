/*
 * leq.c - Windows "Loudness Equalization" DSP, reimplemented.
 * See leq.h and README.md for how the original works.
 */
#include "leq.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Exact bit patterns of the constants used by WMALFXGFXDSP.dll. */
static float  f32(uint32_t u) { float f;  memcpy(&f, &u, 4); return f; }
static double f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

#define K_TWO_PI       f64(0x401921fb54442d18ULL) /* 2*pi (fftr)                    */
#define K_TWO_PI_WIN   f64(0x401921fb54442d11ULL) /* 2*pi, slightly off (lm_init)   */
#define K_WIN_SCALE    16384.0                    /* 0x40d0000000000000             */
#define K_REL_STEP1    f32(0x3dcccccdu)           /* 0.1   (-20 dB / step, r <= 3)  */
#define K_REL_STEP2    f32(0x3ea1cac1u)           /* 0.316 (-10 dB / step, r 4..7)  */
#define K_REL_SCALE    0.0078125f                 /* 1/128 (0x3b000000)             */
#define K_REL_RATE     44100.0f                   /* 0x472c4400                     */
#define K_TINY         f32(0x0a4fb11fu)           /* 1e-32                          */
#define K_LOUD_SCALE   f64(0x3ffb851eb851eb85ULL) /* 1.72                           */
#define K_TARGET       1300.0f                    /* 0x44a28000                     */
#define K_EXPONENT     f64(0x3ff99999a0000000ULL) /* (double)1.6f                   */
#define K_MAX_GAIN     10.0f                      /* 0x41200000, also initial gain  */
#define K_MAX_N        4096u

/* Band edges in Hz (.data 0x1801b6620). */
static const unsigned band_hz[LEQ_NBANDS] = {
	100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000,
	2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24000,
};

/* Per-band threshold subtracted before compression (.rdata 0x180095550).
 * Values are float literals that round to the same bits as the DLL's. */
static const float band_thresh[LEQ_NBANDS] = {
	1000.0f, 100.0f, 31.6f, 10.0f, 3.16f, 2.0f, 1.0f, 1.0f, 0.316f, 0.316f,
	1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
	4.0f, 10.0f, 100.0f, 1000.0f,
};

/* ------------------------------------------------------------------------ */
/* FFT                                                                       */
/* ------------------------------------------------------------------------ */

/* In-place forward complex FFT of n complex points (interleaved re/im),
 * kernel e^{-i...}, unnormalised, natural-order output. Radix-2 decimation
 * in frequency followed by bit reversal, like fft_inner<2> in the DLL. The DLL
 * builds its twiddles by float recurrence, while this version uses a
 * precomputed table, so results agree to float rounding, not bit for bit. */
static void fft_c(const leq_t *s, float *d, int n)
{
	const int tw_n = (int)(s->N >> 1);       /* table built for N/2 points */
	for (int len = n; len >= 2; len >>= 1) {
		const int half = len >> 1, step = tw_n / len;
		for (int j = 0; j < half; j++) {
			const float wr = s->twiddle[2 * j * step];
			const float wi = s->twiddle[2 * j * step + 1];
			for (int i = j; i < n; i += len) {
				float *a = d + 2 * i, *b = d + 2 * (i + half);
				const float tr = a[0] - b[0], ti = a[1] - b[1];
				a[0] += b[0];
				a[1] += b[1];
				b[0] = tr * wr - ti * wi;
				b[1] = tr * wi + ti * wr;
			}
		}
	}
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t;
			t = d[2 * i];     d[2 * i]     = d[2 * j];     d[2 * j]     = t;
			t = d[2 * i + 1]; d[2 * i + 1] = d[2 * j + 1]; d[2 * j + 1] = t;
		}
	}
}

/* fftr: real FFT of N points in place (Numerical Recipes realft packing:
 * d[0] = DC, d[1] = Nyquist, then re/im pairs). Transcribed from the DLL. */
void leq_fftr(leq_t *s, float *d)
{
	const int n = (int)s->N, half = n >> 1;
	const float c = (float)s->rc, sn = (float)s->rs;
	float wr = 1.0f, wi = 0.0f;
	float *q = d + n - 2;

	fft_c(s, d, half);

	for (int i = 2; i < half; i += 2, q -= 2) {
		const float nwr = c * wr - sn * wi;
		wi = c * wi + sn * wr;
		wr = nwr;
		const float h2i = (float)((double)(d[i] - q[0]) * 0.5);
		const float h1r = (float)((double)(d[i] + q[0]) * 0.5);
		const float h2r = (float)((double)(d[i + 1] + q[1]) * 0.5);
		const float h1i = (float)((double)(d[i + 1] - q[1]) * 0.5);
		const float t1 = wr * h2r - wi * h2i;
		const float t2 = -wi * h2r - wr * h2i;
		d[i]     = t1 + h1r;
		q[0]     = h1r - t1;
		d[i + 1] = h1i + t2;
		q[1]     = t2 - h1i;
	}
	d[half + 1] = -d[half + 1];
	const float t = d[1];
	d[1] = d[0] - t;
	d[0] = d[0] + t;
}

/* powspec: d[0..n/2] = |X_k|^2 from the realft packing. */
void leq_powspec(float *d, int n)
{
	const int h = n / 2;
	const float nyq = d[1];
	d[0] = d[0] * d[0];
	for (int k = 1; k < h; k++)
		d[k] = d[2 * k + 1] * d[2 * k + 1] + d[2 * k] * d[2 * k];
	d[h] = nyq * nyq;
}

/* ------------------------------------------------------------------------ */
/* Loudness model                                                            */
/* ------------------------------------------------------------------------ */

/* loudness(): Stevens-style power law, E^0.25 per band above threshold. */
static float loudness(const float *E)
{
	float sum = 0.0f;
	for (int b = 0; b < LEQ_NBANDS; b++) {
		float x = E[b] - band_thresh[b];
		if (!(x > 0.0f))          /* comiss/ja: NaN also takes this path */
			x = K_TINY;
		sum += (float)sqrt(sqrt((double)x));
	}
	return (float)((double)sum * K_LOUD_SCALE);
}

/* LEQ_block: analyse one N-sample frame made of two half-frames, update the
 * gain, then write the gain-crossfaded output:
 *   acc[n]  += prev[n] * g * n/H        (completes the older half-frame)
 *   next[n]  = cur[n]  * g * (H-n)/H    (first half of the newer one's ramp) */
static void leq_block(leq_t *s, const float *prev, const float *cur,
		      float *acc, float *next)
{
	const int nch = s->nch;
	const unsigned N = s->N, H = N >> 1, nbins = H + 1;
	float E[LEQ_NBANDS] = { 0 };
	float *w = s->work;

	for (int ch = 0; ch < nch; ch++) {
		for (unsigned n = 0; n < H; n++)
			w[n] = prev[n * nch + ch];
		for (unsigned n = 0; n < H; n++)
			w[H + n] = cur[n * nch + ch];
		for (unsigned n = 0; n < N; n++)
			w[n] *= s->window[n];

		leq_fftr(s, w);
		leq_powspec(w, (int)N);

		for (int b = 0; b < LEQ_NBANDS; b++) {
			float e = 0.0f;
			for (unsigned k = s->edges[b]; k < s->edges[b + 1] && k < nbins; k++)
				e += w[k];
			E[b] += e;
		}
	}

	float target = (float)pow((double)(K_TARGET / loudness(E)), K_EXPONENT);
	target = target < K_MAX_GAIN ? target : K_MAX_GAIN;   /* minss */

	const float g0 = s->gain;
	if (g0 > target)
		s->gain = target;                                 /* instant attack */
	else
		s->gain = (target - g0) * s->weight + g0;         /* slow release   */

	const float g = s->gain, fH = (float)H;
	for (unsigned n = 0; n < H; n++) {
		const float a = ((float)n * g) / fH;
		for (int c = 0; c < nch; c++)
			acc[n * nch + c] = prev[n * nch + c] * a + acc[n * nch + c];
	}
	for (unsigned n = 0; n < H; n++) {
		const float a = ((float)(H - n) * g) / fH;
		for (int c = 0; c < nch; c++)
			next[n * nch + c] = cur[n * nch + c] * a;
	}
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */
/* ------------------------------------------------------------------------ */

void leq_set_release(leq_t *s, unsigned release)
{
	/* LEQ_UpdateWeight */
	unsigned i = 0;
	const unsigned r3 = release < 3 ? release : 3;
	const unsigned r7 = release < 7 ? release : 7;
	float f = 1.0f;
	for (; i < r3; i++)
		f *= K_REL_STEP1;
	for (; i < r7; i++)
		f *= K_REL_STEP2;
	s->release = release;
	s->weight = (float)s->N * K_REL_SCALE * f * (K_REL_RATE / (float)s->rate);
}

static unsigned fft_size(unsigned rate)
{
	/* N = 2^ceil(log2(rate/100)), max 4096 */
	unsigned x = rate / 100, bits = (x & (x - 1)) != 0;
	for (unsigned v = x; v > 1; v >>= 1)
		bits++;
	return (1u << bits) > K_MAX_N ? K_MAX_N : (1u << bits);
}

int leq_init(leq_t *s, unsigned rate, int nch, unsigned release)
{
	memset(s, 0, sizeof(*s));
	if (rate < 800 || nch < 1 || nch > 64)
		return -1;

	s->rate = rate;
	s->nch = nch;
	s->N = fft_size(rate);
	const unsigned N = s->N, H = N >> 1;
	const size_t half = (size_t)H * nch;

	s->in0 = calloc(half, sizeof(float));
	s->in1 = calloc(half, sizeof(float));
	s->out0 = calloc(half, sizeof(float));
	s->out1 = calloc(half, sizeof(float));
	s->work = calloc(N, sizeof(float));
	s->window = calloc(N, sizeof(float));
	s->twiddle = calloc(N / 2, sizeof(float));
	if (!s->in0 || !s->in1 || !s->out0 || !s->out1 || !s->work ||
	    !s->window || !s->twiddle) {
		leq_free(s);
		return -1;
	}

	/* lm_init: band edges in bins, and the analysis window. */
	const float one = 1.0f;
	s->edges[0] = 0;
	for (int i = 0; i < LEQ_NBANDS; i++) {
		const unsigned lo = s->edges[i] + 1;
		unsigned e = (unsigned)(long long)
			((float)band_hz[i] * (one / ((float)rate / (float)N)) + one);
		if (e < lo)
			e = lo;
		if (e > H)
			e = H;
		s->edges[i + 1] = e;
	}
	const double invN = 1.0 / (double)N, invH = 1.0 / (double)H;
	for (unsigned n = 0; n < N; n++) {
		const double c = cos((double)((float)n + 0.5f) * invN * K_TWO_PI_WIN);
		s->window[n] = (float)((K_WIN_SCALE - c * K_WIN_SCALE) * invH);
	}

	/* FFT tables (not in the DLL, which computes these on the fly). */
	for (unsigned k = 0; k < N / 4; k++) {
		const double a = -K_TWO_PI * (double)k / (double)H;
		s->twiddle[2 * k] = (float)cos(a);
		s->twiddle[2 * k + 1] = (float)sin(a);
	}
	s->rc = cos(K_TWO_PI / (double)N);
	s->rs = sin(K_TWO_PI / (double)N);

	s->pos = 0;
	s->gain = K_MAX_GAIN;
	leq_set_release(s, release);
	return 0;
}

void leq_free(leq_t *s)
{
	free(s->in0);
	free(s->in1);
	free(s->out0);
	free(s->out1);
	free(s->work);
	free(s->window);
	free(s->twiddle);
	memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------------ */
/* Streaming                                                                 */
/* ------------------------------------------------------------------------ */

/* LEQ_process. The DLL's version has fast paths that run LEQ_block directly
 * on the caller's buffers when a call spans several half-frames. This loop
 * produces the same output with fewer special cases. */
void leq_process(leq_t *s, const float *in, float *out, int nframes)
{
	const int nch = s->nch, H = (int)(s->N >> 1);

	while (nframes > 0) {
		int n = H - s->pos;
		if (n > nframes)
			n = nframes;
		const size_t off = (size_t)s->pos * nch, cnt = (size_t)n * nch;

		memcpy(out, s->out0 + off, cnt * sizeof(float));
		memcpy(s->in1 + off, in, cnt * sizeof(float));
		s->pos += n;
		in += cnt;
		out += cnt;
		nframes -= n;

		if (s->pos == H) {
			float *t;
			leq_block(s, s->in0, s->in1, s->out1, s->out0);
			t = s->in0;  s->in0 = s->in1;   s->in1 = t;
			t = s->out0; s->out0 = s->out1; s->out1 = t;
			s->pos = 0;
		}
	}
}
