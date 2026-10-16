#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <sodium.h>

#include "log.h"
#include "sink.h"
#include "stream.h"
#include "util.h"

#define MOD "stream"

// About 3.7 s of frames: covers any anchor round trip, and a speaker that
// joins can be given frames already produced instead of waiting for new ones
#define HISTORY_FRAMES 512

// A receiver this far behind in reading is not keeping up
#define QUEUE_MAX_BYTES (1024 * 1024)

// Block layout on the buffered (type 103) data connection:
//   u16 BE total length, counting these two bytes
//   12 byte header: (marker << 31 | seq & 0x7fffff) BE, rtptime BE, format selector BE
//   ChaCha20-Poly1305 ciphertext, 16 byte tag, last 8 bytes of the 12 byte nonce
// The additional data is the rtptime and format selector, header bytes 4-11.
// For ALAC 48k/24 the format selector is 0x15000000: receivers pick the
// decoder for a buffered stream from that field (owntone-mini measured that
// a zero there plays AAC but not ALAC).
#define HDR_LEN 12
#define TAG_LEN crypto_aead_chacha20poly1305_ietf_ABYTES
#define NONCE_TAIL 8

struct frame
{
  uint64_t index;
  bool valid;
  size_t len;
  uint8_t data[ALAC_FRAME_BYTES_MAX];
};

struct delivery
{
  char *name;
  struct bufferevent *bev;
  uint8_t key[32];
  uint64_t nonce;
  uint64_t next;      // next frame to send
  bool active;
  bool marker;        // first frame after a start carries the marker bit
  bool failed;
  delivery_failed_cb cb;
  void *cb_arg;
  struct delivery *next_d;
};

static struct
{
  uint32_t rtp_base;
  uint64_t head;              // frame being assembled
  bool started;
  int32_t pcm[STREAM_SPF * 2];
  uint32_t fill;              // samples (frames of 2) in pcm
  struct frame history[HISTORY_FRAMES];
  struct delivery *deliveries;
  uint64_t silent_frames;

  bool idle;
  int idle_ms;
  stream_activity_cb activity_cb;
  void *activity_arg;
} st = { .idle = true };

void
stream_activity_cb_set(int idle_ms, stream_activity_cb cb, void *arg)
{
  st.idle_ms = idle_ms;
  st.activity_cb = cb;
  st.activity_arg = arg;
}

void
stream_init(void)
{
  alac_init();
  // A random rtptime origin, as every sender uses
  st.rtp_base = (uint32_t)random_u64();
}

uint64_t
stream_head(void)
{
  return st.head;
}

uint32_t
stream_rtptime(uint64_t frame)
{
  return st.rtp_base + (uint32_t)(frame * STREAM_SPF);
}

int64_t
stream_frame_gm(uint64_t frame)
{
  return sink_index_to_gm(frame * STREAM_SPF);
}

int64_t
stream_silence_ms(void)
{
  return (int64_t)(st.silent_frames * STREAM_SPF * 1000 / SINK_RATE);
}

/* -------------------------------- Delivery -------------------------------- */

static void
delivery_fail(struct delivery *d, const char *why, bool by_peer)
{
  if (d->failed)
    return;
  d->failed = true;
  d->active = false;
  log_warn(MOD, "%s: audio connection %s", d->name, why);
  if (d->cb)
    d->cb(d, by_peer, d->cb_arg);
}

