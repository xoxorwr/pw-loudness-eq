/* test-ladspa.c - the limiter plugin against the core DSP, volumes included. */
#include "../src/ladspa-abi.h"
#include "../src/limiter.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void run_one(const LADSPA_Descriptor *d, unsigned rate, int inplace)
{
	const int nch = (int)(long)d->ImplementationData, total = 48000;
	float *in = malloc(sizeof(float) * total * nch), *ref = malloc(sizeof(float) * total * nch);
	float *pin[64], *pout[64], vol[64], latency = -1;

	for (int i = 0; i < total; i++)
		for (int c = 0; c < nch; c++)
			in[i * nch + c] = ((i / 6000) % 2 ? 2.5f : 0.3f) * sinf(i * (0.01f + 0.003f * c));
	for (int c = 0; c < nch; c++)
		vol[c] = c % 3 == 0 ? 1.0f : c % 3 == 1 ? 0.4f : 1.3f;

	limiter_t l;
	limiter_init(&l, rate, nch);
	limiter_process(&l, in, ref, total, vol);

	LADSPA_Handle h = d->instantiate(d, rate);
	CHECK(h != NULL, "instantiate %s", d->Label);
	for (int c = 0; c < nch; c++) {
		pin[c] = malloc(sizeof(float) * 8192);
		pout[c] = inplace ? pin[c] : malloc(sizeof(float) * 8192);
		d->connect_port(h, (unsigned long)c, pin[c]);
		d->connect_port(h, (unsigned long)(nch + c), pout[c]);
		d->connect_port(h, (unsigned long)(2 * nch + c), &vol[c]);
	}
	d->connect_port(h, (unsigned long)(3 * nch), &latency);
	d->activate(h);

	double err = 0;
	for (int off = 0, n; off < total; off += n) {
		n = (off / 1000) % 7 * 300 + 1;           /* odd block sizes */
		if (n > total - off) n = total - off;
		for (int c = 0; c < nch; c++)
			for (int i = 0; i < n; i++)
				pin[c][i] = in[(off + i) * nch + c];
		d->run(h, (unsigned long)n);
		for (int c = 0; c < nch; c++)
			for (int i = 0; i < n; i++)
				err = fmax(err, fabs(pout[c][i] - ref[(off + i) * nch + c]));
	}
	if (nch <= 2 || nch == 64)
		printf("%-20s rate=%6u %s latency port=%g (L=%u)  max |plugin - core| = %g\n",
		       d->Label, rate, inplace ? "in-place" : "separate", latency, l.L, err);
	CHECK(err == 0.0, "%s output differs from core", d->Label);
	CHECK(latency == (float)l.L, "%s latency %g", d->Label, latency);

	d->cleanup(h);
	for (int c = 0; c < nch; c++) {
		free(pin[c]);
		if (!inplace) free(pout[c]);
	}
	limiter_free(&l);
	free(in); free(ref);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "build/pw_audio_limiter.so";
	void *lib = dlopen(path, RTLD_NOW);
	if (lib == NULL) { printf("FAIL: %s\n", dlerror()); return 1; }
	const LADSPA_Descriptor *(*get)(unsigned long) = (const LADSPA_Descriptor *(*)(unsigned long))dlsym(lib, "ladspa_descriptor");
	CHECK(get != NULL, "no ladspa_descriptor");
	unsigned long n = 0;
	while (get(n)) n++;
	CHECK(n == 64, "%lu labels, expected 64", n);
	for (unsigned long i = 0; i < n; i++) {
		const LADSPA_Descriptor *d = get(i);
		const int nch = (int)i + 1;
		char want[32];
		snprintf(want, sizeof(want), "audio_limiter_%dch", nch);
		CHECK(strcmp(d->Label, want) == 0, "label %s", d->Label);
		CHECK(d->PortCount == (unsigned long)(3 * nch + 1), "%s port count", d->Label);
		CHECK(strcmp(d->PortNames[2 * nch], "Volume 1") == 0, "%s volume port name", d->Label);
		CHECK(strcmp(d->PortNames[3 * nch], "latency") == 0, "%s latency port name", d->Label);
		run_one(d, 48000, (int)(i & 1));
	}
	run_one(get(1), 44100, 0);
	run_one(get(1), 96000, 1);
	printf("\n%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
	return failures != 0;
}
