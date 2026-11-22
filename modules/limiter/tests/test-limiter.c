/* test-limiter.c - the Windows output limiter port, alone and after the
 * Loudness Equalization DSP (from modules/loudness). */
#include "../../loudness/src/leq.h"
#include "../src/limiter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/* The limiter aims at 0.985. When a slightly higher peak arrives mid-ramp,
 * Windows extends the running ramp by ceil() frames, so the gain can land a
 * frame late and the output a little above 0.985 (~0.1 % here). The 0.13 dB
 * of headroom absorbs it: what matters is that nothing reaches full scale. */
#define CEILING 0.99f
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static float peak(const float *x, size_t n)
{
	float p = 0;
	for (size_t i = 0; i < n; i++)
		p = fmaxf(p, fabsf(x[i]));
	return p;
}

/* below the threshold the limiter is a pure delay of L frames */
static void test_transparent(void)
{
	const int nch = 2, n = 4800;
	limiter_t l;
	float *x = malloc(sizeof(float) * n * nch), *y = malloc(sizeof(float) * n * nch);
	limiter_init(&l, 48000, nch);
	for (int i = 0; i < n * nch; i++)
		x[i] = 0.9f * sinf(i * 0.01f);
	limiter_process(&l, x, y, n, NULL);
	double err = 0;
	for (int i = (int)l.L * nch; i < n * nch; i++)
		err = fmax(err, fabs(y[i] - x[i - (int)l.L * nch]));
	printf("transparent below 0.985: max |out - delayed in| = %g (L=%u)\n", err, l.L);
	CHECK(err == 0.0, "limiter changed a signal below its threshold");
	limiter_free(&l);
	free(x); free(y);
}

/* a whistle through Loudness Equalization + limiter never clips */
static void test_whistles(void)
{
	const unsigned rate = 48000;
	const int n = rate * 4;
	float *x = malloc(sizeof(float) * n), *y = malloc(sizeof(float) * n);
	float worst = 0;

	for (int f = 1000; f <= 15000; f += 2000) {
		for (float amp = 0.05f; amp < 1.0f; amp *= 3) {
			leq_t s;
			limiter_t l;
			double ph = 0;
			for (int i = 0; i < n; i++) {
				double fi = f * (1 + 0.01 * sin(2 * M_PI * 5 * i / rate));
				ph += 2 * M_PI * fi / rate;
				x[i] = i < (int)rate / 2 ? 0 : amp * (float)sin(ph);
			}
			leq_init(&s, rate, 1, 4);
			limiter_init(&l, rate, 1);
			leq_process(&s, x, y, n);
			float before = peak(y, n);
			limiter_process(&l, y, y, n, NULL);
			float after = peak(y, n);
			if (amp > 0.3f && f == 3000)
				printf("whistle %5d Hz at %.2f: peak %.2f -> %.4f\n", f, amp, before, after);
			worst = fmaxf(worst, after);
			CHECK(after < CEILING, "whistle %d Hz amp %.2f peaks at %f", f, amp, after);
			leq_free(&s);
			limiter_free(&l);
		}
	}
	printf("whistles 1-15 kHz, 0.05-0.45: highest output peak %.4f\n", worst);
	free(x); free(y);
}

/* after a loud burst the gain comes back to 1 */
static void test_release(void)
{
	const unsigned rate = 48000;
	const int n = rate * 3;
	limiter_t l;
	float *x = malloc(sizeof(float) * n), *y = malloc(sizeof(float) * n);
	for (int i = 0; i < n; i++)
		x[i] = (i < (int)rate / 4 ? 2.0f : 0.1f) * sinf(i * 0.05f);
	limiter_init(&l, rate, 1);
	limiter_process(&l, x, y, n, NULL);
	float burst = peak(y, rate / 4), tail = peak(y + n - rate / 10, rate / 10);
	printf("burst at 2.0 -> %.4f, gain 2.5 s later %.4f\n", burst, l.gain);
	CHECK(burst < CEILING, "burst not limited: %f", burst);
	CHECK(l.gain == 1.0f && fabsf(tail - 0.1f) < 1e-3f, "gain didn't recover: %f", l.gain);
	limiter_free(&l);
	free(x); free(y);
}

/* Windows: volume, then limiter. Here: limiter told the volume, then the
 * volume. Same output, at any volume, per channel, muted or not. */
static void test_volume_order(void)
{
	const unsigned rate = 48000;
	const int nch = 2, n = rate * 2;
	const float vols[][2] = { { 1, 1 }, { 0.5f, 0.5f }, { 0.3f, 0.9f }, { 1.4f, 0.2f }, { 0, 0 } };
	float *x = malloc(sizeof(float) * n * nch), *vx = malloc(sizeof(float) * n * nch);
	float *ref = malloc(sizeof(float) * n * nch), *y = malloc(sizeof(float) * n * nch);
	double worst = 0;

	for (int i = 0; i < n; i++) {
		float env = (i / 12000) % 2 ? 2.5f : 0.4f;   /* sections above and below 1 */
		x[i * 2] = env * sinf(i * 0.031f);
		x[i * 2 + 1] = env * sinf(i * 0.017f + 1.0f);
	}
	for (size_t k = 0; k < sizeof(vols) / sizeof(vols[0]); k++) {
		const float *v = vols[k];
		limiter_t a, b;
		limiter_init(&a, rate, nch);
		limiter_init(&b, rate, nch);
		for (int i = 0; i < n * nch; i++)
			vx[i] = x[i] * v[i % nch];
		limiter_process(&a, vx, ref, n, NULL);          /* Windows order */
		limiter_process(&b, x, y, n, v);                /* PipeWire order... */
		double err = 0;
		for (int i = 0; i < n * nch; i++)
			err = fmax(err, fabs(y[i] * v[i % nch] - ref[i]));  /* ...then volume */
		worst = fmax(worst, err);
		CHECK(err < 1e-6, "volume %.1f/%.1f: differs from Windows order by %g", v[0], v[1], err);
		limiter_free(&a);
		limiter_free(&b);
	}
	printf("limiter before volume == Windows' volume before limiter: max diff %g\n", worst);
	free(x); free(vx); free(ref); free(y);
}

/* garbage (above 128) silences the buffer, as Windows does */
static void test_garbage(void)
{
	limiter_t l;
	float x[256], y[256];
	limiter_init(&l, 48000, 1);
	for (int i = 0; i < 256; i++)
		x[i] = i == 100 ? 1000.0f : 0.5f;
	limiter_process(&l, x, y, 256, NULL);
	CHECK(peak(y, 256) == 0.0f, "garbage buffer not silenced");
	limiter_free(&l);
}

int main(void)
{
	test_transparent();
	test_whistles();
	test_release();
	test_volume_order();
	test_garbage();
	printf("\n%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
	return failures != 0;
}
