#include <string.h>

#include "alac.h"

// ALAC element tags (ALACAudioTypes.h)
#define ID_CPE 1
#define ID_END 7

struct bitwriter
{
  uint8_t *p;
  uint64_t acc;
  int nbits;
};

static void
bits_put(struct bitwriter *bw, uint32_t value, int n)
{
  bw->acc = (bw->acc << n) | (value & ((n == 32) ? 0xffffffffu : ((1u << n) - 1)));
  bw->nbits += n;
  while (bw->nbits >= 8)
    {
      bw->nbits -= 8;
      *bw->p++ = (uint8_t)(bw->acc >> bw->nbits);
    }
}

static void
bits_flush(struct bitwriter *bw)
{
  if (bw->nbits > 0)
    {
      *bw->p++ = (uint8_t)(bw->acc << (8 - bw->nbits));
      bw->nbits = 0;
    }
}

size_t
alac_frame_write(uint8_t *out, const int32_t *samples, int nsamples)
{
  struct bitwriter bw = { .p = out };
  int partial = (nsamples != ALAC_FRAME_SAMPLES);
  int i;

  bits_put(&bw, ID_CPE, 3);
  bits_put(&bw, 0, 4);  // element instance tag
  bits_put(&bw, 0, 12); // unused
  // hasSize (partial frame), 2 bits of shift (none), isNotCompressed
  bits_put(&bw, (partial << 3) | 1, 4);
  if (partial)
    bits_put(&bw, (uint32_t)nsamples, 32);

  for (i = 0; i < nsamples * 2; i++)
    bits_put(&bw, (uint32_t)(samples[i] >> 8), ALAC_BIT_DEPTH);

  bits_put(&bw, ID_END, 3);
  bits_flush(&bw);

  return (size_t)(bw.p - out);
}
