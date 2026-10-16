#ifndef TUXPLAY_PLAYER_H
#define TUXPLAY_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include <event2/event.h>
#include <json-c/json.h>

#include "devices.h"
#include "netwatch.h"
#include "sink.h"

// Ties outputs, sessions and the audio timeline together. Selecting an output
// sets up a session per member and anchors every member on the same frame.
// Another sender taking the speakers is never fought: tuxplayd stands down and
// takes them back only when new sound starts here.

struct player_config
{
  int lead_ms;        // anchor lead: what the speakers buffer, and the latency heard
  int join_ms;        // how far ahead of the write head a start is anchored
  int pause_ms;       // pause the speakers after this much digital silence; 0 = never, they
                      // stay lit and silence is streamed, as macOS 27 does (the default)
  int idle_release_s; // release the speakers after this much silence, 0 = never (saved setting wins)
};

int
player_init(struct event_base *evbase, const struct player_config *cfg);

void
player_stop(void);

// devices.c calls this when a receiver of the output appears, changes or goes
void
player_output_changed(struct output *out, void *arg);

int
player_output_select(struct output *out, bool selected);

int
player_output_volume(struct output *out, int volume);

// netwatch.c: a local address went or came, or a link went down
void
player_network_changed(enum netwatch_event ev, const char *addr, int ifindex, void *arg);

// The desktop slider moved (sink.c)
void
player_desktop_volume(const struct sink_volume *vol, void *arg);

const char *
player_output_state(const struct output *out);

int
player_lead_ms(void);

int
player_lead_set(int ms);

// A/V correction in ms, negative when the sound is late against the picture
int
player_sync_ms(void);

int
player_sync_set(int ms);

// Release the speakers after this long without sound, 0 = never (default)
int
player_idle_release_s(void);

int
player_idle_release_set(int seconds);

struct json_object *
player_status_json(void);

#endif
