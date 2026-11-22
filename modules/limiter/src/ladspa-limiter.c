/*
 * ladspa-limiter.c - the Windows audio engine's output limiter as a LADSPA
 * plugin (pw_audio_limiter.so). See limiter.h.
 *
 * One label per channel count, audio_limiter_1ch .. audio_limiter_64ch
 * (PipeWire's maximum), so a device with N channels runs a single
 * instance and all channels share one gain, as on Windows.
 *
 * Ports for label audio_limiter_<N>ch:
 *   0 .. N-1       "In 1" .. "In N"           audio input
 *   N .. 2N-1      "Out 1" .. "Out N"         audio output
 *   2N .. 3N-1     "Volume 1" .. "Volume N"   control input: the device's
 *                                             software volume per channel,
 *                                             applied after this plugin
 *                                             (0 when muted), default 1
 *   3N             "latency"                  control output, samples
 *                                             (PipeWire reads this name)
 */
#include "ladspa-abi.h"
#include "limiter.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CH      64
#define NPORTS(n)   (3 * (n) + 1)
#define CHUNK       512
#define UNIQUE_BASE 4917100

#define EXPORT __attribute__((visibility("default")))

typedef struct {
	limiter_t lim;
	unsigned long rate;
	int nch;
	LADSPA_Data *in[MAX_CH], *out[MAX_CH], *vol[MAX_CH];
	LADSPA_Data *latency;
	float volume[MAX_CH];
	float buf[CHUNK * MAX_CH];
} inst_t;

static LADSPA_Descriptor descs[MAX_CH];
static LADSPA_PortDescriptor *port_desc[MAX_CH];
static const char **port_names[MAX_CH];
static LADSPA_PortRangeHint *port_hints[MAX_CH];
static char labels[MAX_CH][32], names[MAX_CH][64];
static char in_names[MAX_CH][16], out_names[MAX_CH][16], vol_names[MAX_CH][16];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
	inst_t *s = calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;
	s->rate = rate;
	s->nch = (int)(intptr_t)d->ImplementationData;
	if (limiter_init(&s->lim, (unsigned)rate, s->nch) < 0) {
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
	else if (port < 3 * n)
		s->vol[port - 2 * n] = data;
	else if (port == 3 * n)
		s->latency = data;
}

static void activate(LADSPA_Handle h)
{
	inst_t *s = h;
	limiter_reset(&s->lim);
	if (s->latency)
		*s->latency = (LADSPA_Data)limiter_latency(&s->lim);
}

static void run(LADSPA_Handle h, unsigned long n_samples)
{
	inst_t *s = h;
	const int nch = s->nch;

	if (s->latency)
		*s->latency = (LADSPA_Data)limiter_latency(&s->lim);
	for (int c = 0; c < nch; c++) {
		float v = s->vol[c] ? *s->vol[c] : 1.0f;
		s->volume[c] = isfinite(v) && v > 0.0f ? v : 0.0f;
	}

	for (unsigned long off = 0; off < n_samples; off += CHUNK) {
		const unsigned long n = n_samples - off < CHUNK ? n_samples - off : CHUNK;
		for (int c = 0; c < nch; c++) {
			const LADSPA_Data *src = s->in[c];
			for (unsigned long i = 0; i < n; i++)
				s->buf[i * nch + c] = src ? src[off + i] : 0.0f;
		}
		limiter_process(&s->lim, s->buf, s->buf, (int)n, s->volume);
		for (int c = 0; c < nch; c++) {
			LADSPA_Data *dst = s->out[c];
			if (dst == NULL)
				continue;
			for (unsigned long i = 0; i < n; i++)
				dst[off + i] = s->buf[i * nch + c];
		}
	}
}

static void cleanup(LADSPA_Handle h)
{
	inst_t *s = h;
	limiter_free(&s->lim);
	free(s);
}

static void init_descriptors(void)
{
	for (int i = 0; i < MAX_CH; i++) {
		snprintf(in_names[i], sizeof(in_names[i]), "In %d", i + 1);
		snprintf(out_names[i], sizeof(out_names[i]), "Out %d", i + 1);
		snprintf(vol_names[i], sizeof(vol_names[i]), "Volume %d", i + 1);
	}
	for (int k = 0; k < MAX_CH; k++) {
		const int n = k + 1;
		LADSPA_Descriptor *d = &descs[k];
		int p = 0;

		port_desc[k] = calloc(NPORTS(n), sizeof(*port_desc[k]));
		port_names[k] = calloc(NPORTS(n), sizeof(*port_names[k]));
		port_hints[k] = calloc(NPORTS(n), sizeof(*port_hints[k]));
		if (!port_desc[k] || !port_names[k] || !port_hints[k])
			return;     /* descs[k] stays zeroed: ladspa_descriptor() skips it */

		for (int c = 0; c < n; c++, p++) {
			port_desc[k][p] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
			port_names[k][p] = in_names[c];
		}
		for (int c = 0; c < n; c++, p++) {
			port_desc[k][p] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
			port_names[k][p] = out_names[c];
		}
		for (int c = 0; c < n; c++, p++) {
			port_desc[k][p] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
			port_names[k][p] = vol_names[c];
			/* PipeWire's volume range is 0..10 */
			port_hints[k][p] = (LADSPA_PortRangeHint){
				LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE |
				LADSPA_HINT_DEFAULT_1, 0.0f, 10.0f };
		}
		port_desc[k][p] = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
		port_names[k][p] = "latency";
		p++;

		snprintf(labels[k], sizeof(labels[k]), "audio_limiter_%dch", n);
		snprintf(names[k], sizeof(names[k]), "Windows audio engine limiter (%d ch)", n);
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
	return index < MAX_CH && descs[index].Label ? &descs[index] : NULL;
}
