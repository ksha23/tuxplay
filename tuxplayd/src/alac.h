#ifndef TUXPLAY_ALAC_H
#define TUXPLAY_ALAC_H

#include <stddef.h>
#include <stdint.h>

// ALAC frames in the "escape" (uncompressed) form: a stereo channel-pair
// element carrying the samples verbatim, then an end tag. Every ALAC decoder
// accepts these, and they cost nothing to produce. 352 samples is the classic
// ALAC frame length and the one receivers expect for AirPlay.
#define ALAC_FRAME_SAMPLES 352
#define ALAC_BIT_DEPTH 24

// Upper bound for one stereo 24-bit frame of ALAC_FRAME_SAMPLES samples
#define ALAC_FRAME_BYTES_MAX (8 + ALAC_FRAME_SAMPLES * 2 * 3 + 8)

// Encodes nsamples (<= ALAC_FRAME_SAMPLES) interleaved stereo samples, each a
// signed 32-bit value whose top 24 bits are used. Returns the frame length.
size_t
alac_frame_write(uint8_t *out, const int32_t *samples, int nsamples);

#endif
