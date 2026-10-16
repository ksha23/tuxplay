#ifndef TUXPLAY_NETWATCH_H
#define TUXPLAY_NETWATCH_H

#include <event2/event.h>

// The kernel's view of local network changes, so sessions on an address or
// link that just went away can move to whatever is still up at once, instead
// of waiting for a keep-alive to time out.

enum netwatch_event
{
  NETWATCH_ADDR_GONE, // addr was removed from ifindex
  NETWATCH_LINK_DOWN, // ifindex is down or lost its carrier, addr is NULL
  NETWATCH_ADDR_NEW,  // addr appeared on ifindex: the network may be back
};

typedef void (*netwatch_cb)(enum netwatch_event ev, const char *addr, int ifindex, void *arg);

int
netwatch_start(struct event_base *evbase, netwatch_cb cb, void *arg);

void
netwatch_stop(void);

// The interface a local address is on, 0 if none has it (any more)
int
netwatch_ifindex_of(const char *addr);

#endif
