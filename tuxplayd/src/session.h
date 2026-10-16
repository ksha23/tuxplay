#ifndef TUXPLAY_SESSION_H
#define TUXPLAY_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include <event2/event.h>

#include "devices.h"

// One AirPlay 2 session with one receiver: connect, pair (HomeKit transient),
// set up a PTP-timed buffered audio stream, then wait to be anchored.

enum session_state
{
  SESSION_STARTING,  // connecting, pairing, setting up
  SESSION_READY,     // set up and audio connection open, not playing: waiting for an anchor
  SESSION_ANCHORING, // SETRATEANCHORTIME in flight
  SESSION_PLAYING,   // anchored, frames flowing
  SESSION_FAILED,    // gone; the owner frees it
  SESSION_STOPPED,   // torn down at the owner's request; the owner frees it
};

struct session_params
{
  const char *sender_name;  // shown on the receiver
  const char *sender_id;    // our device id, "AA:BB:CC:DD:EE:FF"
  const char *group_uuid;   // shared by everything playing together
  uint64_t clock_id;        // our PTP clock identity
  const char *const *peers; // every receiver playing in the group (may include this one)
  int npeers;
};

struct session;

typedef void (*session_state_cb)(struct session *s, enum session_state state, void *arg);

// NULL when the connection cannot even be attempted
struct session *
session_start(struct event_base *evbase, struct receiver *r, const struct session_params *p,
              session_state_cb cb, void *arg);

// Frame 'frame' is to render at network time render_gm_ns on timeline gm_id.
// Frames from 'frame' on are delivered once the receiver accepts.
int
session_anchor(struct session *s, uint64_t frame, int64_t render_gm_ns, uint64_t gm_id);

// Stop playing (FLUSHBUFFERED) but keep the session: the receiver goes idle
// and other senders can use it. Back in SESSION_READY once it agrees, ready
// for the next session_anchor().
int
session_pause(struct session *s);

// -30..0 dB, or -144 for silent
int
session_volume(struct session *s, double db);

// The group changed: every receiver now playing together (this one may be
// among them). Sent to the receiver as its PTP peer list.
int
session_peers_set(struct session *s, const char *const *addrs, int n);

// Stop sending; ends in SESSION_STOPPED. Free it afterwards, outside the
// state callback.
void
session_stop(struct session *s);

// Closes every connection. Not from within the session's own state callback.
void
session_free(struct session *s);

// The receiver asked us to stand down or hung up on us: another sender took
// it over
bool
session_taken_over(const struct session *s);

enum session_state
session_state(const struct session *s);

const char *
session_state_name(enum session_state st);

struct receiver *
session_receiver(const struct session *s);

const char *
session_local_address(const struct session *s);

// The interface our end is on, 0 if unknown
int
session_ifindex(const struct session *s);

// Our address or link under the session went away: fail it now rather than
// wait for a keep-alive to time out
void
session_network_lost(struct session *s);

bool
session_lost_network(const struct session *s);

#endif
