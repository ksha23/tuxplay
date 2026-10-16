#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "airptp.h"
#include "log.h"
#include "timeline.h"

#define MOD "ptp"

// How long the timeline stays WAITING after the first receiver is added. A
// HomePod pair locks in about 0.4 s; this only bounds a receiver that offers
// a clock and then never completes an exchange.
#define LOCK_WAIT_MS 3000

static struct airptp_handle *ptp_hdl;
static uint64_t ptp_clock_id;

static pthread_mutex_t gen_lck = PTHREAD_MUTEX_INITIALIZER;
static uint64_t gen_gm_id;
static enum timeline_state gen_state;
static uint32_t gen_counter;

static void
ptp_log(const char *fmt, ...)
{
  char buf[512];
  va_list ap;

  if (log_threshold < LOG_DEBUG)
    return;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_debug(MOD, "%s", buf);
}

static void
ptp_notice(const char *fmt, ...)
{
  char buf[512];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_info(MOD, "%s", buf);
}

int
timeline_start(uint64_t clock_id)
{
  struct airptp_callbacks cb = { .logmsg = ptp_log, .lognotice = ptp_notice };

  airptp_callbacks_register(&cb);

  ptp_hdl = airptp_daemon_bind(NULL);
  if (!ptp_hdl)
    {
      log_err(MOD, "Could not bind PTP ports 319/320: %s", airptp_errmsg_get());
      return -1;
    }

  airptp_role_set(ptp_hdl, AIRPTP_ROLE_FOLLOW, LOCK_WAIT_MS);

  if (airptp_daemon_start(ptp_hdl, clock_id, false) < 0)
    {
      log_err(MOD, "Could not start PTP: %s", airptp_errmsg_get());
      airptp_end(ptp_hdl);
      ptp_hdl = NULL;
      return -1;
    }

  airptp_clock_id_get(&ptp_clock_id, ptp_hdl);
  log_info(MOD, "PTP clock identity %016" PRIx64 ", following the receivers' grandmaster when there is one", ptp_clock_id);
  return 0;
}

void
timeline_stop(void)
{
  airptp_end(ptp_hdl);
  ptp_hdl = NULL;
}

uint64_t
timeline_clock_id(void)
{
  return ptp_clock_id;
}

void
timeline_get(struct timeline_snapshot *s)
{
  struct airptp_timeline tl;
  enum timeline_state state;

  memset(s, 0, sizeof(*s));
  if (!ptp_hdl || airptp_timeline_get(&tl, ptp_hdl) < 0)
    {
      s->state = TIMELINE_OWN;
      s->gm_id = ptp_clock_id;
      return;
    }

  state = (tl.state == AIRPTP_TIMELINE_FOLLOWING) ? TIMELINE_FOLLOWING :
          (tl.state == AIRPTP_TIMELINE_WAITING) ? TIMELINE_WAITING : TIMELINE_OWN;

  s->state = state;
  s->gm_id = tl.clock_id;
  s->ref_ns = tl.ref_ns;
  s->offset_ns = (state == TIMELINE_FOLLOWING) ? tl.offset_ns : 0;
  s->rate = (state == TIMELINE_FOLLOWING) ? tl.rate : 0.0;

  // WAITING is still our own clock as far as time goes; only a change of
  // grandmaster, or between our clock and a grandmaster's, is a new timeline
  pthread_mutex_lock(&gen_lck);
  if (state == TIMELINE_WAITING)
    state = TIMELINE_OWN;
  if (s->gm_id != gen_gm_id || state != gen_state)
    {
      gen_gm_id = s->gm_id;
      gen_state = state;
      gen_counter++;
    }
  s->generation = gen_counter;
  pthread_mutex_unlock(&gen_lck);
}

// gm = local - (offset + rate * (local - ref))
int64_t
timeline_local_to_gm(const struct timeline_snapshot *s, int64_t local_ns)
{
  return local_ns - (s->offset_ns + (int64_t)(s->rate * (double)(local_ns - s->ref_ns)));
}

int64_t
timeline_gm_to_local(const struct timeline_snapshot *s, int64_t gm_ns)
{
  // Solve gm = local (1 - rate) + rate ref - offset for local
  return (int64_t)(((double)(gm_ns + s->offset_ns) - s->rate * (double)s->ref_ns) / (1.0 - s->rate));
}

int
timeline_peer_add(const char *addr, uint32_t *id)
{
  if (!ptp_hdl)
    return -1;
  if (airptp_peer_add(id, addr, ptp_hdl) < 0)
    {
      log_warn(MOD, "Could not add PTP peer %s: %s", addr, airptp_errmsg_get());
      return -1;
    }
  log_debug(MOD, "PTP peer added: %s", addr);
  return 0;
}

void
timeline_peer_remove(uint32_t id)
{
  if (ptp_hdl)
    airptp_peer_remove(id, ptp_hdl);
}
