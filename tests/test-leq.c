/* test-leq.c - checks for the LEQ port. */
#include "../src/leq.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static double frand(void) { return (double)rand() / RAND_MAX * 2.0 - 1.0; }

/* Real FFT + powspec against a naive double-precision DFT. */
static void test_fft(unsigned rate)
{
	leq_t s;
	leq_init(&s, rate, 1, 4);
	const unsigned N = s.N;
	float *d = malloc(N * sizeof(float));
	double *x = malloc(N * sizeof(double));
	for (unsigned i = 0; i < N; i++)
		x[i] = d[i] = (float)(frand() * 1000.0);

	leq_fftr(&s, d);
	leq_powspec(d, (int)N);

	double maxrel = 0, peak = 0;
	for (unsigned k = 0; k <= N / 2; k++) {
		double re = 0, im = 0;
		for (unsigned n = 0; n < N; n++) {
			re += x[n] * cos(2 * M_PI * k * n / N);
			im -= x[n] * sin(2 * M_PI * k * n / N);
		}
		const double p = re * re + im * im;
		if (p > peak) peak = p;
		const double err = fabs(d[k] - p);
		if (err > maxrel) maxrel = err;
	}
	maxrel /= peak;
	printf("fft  rate=%6u N=%4u  max |P - P_dft| / max P = %.2e\n", rate, N, maxrel);
	CHECK(maxrel < 1e-5, "fftr/powspec mismatch at N=%u", N);
	free(d); free(x); leq_free(&s);
}

/* An impulse must come out exactly N frames later. */
static void test_latency(unsigned rate, int nch, int chunk)
{
	leq_t s;
	leq_init(&s, rate, nch, 4);
	const int total = (int)s.N * 4;
	float *in = calloc((size_t)total * nch, sizeof(float));
	float *out = calloc((size_t)total * nch, sizeof(float));
	const int at = 123;
	in[at * nch] = 0.5f;
	for (int i = 0; i < total; i += chunk) {
		const int n = i + chunk > total ? total - i : chunk;
		leq_process(&s, in + (size_t)i * nch, out + (size_t)i * nch, n);
	}
	int found = -1;
	for (int i = 0; i < total; i++)
		if (out[i * nch] != 0.0f) { found = i; break; }
	printf("lat  rate=%6u nch=%d chunk=%4d  impulse %d -> %d (latency %d, N=%u, gain %.3f)\n",
	       rate, nch, chunk, at, found, found - at, s.N, out[found * nch] / 0.5f);
	CHECK(found - at == (int)s.N, "latency %d != N %u", found - at, s.N);
	free(in); free(out); leq_free(&s);
}

static void test_constants(void)
{
	/* Threshold table as raw bits from WMALFXGFXDSP.dll .rdata 0x180095550 */
	static const uint32_t want[] = {
		0x447a0000, 0x42c80000, 0x41fccccd, 0x41200000, 0x404a3d71, 0x40000000,
		0x3f800000, 0x3f800000, 0x3ea1cac1, 0x3ea1cac1,
	};
	const float vals[] = { 1000.0f, 100.0f, 31.6f, 10.0f, 3.16f, 2.0f, 1.0f, 1.0f, 0.316f, 0.316f };
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		uint32_t u; memcpy(&u, &vals[i], 4);
		CHECK(u == want[i], "threshold %u bits %08x != %08x", i, u, want[i]);
	}
	leq_t s;
	leq_init(&s, 48000, 2, 4);
	printf("bins 48k N=%u:", s.N);
	for (int i = 0; i <= LEQ_NBANDS; i++) printf(" %u", s.edges[i]);
	printf("\n");
	leq_free(&s);
}

/* Pink noise (Paul Kellet's filter), unit-ish RMS after scaling. */
static double pink(double st[7])
{
	const double w = frand();
	st[0] = 0.99886 * st[0] + w * 0.0555179;
	st[1] = 0.99332 * st[1] + w * 0.0750759;
	st[2] = 0.96900 * st[2] + w * 0.1538520;
	st[3] = 0.86650 * st[3] + w * 0.3104856;
	st[4] = 0.55000 * st[4] + w * 0.5329522;
	st[5] = -0.7616 * st[5] - w * 0.0168980;
	const double p = st[0] + st[1] + st[2] + st[3] + st[4] + st[5] + st[6] + w * 0.5362;
	st[6] = w * 0.115926;
	return p * 0.2;   /* ~ -9 dBFS RMS before level scaling */
}

/* Steady-state gain vs. level (stereo, 48 kHz, release=2, after 20 s).
 * Levels are RMS dBFS; a 0 dBFS-RMS sine peaks at +3 dBFS. */
static void test_curve(int noise)
{
	const unsigned rate = 48000;
	printf("\nstatic curve: %s, stereo, 48 kHz, release=2, after 20 s\n",
	       noise ? "pink noise" : "1 kHz sine");
	printf("   in dBFS(rms)  gain dB   out dBFS(rms)\n");
	for (int db = -70; db <= 0; db += 10) {
		leq_t s;
		leq_init(&s, rate, 2, 2);
		const int chunk = 480, secs = 20;
		float in[480 * 2], out[480 * 2];
		double st[7] = { 0 };
		long t = 0;
		double ein = 0, eout = 0;
		const int nblk = secs * (int)rate / chunk;
		/* measure input RMS of the generator first to calibrate */
		double cal = 0;
		for (int i = 0; i < 480000; i++) {
			const double v = noise ? pink(st) : sin(2 * M_PI * 1000.0 * i / rate);
			cal += v * v;
		}
		cal = sqrt(cal / 480000);
		const double amp = pow(10.0, db / 20.0) / cal;
		for (int b = 0; b < nblk; b++) {
			for (int i = 0; i < chunk; i++, t++) {
				const double v = noise ? pink(st) : sin(2 * M_PI * 1000.0 * t / rate);
				in[2 * i] = in[2 * i + 1] = (float)(amp * v);
			}
			leq_process(&s, in, out, chunk);
			if (b >= nblk - (int)rate / chunk)
				for (int i = 0; i < chunk * 2; i++) {
					ein += in[i] * (double)in[i];
					eout += out[i] * (double)out[i];
				}
		}
		printf("   %11d   %7.2f   %12.2f\n", db, 20 * log10(s.gain),
		       10 * log10(eout / ein) + db);
		leq_free(&s);
	}
}

static void test_release(void)
{
	printf("\nrelease setting -> per-block weight and time constant (48 kHz, hop 256):\n");
	for (unsigned r = 0; r <= 7; r++) {
		leq_t s;
		leq_init(&s, 48000, 2, r);
		const double hop = (s.N / 2) / 48000.0;
		const double tau = s.weight < 1 ? -hop / log(1.0 - s.weight) : 0;
		printf("   r=%u  w=%.6g  tau=%.3f s%s\n", r, s.weight, tau,
		       r == LEQ_RELEASE_DEFAULT ? "  (Windows default)" : "");
		leq_free(&s);
	}
}

int main(void)
{
	test_constants();
	test_fft(44100);
	test_fft(48000);
	test_fft(96000);
	test_fft(192000);
	test_latency(48000, 2, 1);
	test_latency(48000, 2, 97);
	test_latency(48000, 2, 1024);
	test_latency(44100, 6, 333);
	test_latency(96000, 1, 4096);
	test_curve(0);
	test_curve(1);
	test_release();
	printf("\n%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
	return failures != 0;
}
