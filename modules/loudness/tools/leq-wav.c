/* leq-wav.c - run the Loudness Equalization DSP over a WAV file.
 *
 *   leq-wav [-r release] [-c] in.wav out.wav
 *
 * Input: PCM 16/24/32-bit or IEEE float 32-bit, any channel count / rate.
 * Output: IEEE float 32-bit WAV, same rate/channels. Output is shifted back by
 * the algorithm's latency (N frames) so it lines up with the input; use -c to
 * keep the raw, delayed stream. */
#include "../src/leq.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void wr_hdr(FILE *f, unsigned rate, unsigned nch, uint32_t frames)
{
	const uint32_t data = frames * nch * 4;
	uint8_t h[44];
	memcpy(h, "RIFF", 4);
	uint32_t v = 36 + data;               memcpy(h + 4, &v, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	v = 16;                               memcpy(h + 16, &v, 4);
	uint16_t s = 3;                       memcpy(h + 20, &s, 2);   /* IEEE float */
	s = (uint16_t)nch;                    memcpy(h + 22, &s, 2);
	v = rate;                             memcpy(h + 24, &v, 4);
	v = rate * nch * 4;                   memcpy(h + 28, &v, 4);
	s = (uint16_t)(nch * 4);              memcpy(h + 32, &s, 2);
	s = 32;                               memcpy(h + 34, &s, 2);
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &data, 4);
	fwrite(h, 1, 44, f);
}

int main(int argc, char **argv)
{
	unsigned release = LEQ_RELEASE_DEFAULT;
	int raw = 0, opt;
	while ((opt = getopt(argc, argv, "r:c")) != -1) {
		if (opt == 'r') release = (unsigned)atoi(optarg);
		else if (opt == 'c') raw = 1;
		else goto usage;
	}
	if (argc - optind != 2) {
usage:
		fprintf(stderr, "usage: %s [-r release 0..7] [-c] in.wav out.wav\n", argv[0]);
		return 2;
	}

	FILE *f = fopen(argv[optind], "rb");
	if (!f) { perror(argv[optind]); return 1; }
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)fsz);
	if (!buf || fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) { fprintf(stderr, "read error\n"); return 1; }
	fclose(f);
	if (fsz < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { fprintf(stderr, "not a WAV file\n"); return 1; }

	unsigned fmt = 0, nch = 0, rate = 0, bits = 0;
	const uint8_t *data = NULL;
	uint32_t dsize = 0;
	for (long p = 12; p + 8 <= fsz;) {
		const uint32_t csz = rd32(buf + p + 4);
		const uint8_t *c = buf + p + 8;
		if (!memcmp(buf + p, "fmt ", 4) && csz >= 16) {
			fmt = rd16(c); nch = rd16(c + 2); rate = rd32(c + 4); bits = rd16(c + 14);
			if (fmt == 0xfffe && csz >= 26) fmt = rd16(c + 24);   /* WAVE_FORMAT_EXTENSIBLE */
		} else if (!memcmp(buf + p, "data", 4)) {
			data = c;
			dsize = (uint32_t)((long)csz < fsz - (p + 8) ? (long)csz : fsz - (p + 8));
			break;
		}
		p += 8 + csz + (csz & 1);
	}
	if (!data || !nch || !rate || !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) {
		fprintf(stderr, "unsupported WAV (need PCM16/24/32 or float32)\n");
		return 1;
	}

	const unsigned bps = bits / 8;
	const uint32_t frames = dsize / (bps * nch);
	float *in = calloc((size_t)frames * nch, sizeof(float));
	for (size_t i = 0; i < (size_t)frames * nch; i++) {
		const uint8_t *s = data + i * bps;
		if (fmt == 3) { float v; memcpy(&v, s, 4); in[i] = v; }
		else if (bits == 16) in[i] = (int16_t)rd16(s) / 32768.0f;
		else if (bits == 24) in[i] = (int32_t)((uint32_t)(s[0] << 8 | s[1] << 16 | (uint32_t)s[2] << 24)) / 2147483648.0f;
		else in[i] = (int32_t)rd32(s) / 2147483648.0f;
	}

	leq_t s;
	if (leq_init(&s, rate, (int)nch, release) < 0) { fprintf(stderr, "leq_init failed\n"); return 1; }
	const uint32_t lat = raw ? 0 : leq_latency(&s);
	const uint32_t total = frames + lat;
	float *pad = calloc((size_t)total * nch, sizeof(float));
	float *out = calloc((size_t)total * nch, sizeof(float));
	memcpy(pad, in, (size_t)frames * nch * sizeof(float));
	leq_process(&s, pad, out, (int)total);

	FILE *o = fopen(argv[optind + 1], "wb");
	if (!o) { perror(argv[optind + 1]); return 1; }
	wr_hdr(o, rate, nch, frames);
	fwrite(out + (size_t)lat * nch, sizeof(float), (size_t)frames * nch, o);
	fclose(o);

	fprintf(stderr, "%u Hz, %u ch, %u frames, N=%u, latency %u, release=%u, final gain %.2f dB\n",
		rate, nch, frames, s.N, leq_latency(&s), release, 20.0 * log10(s.gain));
	leq_free(&s);
	return 0;
}
