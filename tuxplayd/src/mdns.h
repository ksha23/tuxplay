#ifndef TUXPLAY_MDNS_H
#define TUXPLAY_MDNS_H

#include <stdbool.h>
#include <stdint.h>

#define MDNS_TXT_MAX 48

struct mdns_txt
{
  char *key;
  char *value;
};

// One resolved _airplay._tcp service, as seen on one interface and protocol
struct mdns_service
{
  char *name;     // service instance name, e.g. "White HomePod Left"
  char *host;     // e.g. "White-HomePod-Left.local"
  char *address;  // numeric, IPv4 or IPv6 (link-local carries %ifname)
  bool ipv6;
  int ifindex;
  uint16_t port;
  int ntxt;
  struct mdns_txt txt[MDNS_TXT_MAX];
};

// Called on the main thread from mdns_drain(). service is only valid during
// the call. A removal carries just the name (and ipv6/ifindex).
typedef void (*mdns_service_cb)(const struct mdns_service *svc, bool removed, void *arg);

int
mdns_start(mdns_service_cb cb, void *arg);

void
mdns_stop(void);

int
mdns_notify_fd(void);

void
mdns_drain(void);

const char *
mdns_txt_get(const struct mdns_service *svc, const char *key);

#endif
