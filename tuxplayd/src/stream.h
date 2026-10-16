#ifndef TUXPLAY_STREAM_H
#define TUXPLAY_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <event2/event.h>

#include "alac.h"

// The audio timeline. Sample index n from the sink belongs to frame
// n / STREAM_SPF, and every frame has one rtptime and one network time, the
// same for every receiver. An anchor is therefore just "frame m plays at
// network time T(m) + lead": two speakers given the same frame start
// together, and a speaker added later joins in step with the rest.

#define STREAM_SPF ALAC_FRAME_SAMPLES
// Per-block format selector for ALAC 48 kHz/24-bit, see stream.c
#define STREAM_FORMAT_ALAC48K_24 0x15000000u

struct delivery;

// The input turned audible (frame is the first audible one) or has been
// digital silence for idle_ms (frame is the next to be made; idle_ms 0 means
// never). Main thread.
typedef void (*stream_activity_cb)(bool audible, uint64_t frame, void *arg);

// by_peer: the receiver closed the connection (EOF) rather than an error
typedef void (*delivery_failed_cb)(struct delivery *d, bool by_peer, void *arg);

void
stream_init(void);

void
stream_activity_cb_set(int idle_ms, stream_activity_cb cb, void *arg);

// Sink callback (see sink.h)
void
stream_audio_cb(uint64_t index, const int32_t *samples, uint32_t frames, void *arg);

// The next frame to be completed
uint64_t
stream_head(void);

uint32_t
stream_rtptime(uint64_t frame);

// Network time at which the frame's first sample was produced
int64_t
stream_frame_gm(uint64_t frame);

// Milliseconds the input has been digital silence, 0 while there is sound
int64_t
stream_silence_ms(void);

// One receiver's buffered data connection. key is the stream key (shk).
struct delivery *
delivery_new(struct event_base *evbase, const char *name, int fd, const uint8_t key[32],
             delivery_failed_cb cb, void *arg);

// Send frames from 'from' on: those already produced straight away from the
// history, the rest as they are made
void
delivery_start(struct delivery *d, uint64_t from);

void
delivery_stop(struct delivery *d);

void
delivery_free(struct delivery *d);

size_t
delivery_queued(const struct delivery *d);

#endif