static void
delivery_send(struct delivery *d, const struct frame *f)
{
  uint8_t block[2 + HDR_LEN + ALAC_FRAME_BYTES_MAX + TAG_LEN + NONCE_TAIL];
  uint8_t nonce[12] = { 0 };
  uint8_t *hdr = block + 2;
  uint8_t *ct = hdr + HDR_LEN;
  unsigned long long taglen;
  uint32_t word;
  uint32_t rtptime = stream_rtptime(f->index);
  size_t total = 2 + HDR_LEN + f->len + TAG_LEN + NONCE_TAIL;

  // Bit 31 is set on every block, as owntone-mini always sent it (it reads
  // as the RTP version byte, 0x80); the low 23 bits are the frame sequence
  word = 0x80000000u | ((uint32_t)f->index & 0x7fffff);
  hdr[0] = word >> 24; hdr[1] = word >> 16; hdr[2] = word >> 8; hdr[3] = word;
  hdr[4] = rtptime >> 24; hdr[5] = rtptime >> 16; hdr[6] = rtptime >> 8; hdr[7] = rtptime;
  word = STREAM_FORMAT_ALAC48K_24;
  hdr[8] = word >> 24; hdr[9] = word >> 16; hdr[10] = word >> 8; hdr[11] = word;

  memcpy(nonce + 4, &d->nonce, 8);
  d->nonce++;

  crypto_aead_chacha20poly1305_ietf_encrypt_detached(ct, ct + f->len, &taglen, f->data, f->len,
                                                     hdr + 4, 8, NULL, nonce, d->key);
  memcpy(ct + f->len + TAG_LEN, nonce + 4, NONCE_TAIL);

  block[0] = (uint8_t)(total >> 8);
  block[1] = (uint8_t)total;

  if (d->marker)
    log_info(MOD, "%s: first frame %llu, rtptime %u", d->name, (unsigned long long)f->index, rtptime);
  d->marker = false;

  if (bufferevent_write(d->bev, block, total) < 0)
    delivery_fail(d, "write failed", false);
  else if (evbuffer_get_length(bufferevent_get_output(d->bev)) > QUEUE_MAX_BYTES)
    delivery_fail(d, "not being read by the receiver", false);
}

static void
delivery_event_cb(struct bufferevent *bev, short what, void *arg)
{
  struct delivery *d = arg;

  if (what & BEV_EVENT_EOF)
    delivery_fail(d, "closed by the receiver", true);
  else if (what & BEV_EVENT_ERROR)
    delivery_fail(d, evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()), false);
}

static void
delivery_read_cb(struct bufferevent *bev, void *arg)
{
  // Receivers send nothing we need on this connection; keep it drained
  evbuffer_drain(bufferevent_get_input(bev), evbuffer_get_length(bufferevent_get_input(bev)));
}

