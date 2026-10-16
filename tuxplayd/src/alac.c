#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "alac.h"
#include "log.h"

#define MOD "alac"

// ALAC element tags (ALACAudioTypes.h)
#define ID_CPE 1
#define ID_END 7

// The decoder settings AirPlay receivers use for this stream: frames of 352,
// 24 bits, and ALAC's standard adaptive-coding constants (pb 40, mb 10, kb 14)
#define COOKIE_PB 40
#define COOKIE_MB 10
#define COOKIE_KB 14
#define COOKIE_MAX_RUN 255

static struct
{
  AVCodecContext *enc;
  AVFrame *in;
  AVPacket *pkt;
  bool compress;  // libavcodec frames decode bit-exact (self-test)
  bool zero_ok;   // our silent frame decodes to silence (self-test)
} A;

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

// The "escape" form: the samples verbatim. Always valid, and the fallback.
static size_t
escape_write(uint8_t *out, const int32_t *samples, int nsamples)
{
  struct bitwriter bw = { .p = out };
  int partial = (nsamples != ALAC_FRAME_SAMPLES);

  bits_put(&bw, ID_CPE, 3);
  bits_put(&bw, 0, 4);  // element instance tag
  bits_put(&bw, 0, 12); // unused
  // hasSize (partial frame), 2 bits of shift (none), isNotCompressed
  bits_put(&bw, (partial << 3) | 1, 4);
  if (partial)
    bits_put(&bw, (uint32_t)nsamples, 32);
  for (int i = 0; i < nsamples * 2; i++)
    bits_put(&bw, (uint32_t)(samples[i] >> 8), ALAC_BIT_DEPTH);
  bits_put(&bw, ID_END, 3);
  bits_flush(&bw);
  return (size_t)(bw.p - out);
}

// A full frame of digital silence, compressed with no shifted bytes, which
// libavcodec's encoder always stores raw for 24-bit audio (740 bytes a frame,
// 0.8 Mbit/s of nothing). Per channel: no predictor, the first residual 0 as
// a one-bit Rice code, then the other 351 as one run of zeros, the run length
// in the escape form (nine 1 bits and 16 raw bits), which does not depend on
// the Rice parameter. 20 bytes.
static size_t
zero_write(uint8_t *out)
{
  struct bitwriter bw = { .p = out };

  bits_put(&bw, ID_CPE, 3);
  bits_put(&bw, 0, 4);
  bits_put(&bw, 0, 12);
  bits_put(&bw, 1 << 3, 4);  // hasSize, no shift, compressed
  bits_put(&bw, ALAC_FRAME_SAMPLES, 32);
  bits_put(&bw, 0, 8);       // mixBits
  bits_put(&bw, 0, 8);       // mixRes: channels independent
  for (int ch = 0; ch < 2; ch++)
    {
      bits_put(&bw, 0, 4);   // prediction mode
      bits_put(&bw, 9, 4);   // denShift (any non-zero)
      bits_put(&bw, 4, 3);   // pbFactor: the cookie's pb as is
      bits_put(&bw, 0, 5);   // no predictor coefficients
    }
  for (int ch = 0; ch < 2; ch++)
    {
      bits_put(&bw, 0, 1);                           // first residual: 0
      bits_put(&bw, 0x1ff, 9);                       // escape
      bits_put(&bw, ALAC_FRAME_SAMPLES - 1, 16);     // a run of 351 zeros
    }
  bits_put(&bw, ID_END, 3);
  bits_flush(&bw);
  return (size_t)(bw.p - out);
}

static size_t
encoder_write(uint8_t *out, const int32_t *samples)
{
  int32_t *l = (int32_t *)A.in->data[0], *r = (int32_t *)A.in->data[1];
  size_t len;

  if (av_frame_make_writable(A.in) < 0)
    return 0;
  for (int i = 0; i < ALAC_FRAME_SAMPLES; i++)
    {
      l[i] = samples[i * 2];
      r[i] = samples[i * 2 + 1];
    }
  if (avcodec_send_frame(A.enc, A.in) < 0 || avcodec_receive_packet(A.enc, A.pkt) < 0)
    return 0;
  len = (A.pkt->size > 0 && (size_t)A.pkt->size <= ALAC_FRAME_BYTES_MAX) ? (size_t)A.pkt->size : 0;
  if (len)
    memcpy(out, A.pkt->data, len);
  av_packet_unref(A.pkt);
  return len;
}

/* -------------------------------- Self-test ------------------------------- */

