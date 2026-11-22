/*
 * limiter.h - the Windows audio engine's output limiter, reimplemented.
 *
 * Ported from AudioEng.dll (Windows 11 x64), class CAudioLimiter: Initialize,
 * AllocateDelayLine, APOProcess, ProcessLimiterBuffer,
 * ComputeMaxFrameValsInBuffer (names from Microsoft's public PDB).
 *
 * Windows puts this limiter on every playback device (audiodg.exe,
 * CPipeInstance::CreateDevicePipeInstance), after the mix and after all
 * software volume, always on. It is what keeps Loudness Equalization's
 * boost (up to +20 dB, most on pure tones such as a whistle) from clipping.
 * It is a look-ahead peak limiter:
 *   - output = input delayed by L frames (16/32/64/128 for rates up to
 *     16k/32k/64k/above), times the gain;
 *   - when a frame entering the delay line peaks above the tracked peak
 *     (initially 0.985), the gain ramps down linearly over the L frames so
 *     it reaches 0.985/peak exactly when that frame leaves;
 *   - the gain only recovers once frames peak at half the tracked peak or
 *     less: it rises by exp(2.205/fs) per frame (~19 dB/s) while the
 *     tracked peak decays by the same factor, until back at 1.
 *
 * Audio: interleaved float32.
 *
 * Volume: PipeWire applies a device's software volume after its filter
 * graphs, the reverse of Windows. limiter_process() therefore takes the
 * per-channel volume that will follow, measures peaks on in*volume (what
 * Windows' limiter sees) and outputs in*gain. Once PipeWire multiplies by
 * the volume, that is exactly Windows' (in*volume)*gain.
 */
#ifndef LIMITER_H
#define LIMITER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct limiter {
	int       nch;
	unsigned  L;          /* look-ahead, frames          (CAudioLimiter+0xb0) */
	float     invL;       /* 1/L                                    (+0xb4) */
	float     gain;       /* current gain, 1 = untouched            (+0xbc) */
	float     target;     /* gain the current ramp heads to         (+0xc0) */
	float     step;       /* per-frame gain decrement of the ramp   (+0xc4) */
	float     peak;       /* tracked peak, initially 0.985          (+0xc8) */
	unsigned  count;      /* frames left in the ramp                (+0xd0) */
	double    rise;       /* exp(2.205/fs): release gain factor     (+0xd8) */
	double    fall;       /* exp(-2.205/fs): tracked peak decay     (+0xe0) */
	float    *delay;      /* L frames, interleaved                  (+0xa8) */
	unsigned  dpos;       /* ring position in the delay line              */
} limiter_t;

/* Returns 0 on success, -1 on bad arguments or allocation failure. */
int  limiter_init(limiter_t *l, unsigned rate, int nch);
void limiter_free(limiter_t *l);
void limiter_reset(limiter_t *l);
/* Process nframes interleaved frames; in and out may alias. volume: the
 * software volume per channel applied after this (0 when muted), or NULL
 * for none. */
void limiter_process(limiter_t *l, const float *in, float *out, int nframes,
		     const float *volume);
/* Latency in frames. */
static inline unsigned limiter_latency(const limiter_t *l) { return l->L; }

#ifdef __cplusplus
}
#endif
#endif