struct delivery *
delivery_new(struct event_base *evbase, const char *name, int fd, const uint8_t key[32],
             delivery_failed_cb cb, void *arg)
{
  struct delivery *d = xcalloc(1, sizeof(*d));
  int one = 1;

  d->name = xstrdup(name);
  memcpy(d->key, key, 32);
  d->cb = cb;
  d->cb_arg = arg;

  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  evutil_make_socket_nonblocking(fd);
  d->bev = bufferevent_socket_new(evbase, fd, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(d->bev, delivery_read_cb, NULL, delivery_event_cb, d);
  bufferevent_enable(d->bev, EV_READ | EV_WRITE);

  d->next_d = st.deliveries;
  st.deliveries = d;
  return d;
}

void
delivery_start(struct delivery *d, uint64_t from)
{
  uint64_t oldest = (st.head > HISTORY_FRAMES - 1) ? st.head - (HISTORY_FRAMES - 1) : 0;
  uint64_t i;

  if (from < oldest)
    {
      log_warn(MOD, "%s: frame %llu no longer in history, starting at %llu", d->name,
               (unsigned long long)from, (unsigned long long)oldest);
      from = oldest;
    }

  d->next = from;
  d->active = true;
  d->marker = true;

  // Catch up on frames made while the anchor was in flight
  for (i = from; i < st.head && d->active; i++)
    {
      struct frame *f = &st.history[i % HISTORY_FRAMES];
      if (f->valid && f->index == i)
        delivery_send(d, f);
      d->next = i + 1;
    }
  if (from < st.head)
    log_debug(MOD, "%s: caught up %llu frames from history", d->name, (unsigned long long)(st.head - from));
}

void
delivery_stop(struct delivery *d)
{
  d->active = false;
}

void
delivery_free(struct delivery *d)
{
  struct delivery **p;

  if (!d)
    return;
  for (p = &st.deliveries; *p; p = &(*p)->next_d)
    if (*p == d)
      {
        *p = d->next_d;
        break;
      }
  bufferevent_free(d->bev);
  sodium_memzero(d->key, sizeof(d->key));
  free(d->name);
  free(d);
}

size_t
delivery_queued(const struct delivery *d)
{
  return evbuffer_get_length(bufferevent_get_output(d->bev));
}

/* --------------------------------- Frames --------------------------------- */

static void
frame_complete(void)
{
  struct frame *f = &st.history[st.head % HISTORY_FRAMES];
  struct delivery *d, *next;
  bool silent = true;
  uint32_t i;

  for (i = 0; i < STREAM_SPF * 2; i++)
    if (st.pcm[i])
      {
        silent = false;
        break;
      }
  st.silent_frames = silent ? st.silent_frames + 1 : 0;

  // Digital silence is what a Mac's AirPlay driver idles on too: a paused
  // player often keeps writing zeroes rather than stopping its stream
  if (!silent && st.idle)
    {
      st.idle = false;
      if (st.activity_cb)
        st.activity_cb(true, st.head, st.activity_arg);
    }
  else if (silent && !st.idle && st.idle_ms > 0 && stream_silence_ms() >= st.idle_ms)
    {
      st.idle = true;
      if (st.activity_cb)
        st.activity_cb(false, st.head, st.activity_arg);
    }

  f->index = st.head;
  f->len = alac_frame_write(f->data, st.pcm, STREAM_SPF);
  f->valid = true;

  for (d = st.deliveries; d; d = next)
    {
      next = d->next_d;
      if (d->active && d->next == st.head)
        {
          delivery_send(d, f);
          d->next = st.head + 1;
        }
    }

  st.head++;
  st.fill = 0;
}

void
stream_audio_cb(uint64_t index, const int32_t *samples, uint32_t frames, void *arg)
{
  uint64_t expect;
  uint32_t n;

  if (!st.started)
    {
      // Start on a frame boundary
      st.head = (index + STREAM_SPF - 1) / STREAM_SPF;
      st.fill = 0;
      st.started = true;
    }

  expect = st.head * STREAM_SPF + st.fill;
  if (index > expect)
    {
      uint64_t gap = index - expect;

      // A short hole (a missed cycle) becomes silence so frame timing stays
      // exact; a long one restarts frame assembly at the next boundary
      if (gap < SINK_RATE)
        {
          while (gap > 0)
            {
              n = (uint32_t)((gap < (uint64_t)(STREAM_SPF - st.fill)) ? gap : (uint64_t)(STREAM_SPF - st.fill));
              memset(&st.pcm[st.fill * 2], 0, n * 2 * sizeof(int32_t));
              st.fill += n;
              gap -= n;
              if (st.fill == STREAM_SPF)
                frame_complete();
            }
        }
      else
        {
          log_warn(MOD, "%llu samples missing, restarting at the next frame", (unsigned long long)gap);
          st.head = (index + STREAM_SPF - 1) / STREAM_SPF;
          st.fill = 0;
        }
    }
  else if (index < expect)
    {
      // Already have these
      uint64_t skip = expect - index;
      if (skip >= frames)
        return;
      samples += skip * 2;
      frames -= (uint32_t)skip;
      index = expect;
    }

  // Samples before the first frame boundary are dropped
  if (index < st.head * STREAM_SPF)
    {
      uint64_t skip = st.head * STREAM_SPF - index;
      if (skip >= frames)
        return;
      samples += skip * 2;
      frames -= (uint32_t)skip;
    }

  while (frames > 0)
    {
      n = STREAM_SPF - st.fill;
      if (n > frames)
        n = frames;
      memcpy(&st.pcm[st.fill * 2], samples, n * 2 * sizeof(int32_t));
      st.fill += n;
      samples += n * 2;
      frames -= n;
      if (st.fill == STREAM_SPF)
        frame_complete();
    }
}
