#ifndef TUXPLAY_TIMELINE_H
#define TUXPLAY_TIMELINE_H

#include <stdbool.h>
#include <stdint.h>

// The network timeline: the receivers' PTP grandmaster time, as estimated by
// the vendored libairptp follower. Everything tuxplayd schedules - the PipeWire
// cycle, every anchor - is expressed on it, so audio is produced at exactly
// the rate the speakers render it. A Mac does the same: its AirPlay audio
// device is clocked by the network clock.

enum timeline_state
{
  TIMELINE_OWN,       // no receiver clock yet, our CLOCK_MONOTONIC is the timeline
  TIMELINE_WAITING,   // receivers just added, a better clock may be about to lock
  TIMELINE_FOLLOWING, // locked to a receiver's grandmaster
};

struct timeline_snapshot
{
  enum timeline_state state;
  uint64_t gm_id;     // clock to name in anchors (networkTimeTimelineID)
  int64_t ref_ns;
  int64_t offset_ns;
  double rate;
  uint32_t generation; // changes whenever gm_id or state changes
};

int
timeline_start(uint64_t clock_id);

void
timeline_stop(void);

uint64_t
timeline_clock_id(void);

// Thread-safe. Returns a consistent copy of the current mapping.
void
timeline_get(struct timeline_snapshot *s);

int64_t
timeline_local_to_gm(const struct timeline_snapshot *s, int64_t local_ns);

int64_t
timeline_gm_to_local(const struct timeline_snapshot *s, int64_t gm_ns);

// Receivers to exchange PTP with. Returns a handle for timeline_peer_remove().
int
timeline_peer_add(const char *addr, uint32_t *id);

void
timeline_peer_remove(uint32_t id);

#endif
