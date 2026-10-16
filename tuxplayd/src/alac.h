#ifndef TUXPLAY_ALAC_H
#define TUXPLAY_ALAC_H

#include <stddef.h>
#include <stdint.h>

// ALAC frames of 352 stereo samples, the length receivers expect for AirPlay.
// Compressed with libavcodec's encoder, digital silence as a 20-byte frame, and
// the uncompressed "escape" form as the fallback. alac_init() checks the first
// two with libavcodec's decoder and drops whichever does not decode bit-exact.
#define ALAC_FRAME_SAMPLES 352
#define ALAC_BIT_DEPTH 24

// Upper bound for one stereo 24-bit frame of ALAC_FRAME_SAMPLES samples
#define ALAC_FRAME_BYTES_MAX (8 + ALAC_FRAME_SAMPLES * 2 * 3 + 8)

// Encodes nsamples (<= ALAC_FRAME_SAMPLES) interleaved stereo samples, each a
// signed 32-bit value whose top 24 bits are used. Returns the frame length.
int
alac_init(void);

void
alac_close(void);

size_t
alac_frame_write(uint8_t *out, const int32_t *samples, int nsamples);

#endif
