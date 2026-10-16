#ifndef TUXPLAY_LOG_H
#define TUXPLAY_LOG_H

#include <stddef.h>

enum log_level
{
  LOG_ERR = 0,
  LOG_WARN,
  LOG_INFO,
  LOG_DEBUG,
};

extern int log_threshold;

void
log_msg(enum log_level level, const char *module, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

void
log_hex(const char *module, const char *label, const void *data, size_t len);

#define log_err(m, ...)   log_msg(LOG_ERR, m, __VA_ARGS__)
#define log_warn(m, ...)  log_msg(LOG_WARN, m, __VA_ARGS__)
#define log_info(m, ...)  log_msg(LOG_INFO, m, __VA_ARGS__)
#define log_debug(m, ...) do { if (log_threshold >= LOG_DEBUG) log_msg(LOG_DEBUG, m, __VA_ARGS__); } while (0)

#endif
