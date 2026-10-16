#ifndef TUXPLAY_DEVICES_H
#define TUXPLAY_DEVICES_H

#include <stdbool.h>
#include <stdint.h>

#include "mdns.h"

// AirPlay feature bits that matter here (the _airplay._tcp "features" field)
#define FEATURE_AUDIO           (1ULL << 9)
#define FEATURE_BUFFERED_AUDIO  (1ULL << 40)
#define FEATURE_PTP             (1ULL << 41)
#define FEATURE_HK_PAIRING      (1ULL << 46)
#define FEATURE_TRANSIENT_PAIR  (1ULL << 48)

struct session;
struct output;

// One AirPlay receiver: a single speaker, one half of a stereo pair, an Apple TV
struct receiver
{
  char *name;
  char *deviceid;
  char *model;
  char *srcvers;
  char *pi;
  char *gid;
  char *tsid;
  char *gpn;
  uint64_t features;
  uint32_t flags;
  bool igl;
  bool gcgl;

  char *addr4;
  char *addr6;
  uint16_t port;
  bool present;
  // Interfaces the service is currently seen on, one bit per ifindex, per
  // protocol: a receiver is only gone once no interface sees it
  uint64_t seen4;
  uint64_t seen6;

  struct output *output;
  struct session *session;
  struct receiver *next;
};

// What a user picks: a stereo pair as one output, or a single receiver
struct output
{
  char id[24];      // stable across restarts, decimal
  char *name;
  bool is_pair;
  struct receiver *members[2];
  int nmembers;

  bool selected;
  bool claimed;     // tuxplayd is to hold these speakers now (player.c)
  int volume;       // 0..100, as the desktop slider shows it
  int offset_ms;    // per-output delay correction, normally 0

  struct output *next;
};

typedef void (*devices_changed_cb)(struct output *out, void *arg);

int
devices_init(const char *state_path, devices_changed_cb cb, void *arg);

void
devices_mdns_cb(const struct mdns_service *svc, bool removed, void *arg);

struct output *
devices_outputs(void);

struct output *
devices_output_find(const char *id);

struct receiver *
devices_receivers(void);

// A stereo pair is ready to start once both halves are present
bool
output_ready(const struct output *out);

// Every member has a last known address, present or not: enough to try
// reconnecting an output whose sessions our own network change cut
bool
output_reachable(const struct output *out);

// The member volume goes to. macOS sends a pair's volume to one member, its
// "volume leader", and lets the pair carry it to the other.
struct receiver *
output_volume_leader(const struct output *out);

bool
receiver_usable(const struct receiver *r);

const char *
receiver_address(const struct receiver *r);

void
devices_state_save(void);

// Daemon settings kept in the same state file
int
devices_setting_get(const char *key, int def);

void
devices_setting_set(const char *key, int value);

#endif
