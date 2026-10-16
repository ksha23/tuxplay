#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sodium.h>

#include "util.h"

int64_t
mono_ns(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

void
uuid_make(char out[37])
{
  uint8_t b[16];

  randombytes_buf(b, sizeof(b));
  b[6] = (b[6] & 0x0f) | 0x40; // version 4
  b[8] = (b[8] & 0x3f) | 0x80; // RFC 4122 variant
  snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

uint64_t
random_u64(void)
{
  uint64_t v;

  randombytes_buf(&v, sizeof(v));
  return v;
}

void
mac_format(char out[18], const uint8_t mac[6])
{
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// The default route's interface is not known here without netlink, so take
// the first up, non-loopback interface with a real MAC that also has an IPv4
// address. On a desktop that is the LAN interface.
bool
local_mac_get(uint8_t mac[6])
{
  struct ifaddrs *ifa, *a, *b;
  bool found = false;

  if (getifaddrs(&ifa) < 0)
    return false;

  for (a = ifa; a && !found; a = a->ifa_next)
    {
      struct sockaddr_ll *ll;

      if (!a->ifa_addr || a->ifa_addr->sa_family != AF_PACKET)
        continue;
      if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK))
        continue;
      ll = (struct sockaddr_ll *)a->ifa_addr;
      if (ll->sll_halen != 6)
        continue;

      for (b = ifa; b; b = b->ifa_next)
        {
          if (b->ifa_addr && b->ifa_addr->sa_family == AF_INET && strcmp(b->ifa_name, a->ifa_name) == 0)
            {
              memcpy(mac, ll->sll_addr, 6);
              found = true;
              break;
            }
        }
    }

  freeifaddrs(ifa);
  return found;
}

char *
xstrdup(const char *s)
{
  char *d;

  if (!s)
    return NULL;
  d = strdup(s);
  if (!d)
    abort();
  return d;
}

void *
xcalloc(size_t n, size_t size)
{
  void *p = calloc(n, size);

  if (!p)
    abort();
  return p;
}
