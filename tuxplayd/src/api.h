#ifndef TUXPLAY_API_H
#define TUXPLAY_API_H

#include <stdint.h>

#include <event2/event.h>

// Local HTTP API. The paths the tuxplay tools have always used (they were
// written against owntone's API) keep working:
//   GET  /api/outputs              outputs with id, name, type, selected, volume
//   PUT  /api/outputs/{id}         {"selected": bool, "volume": 0..100}
//   GET  /api/player/silence       {"silent_msec": N}
//   PUT  /api/player/active|idle   accepted, the daemon decides this itself now
// and tuxplayd adds:
//   GET  /api/status               timeline, latency, sessions
//   PUT  /api/latency              {"ms": N}

int
api_start(struct event_base *evbase, const char *address, uint16_t port);

void
api_stop(void);

#endif
