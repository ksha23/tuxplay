#ifndef TUXPLAY_PLISTX_H
#define TUXPLAY_PLISTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <plist/plist.h>

// Small helpers over libplist for the binary property lists AirPlay speaks

void
px_set_str(plist_t dict, const char *key, const char *value);

void
px_set_bool(plist_t dict, const char *key, bool value);

// Positive integers that fit in 63 bits
void
px_set_uint(plist_t dict, const char *key, uint64_t value);

// Full 64-bit values (a 2^64 fraction, a clock identity) written as 8-byte
// signed integers. libplist writes values above INT64_MAX as 16-byte
// integers, which some receivers reject; they read these fields back as
// unsigned 64-bit anyway.
void
px_set_u64_as_int(plist_t dict, const char *key, uint64_t value);

void
px_set_data(plist_t dict, const char *key, const void *data, size_t len);

void
px_set(plist_t dict, const char *key, plist_t value);

// Returns a malloc'ed binary plist
int
px_to_bin(plist_t p, uint8_t **out, size_t *len);

plist_t
px_from_bin(const void *data, size_t len);

const char *
px_get_str(plist_t dict, const char *key); // valid while dict lives

bool
px_get_uint(plist_t dict, const char *key, uint64_t *value);

bool
px_get_bool(plist_t dict, const char *key, bool *value);

// One line per key, for logs
char *
px_describe(plist_t p);

#endif
