/*
 * ladspa-leq.c - Loudness Equalization as a LADSPA plugin (pw_loudness_eq.so).
 *
 * One label per channel count, loudness_eq_1ch .. loudness_eq_8ch, so a
 * stream with N channels runs a single instance. All channels then share one
 * gain, as in the Windows APO (band energies are summed across channels).
 *
 * Ports for label loudness_eq_<N>ch:
 *   0 .. N-1     "In 1" .. "In N"     audio input
 *   N .. 2N-1    "Out 1" .. "Out N"   audio output
 *   2N           "Release"            control input, 0..8, default 4 (Windows default)
 *   2N+1         "latency"            control output, samples (PipeWire reads this name)
 */
#include "ladspa-abi.h"
#include "leq.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CH      8
#define CHUNK       1024
#define UNIQUE_BASE 4917000

#define EXPORT __attribute__((visibility("default")))

typedef struct {
	leq_t leq;
	int ok;
	unsigned long rate;
	int nch;
	LADSPA_Data *in[MAX_CH], *out[MAX_CH];
	LADSPA_Data *release, *latency;
	unsigned cur_release;
	float ibuf[CHUNK * MAX_CH];
	float obuf[CHUNK * MAX_CH];
} inst_t;

static LADSPA_Descriptor descs[MAX_CH];
static LADSPA_PortDescriptor port_desc[MAX_CH][2 * MAX_CH + 2];
static const char *port_names[MAX_CH][2 * MAX_CH + 2];
static LADSPA_PortRangeHint port_hints[MAX_CH][2 * MAX_CH + 2];
static char labels[MAX_CH][32], names[MAX_CH][64];
static char in_names[MAX_CH][16], out_names[MAX_CH][16];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static unsigned release_from(const LADSPA_Data *p)
{
	if (p == NULL || !isfinite(*p))
		return LEQ_RELEASE_DEFAULT;
	float v = roundf(*p);
	return v < 0 ? 0 : v > 7 ? 7 : (unsigned)v;
}

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
	inst_t *s = calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;
	s->rate = rate;
	s->nch = (int)(intptr_t)d->ImplementationData;
	s->cur_release = LEQ_RELEASE_DEFAULT;
	s->ok = leq_init(&s->leq, (unsigned)rate, s->nch, s->cur_release) == 0;
	if (!s->ok) {
		free(s);
		return NULL;
	}
	return s;
}

static void connect_port(LADSPA_Handle h, unsigned long port, LADSPA_Data *data)
{
	inst_t *s = h;
	const unsigned long n = (unsigned long)s->nch;
	if (port < n)
		s->in[port] = data;
	else if (port < 2 * n)
		s->out[port - n] = data;
	else if (port == 2 * n)
		s->release = data;
	else if (port == 2 * n + 1)
		s->latency = data;
}

static void activate(LADSPA_Handle h)
{
	/* reset to the freshly-initialised state (gain 10, empty buffers) */
	inst_t *s = h;
	leq_free(&s->leq);
	s->cur_release = release_from(s->release);
	s->ok = leq_init(&s->leq, (unsigned)s->rate, s->nch, s->cur_release) == 0;
	if (s->latency)
		*s->latency = (LADSPA_Data)(s->ok ? leq_latency(&s->leq) : 0);
}

static void run(LADSPA_Handle h, unsigned long n_samples)
{
	inst_t *s = h;
	const int nch = s->nch;

	if (s->latency)
		*s->latency = (LADSPA_Data)(s->ok ? leq_latency(&s->leq) : 0);

	if (!s->ok) {
		for (int c = 0; c < nch; c++)
			if (s->out[c] && s->in[c] && s->out[c] != s->in[c])
				memmove(s->out[c], s->in[c], n_samples * sizeof(float));
		return;
	}

	const unsigned r = release_from(s->release);
	if (r != s->cur_release) {
		s->cur_release = r;
		leq_set_release(&s->leq, r);
	}

	for (unsigned long off = 0; off < n_samples; off += CHUNK) {
		const unsigned long n = n_samples - off < CHUNK ? n_samples - off : CHUNK;
		for (int c = 0; c < nch; c++) {
			const LADSPA_Data *src = s->in[c];
			for (unsigned long i = 0; i < n; i++)
				s->ibuf[i * nch + c] = src ? src[off + i] : 0.0f;
		}
		leq_process(&s->leq, s->ibuf, s->obuf, (int)n);
		for (int c = 0; c < nch; c++) {
			LADSPA_Data *dst = s->out[c];
			if (dst == NULL)
				continue;
			for (unsigned long i = 0; i < n; i++)
				dst[off + i] = s->obuf[i * nch + c];
		}
	}
}

static void cleanup(LADSPA_Handle h)
{
	inst_t *s = h;
	leq_free(&s->leq);
	free(s);
}

static void init_descriptors(void)
{
	for (int i = 0; i < MAX_CH; i++) {
		snprintf(in_names[i], sizeof(in_names[i]), "In %d", i + 1);
		snprintf(out_names[i], sizeof(out_names[i]), "Out %d", i + 1);
	}
	for (int k = 0; k < MAX_CH; k++) {
		const int n = k + 1;
		LADSPA_Descriptor *d = &descs[k];
		int p = 0;
		for (int c = 0; c < n; c++, p++) {
			port_desc[k][p] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
			port_names[k][p] = in_names[c];
		}
		for (int c = 0; c < n; c++, p++) {
			port_desc[k][p] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
			port_names[k][p] = out_names[c];
		}
		port_desc[k][p] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
		port_names[k][p] = "Release";
		/* 0..8 with DEFAULT_MIDDLE gives 4; 8 behaves exactly like 7 */
		port_hints[k][p] = (LADSPA_PortRangeHint){
			LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE |
			LADSPA_HINT_INTEGER | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 8.0f };
		p++;
		port_desc[k][p] = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
		port_names[k][p] = "latency";
		p++;

		snprintf(labels[k], sizeof(labels[k]), "loudness_eq_%dch", n);
		snprintf(names[k], sizeof(names[k]), "Loudness Equalization (%d ch)", n);
		*d = (LADSPA_Descriptor){
			.UniqueID = UNIQUE_BASE + (unsigned long)n,
			.Label = labels[k],
			.Properties = LADSPA_PROPERTY_REALTIME | LADSPA_PROPERTY_HARD_RT_CAPABLE,
			.Name = names[k],
			.Maker = "pw-loudness-eq",
			.Copyright = "MIT",
			.PortCount = (unsigned long)p,
			.PortDescriptors = port_desc[k],
			.PortNames = port_names[k],
			.PortRangeHints = port_hints[k],
			.ImplementationData = (void *)(intptr_t)n,
			.instantiate = instantiate,
			.connect_port = connect_port,
			.activate = activate,
			.run = run,
			.cleanup = cleanup,
		};
	}
}

EXPORT const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
	pthread_once(&once, init_descriptors);
	return index < MAX_CH ? &descs[index] : NULL;
}