static void
be32(uint8_t *p, uint32_t v)
{
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// A decoder configured the way the receivers are, from an ALAC magic cookie
static AVCodecContext *
test_decoder(void)
{
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
  AVCodecContext *d = codec ? avcodec_alloc_context3(codec) : NULL;
  uint8_t *c;

  if (!d)
    return NULL;
  c = av_mallocz(36 + AV_INPUT_BUFFER_PADDING_SIZE);
  be32(c, 36);
  memcpy(c + 4, "alac", 4);
  be32(c + 12, ALAC_FRAME_SAMPLES);
  c[17] = ALAC_BIT_DEPTH;
  c[18] = COOKIE_PB;
  c[19] = COOKIE_MB;
  c[20] = COOKIE_KB;
  c[21] = 2;
  c[22] = COOKIE_MAX_RUN >> 8;
  c[23] = COOKIE_MAX_RUN & 0xff;
  be32(c + 32, 48000);
  d->extradata = c;
  d->extradata_size = 36;
  d->sample_rate = 48000;
  av_channel_layout_default(&d->ch_layout, 2);
  if (avcodec_open2(d, codec, NULL) < 0)
    avcodec_free_context(&d);
  return d;
}

// Decodes one frame and compares it with what was encoded
static bool
roundtrip_ok(AVCodecContext *d, const uint8_t *frame, size_t len, const int32_t *samples)
{
  AVPacket *p = av_packet_alloc();
  AVFrame *o = av_frame_alloc();
  bool ok = false;

  if (p && o && av_new_packet(p, (int)len) == 0)
    {
      memcpy(p->data, frame, len);
      if (avcodec_send_packet(d, p) == 0 && avcodec_receive_frame(d, o) == 0 &&
          o->nb_samples == ALAC_FRAME_SAMPLES && o->format == AV_SAMPLE_FMT_S32P)
        {
          ok = true;
          for (int i = 0; i < ALAC_FRAME_SAMPLES && ok; i++)
            ok = ((int32_t *)o->data[0])[i] == (samples[i * 2] & ~0xff) &&
                 ((int32_t *)o->data[1])[i] == (samples[i * 2 + 1] & ~0xff);
        }
    }
  av_packet_free(&p);
  av_frame_free(&o);
  return ok;
}

static void
self_test(void)
{
  int32_t pcm[ALAC_FRAME_SAMPLES * 2];
  uint8_t frame[ALAC_FRAME_BYTES_MAX];
  AVCodecContext *d = test_decoder();
  unsigned seed = 1;
  size_t len;

  if (!d)
    {
      log_warn(MOD, "No ALAC decoder to check frames with; sending them uncompressed");
      A.compress = false;
      return;
    }

  memset(pcm, 0, sizeof(pcm));
  len = zero_write(frame);
  A.zero_ok = roundtrip_ok(d, frame, len, pcm);

  // Music-like and worst-case frames, several in a row
  for (int n = 0; n < 8 && A.compress; n++)
    {
      for (int i = 0; i < ALAC_FRAME_SAMPLES * 2; i++)
        {
          seed = seed * 1103515245 + 12345;
          pcm[i] = (n < 6) ? (int32_t)(4e8 * __builtin_sin((n * ALAC_FRAME_SAMPLES + i / 2) * 0.0654 + (i & 1))) + (int32_t)(seed >> 12) - (1 << 19)
                           : (int32_t)seed;
        }
      len = encoder_write(frame, pcm);
      A.compress = len > 0 && roundtrip_ok(d, frame, len, pcm);
    }
  avcodec_free_context(&d);
}

/* ---------------------------------- API ----------------------------------- */

int
alac_init(void)
{
  const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_ALAC);

  if (codec && (A.enc = avcodec_alloc_context3(codec)))
    {
      A.enc->sample_fmt = AV_SAMPLE_FMT_S32P;
      A.enc->bits_per_raw_sample = ALAC_BIT_DEPTH; // the top 24 bits of each sample
      A.enc->sample_rate = 48000;
      av_channel_layout_default(&A.enc->ch_layout, 2);
      if (avcodec_open2(A.enc, codec, NULL) == 0)
        {
          // The encoder takes any frame up to its default of 4096 and marks the
          // length in the frame; tell the generic layer ours is the norm
          A.enc->frame_size = ALAC_FRAME_SAMPLES;
          A.in = av_frame_alloc();
          A.pkt = av_packet_alloc();
          A.in->nb_samples = ALAC_FRAME_SAMPLES;
          A.in->format = A.enc->sample_fmt;
          av_channel_layout_copy(&A.in->ch_layout, &A.enc->ch_layout);
          A.compress = av_frame_get_buffer(A.in, 0) == 0;
        }
    }

  self_test();
  log_info(MOD, "ALAC frames: %s, silence %s", A.compress ? "compressed" : "uncompressed",
           A.zero_ok ? "as 20-byte frames" : "as ordinary frames");
  return 0;
}

void
alac_close(void)
{
  av_frame_free(&A.in);
  av_packet_free(&A.pkt);
  avcodec_free_context(&A.enc);
}

size_t
alac_frame_write(uint8_t *out, const int32_t *samples, int nsamples)
{
  size_t len;

  if (nsamples == ALAC_FRAME_SAMPLES)
    {
      bool silent = true;

      for (int i = 0; i < nsamples * 2 && silent; i++)
        silent = (samples[i] >> 8) == 0;
      if (silent && A.zero_ok)
        return zero_write(out);
      if (A.compress && (len = encoder_write(out, samples)) > 0)
        return len;
    }
  return escape_write(out, samples, nsamples);
}
