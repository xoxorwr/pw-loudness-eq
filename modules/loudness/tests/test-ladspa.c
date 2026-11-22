/* test-ladspa.c - load pw_loudness_eq.so like a LADSPA host and check it
 * against the DSP core, for every channel-count label. */
#include "../src/ladspa-abi.h"
#include "../src/leq.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static float frand(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

static void test_label(const LADSPA_Descriptor *d, int nch, unsigned rate, int inplace)
{
	const int total = 48000 * 3;
	float *in = malloc(sizeof(float) * total * nch);       /* interleaved reference input */
	float *ref = malloc(sizeof(float) * total * nch);
	float *pin[8], *pout[8];
	float release = 4.0f, latency = -1.0f;

	srand(42);
	for (int i = 0; i < total; i++) {
		const float env = (i / 24000) % 2 ? 0.5f : 0.01f;   /* loud/quiet sections */
		for (int c = 0; c < nch; c++)
			in[i * nch + c] = frand() * env * (1.0f + 0.1f * c);
	}

	leq_t s;
	leq_init(&s, rate, nch, 4);
	leq_process(&s, in, ref, total);

	LADSPA_Handle h = d->instantiate(d, rate);
	CHECK(h != NULL, "instantiate %s", d->Label);
	for (int c = 0; c < nch; c++) {
		pin[c] = malloc(sizeof(float) * 8192);
		pout[c] = inplace ? pin[c] : malloc(sizeof(float) * 8192);
		d->connect_port(h, (unsigned long)c, pin[c]);
		d->connect_port(h, (unsigned long)(nch + c), pout[c]);
	}
	d->connect_port(h, (unsigned long)(2 * nch), &release);
	d->connect_port(h, (unsigned long)(2 * nch + 1), &latency);
	d->activate(h);

	double maxerr = 0;
	static const int chunks[] = { 1024, 7, 256, 4096, 333, 512, 1 };
	for (int off = 0, k = 0; off < total; k++) {
		int n = chunks[k % 7];
		if (off + n > total) n = total - off;
		for (int c = 0; c < nch; c++)
			for (int i = 0; i < n; i++)
				pin[c][i] = in[(off + i) * nch + c];
		d->run(h, (unsigned long)n);
		for (int c = 0; c < nch; c++)
			for (int i = 0; i < n; i++) {
				const double e = fabs((double)pout[c][i] - ref[(off + i) * nch + c]);
				if (e > maxerr) maxerr = e;
			}
		off += n;
	}
	printf("%-18s rate=%6u %s  latency port=%g (N=%u)  max |plugin - core| = %g\n",
	       d->Label, rate, inplace ? "in-place " : "separate ", latency, s.N, maxerr);
	CHECK(maxerr == 0.0, "%s output differs from core", d->Label);
	CHECK(latency == (float)s.N, "%s latency %g != %u", d->Label, latency, s.N);

	/* release control takes effect */
	release = 2.0f;
	d->run(h, 16);
	leq_set_release(&s, 2);

	d->cleanup(h);
	for (int c = 0; c < nch; c++) {
		free(pin[c]);
		if (!inplace) free(pout[c]);
	}
	leq_free(&s);
	free(in);
	free(ref);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "build/pw_loudness_eq.so";
	void *lib = dlopen(path, RTLD_NOW);
	if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
	const LADSPA_Descriptor *(*get)(unsigned long) =
		(const LADSPA_Descriptor *(*)(unsigned long))dlsym(lib, "ladspa_descriptor");
	if (!get) { printf("no ladspa_descriptor\n"); return 1; }

	int n = 0;
	for (const LADSPA_Descriptor *d; (d = get((unsigned long)n)) != NULL; n++) {
		const int nch = n + 1;
		char want[32];
		snprintf(want, sizeof(want), "loudness_eq_%dch", nch);
		CHECK(strcmp(d->Label, want) == 0, "label %s != %s", d->Label, want);
		CHECK(d->PortCount == (unsigned long)(2 * nch + 2), "port count");
		CHECK(strcmp(d->PortNames[2 * nch], "Release") == 0, "Release port name");
		CHECK(strcmp(d->PortNames[2 * nch + 1], "latency") == 0, "latency port name");
		const LADSPA_PortRangeHint *rh = &d->PortRangeHints[2 * nch];
		const float def = 0.5f * (rh->LowerBound + rh->UpperBound);  /* DEFAULT_MIDDLE */
		CHECK(def == 4.0f, "Release default %g != 4", def);
		test_label(d, nch, 48000, 0);
		if (nch == 2) {
			test_label(d, nch, 44100, 1);
			test_label(d, nch, 96000, 0);
		}
	}
	CHECK(n == 8, "expected 8 labels, got %d", n);
	printf("\n%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
	return failures != 0;
}
