#ifndef TUXPLAY_UTIL_H
#define TUXPLAY_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_PER_SEC 1000000000LL
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

int64_t
mono_ns(void);

// Random UUID as upper-case text, 36 chars plus NUL
void
uuid_make(char out[37]);

uint64_t
random_u64(void);

// "AB:CD:..." from 6 bytes
void
mac_format(char out[18], const uint8_t mac[6]);

// MAC address of the interface carrying the default route, false if unknown
bool
local_mac_get(uint8_t mac[6]);

char *
xstrdup(const char *s);

void *
xcalloc(size_t n, size_t size);

#endif
