/*
 * leq.h - Windows "Loudness Equalization" DSP, reimplemented.
 *
 * Ported from WMALFXGFXDSP.dll (Windows 11 x64), functions LEQ_init, lm_init,
 * LEQ_UpdateWeight, LEQ_process, LEQ_block, loudness, fftr, powspec, fft
 * (names from Microsoft's public PDB). The first fields mirror the original
 * state struct; the offsets are noted for reference.
 *
 * Audio: interleaved float32, nominal range [-1, 1].
 * Latency: N frames (N = FFT size; 512 at 44.1/48 kHz).
 */
#ifndef LEQ_H
#define LEQ_H

#ifdef __cplusplus
extern "C" {
#endif

#define LEQ_NBANDS          25
#define LEQ_RELEASE_DEFAULT 4   /* Windows default ("Release Time" slider 2..7) */

typedef struct leq {
	/* --- original layout (struct at corr+0x110 in the DLL) --- */
	float    *in0;     /* +0x00 older input half-frame   (H*nch interleaved)   */
	float    *in1;     /* +0x08 input half-frame being filled                 */
	float    *out0;    /* +0x10 finished output half-frame being emitted      */
	float    *out1;    /* +0x18 partial output (ramp-down part only)          */
	float    *work;    /* +0x20 FFT scratch, N floats                         */
	float     gain;    /* +0x28 current gain (initialised to 10.0)            */
	float     weight;  /* +0x2c release smoothing coefficient                 */
	int       nch;     /* +0x30                                               */
	int       pos;     /* +0x34 fill position inside the current half-frame   */
	unsigned  release; /* +0x38 release-time setting                          */
	unsigned  N;       /* +0x3c FFT size                                      */
	unsigned  edges[LEQ_NBANDS + 1]; /* +0x40 band edges in FFT bins          */
	float    *window;  /* +0xa8 analysis window, N floats                     */

	/* --- additions for this port --- */
	unsigned  rate;
	float    *twiddle; /* N/4 complex twiddles for the N/2-point complex FFT  */
	double    rc, rs;  /* cos/sin(2*pi/N) for the real-FFT post-processing    */
} leq_t;

/* Returns 0 on success, -1 on bad arguments or allocation failure. */
int  leq_init(leq_t *s, unsigned rate, int nch, unsigned release);
void leq_free(leq_t *s);
/* Release-time setting, 2..7 in the Windows UI (0..7 accepted). */
void leq_set_release(leq_t *s, unsigned release);
/* Process nframes interleaved frames. in and out must not alias. */
void leq_process(leq_t *s, const float *in, float *out, int nframes);
/* Latency in frames. */
static inline unsigned leq_latency(const leq_t *s) { return s->N; }

/* Exposed for tests: in-place real FFT (NR "realft" packing) and power spectrum. */
void leq_fftr(leq_t *s, float *d);
void leq_powspec(float *d, int n);

#ifdef __cplusplus
}
#endif
#endif
