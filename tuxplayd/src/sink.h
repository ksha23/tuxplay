#ifndef TUXPLAY_SINK_H
#define TUXPLAY_SINK_H

#include <stdbool.h>
#include <stdint.h>

// The PipeWire sink. It is a DRIVER node: PipeWire runs the graph when this
// sink says so, and it says so on the network timeline (see timeline.h), one
// quantum at a time. Applications playing to it are therefore paced by the
// speakers' clock, so no buffer between PipeWire and the network ever fills
// or drains, and nothing is ever resampled or skipped to correct for drift.

#define SINK_RATE 48000
#define SINK_CHANNELS 2

struct sink_config
{
  const char *node_name;
  const char *description;
  uint32_t quantum;          // frames per cycle
  int64_t latency_ns;        // what the sink reports to clients (for A/V sync)
};

struct sink_volume
{
  float volume; // 0..1 as the desktop slider shows it (cubic)
  bool mute;
};

// Called on the main thread (via sink_drain()) with samples in order.
// index is the network-timeline sample index of samples[0]; gaps in index
// mean PipeWire missed cycles and the caller should treat them as silence.
typedef void (*sink_audio_cb)(uint64_t index, const int32_t *samples, uint32_t frames, void *arg);
typedef void (*sink_volume_cb)(const struct sink_volume *vol, void *arg);

int
sink_start(const struct sink_config *cfg, sink_audio_cb audio_cb, sink_volume_cb volume_cb, void *arg);

void
sink_stop(void);

// fd that becomes readable when audio or a volume change is waiting
int
sink_notify_fd(void);

// Hands everything waiting to the callbacks. Main thread only.
void
sink_drain(void);

// The sample index the next cycle will start at, and the network time it
// is scheduled for. Thread-safe.
void
sink_position(uint64_t *next_index, int64_t *next_gm_ns, uint32_t *generation);

// Network time of a sample index on the current timeline generation
int64_t
sink_index_to_gm(uint64_t index);

void
sink_latency_set(int64_t latency_ns);

void
sink_volume_set(const struct sink_volume *vol);

#endif
