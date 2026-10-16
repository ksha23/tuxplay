#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <time.h>

#include "log.h"

int log_threshold = LOG_INFO;

static const char *level_names[] = { "E", "W", "I", "D" };

void
log_msg(enum log_level level, const char *module, const char *fmt, ...)
{
  struct timespec ts;
  struct tm tm;
  va_list ap;

  if ((int)level > log_threshold)
    return;

  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &tm);

  // One locked write per line, so lines from different threads never interleave
  flockfile(stderr);
  fprintf(stderr, "%02d:%02d:%02d.%03ld %s %-8s ", tm.tm_hour, tm.tm_min, tm.tm_sec,
          ts.tv_nsec / 1000000, level_names[level], module);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  funlockfile(stderr);
}

void
log_hex(const char *module, const char *label, const void *data, size_t len)
{
  const unsigned char *p = data;
  char line[16 * 3 + 1];
  size_t i, j;

  if (log_threshold < LOG_DEBUG)
    return;

  log_msg(LOG_DEBUG, module, "%s (%zu bytes)", label, len);
  for (i = 0; i < len; i += 16)
    {
      for (j = 0; j < 16 && i + j < len; j++)
        snprintf(line + 3 * j, 4, "%02x ", p[i + j]);
      log_msg(LOG_DEBUG, module, "  %04zx  %s", i, line);
    }
}
