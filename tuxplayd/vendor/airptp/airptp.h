#ifndef __AIRPTP_H__
#define __AIRPTP_H__

#include <inttypes.h>
#include <stdbool.h>

/* libairptp supports three modes of operation:
 * - Running a shared, standalone ptp daemon. In this case you will bind the ptp
 *   ports with airptp_daemon_bind() and then start with airptp_daemon_start()
 *   where is_shared is true.
 * - Running a private ptp daemon. You still bind and start (with is_shared
 *   false), but other processes using libairptpd won't be able to find it.
 * - As a library facilitating making requests to a ptp daemon, shared or
 *   private.
 */

struct airptp_handle;

struct airptp_callbacks
{
  // Optional - set name of thread
  void (*thread_name_set)(const char *name);

  // Debugging
  void (*hexdump)(const char *msg, uint8_t *data, size_t data_len);
  void (*logmsg)(const char *fmt, ...);

  // Optional - clock role changes and periodic sync quality, worth keeping in
  // a normal log (logmsg is per-packet detail)
  void (*lognotice)(const char *fmt, ...);
};

// How this clock takes part in the PTP domain.
enum airptp_role
{
  // Always grandmaster: announce a primary reference clock, so that every
  // receiver follows this host's clock.
  AIRPTP_ROLE_MASTER = 0,
  // What an Apple sender does: announce an ordinary clock, and when a receiver
  // offers a better one (HomePods and Apple TVs do) follow it, publishing the
  // mapping to its timeline. Grandmaster only when nobody better is present.
  AIRPTP_ROLE_FOLLOW,
};

enum airptp_timeline_state
{
  // This host's clock is the timeline
  AIRPTP_TIMELINE_OWN = 0,
  // Peers were just added: it is not known yet whether a better clock exists
  AIRPTP_TIMELINE_WAITING,
  // Locked to a receiver's grandmaster
  AIRPTP_TIMELINE_FOLLOWING,
};

// The timeline to express network time in, as seen from CLOCK_MONOTONIC. When
// following: gm_ns = local_ns - (offset_ns + rate * (local_ns - ref_ns)).
struct airptp_timeline
{
  enum airptp_timeline_state state;
  uint64_t clock_id; // grandmaster identity to name alongside the time
  int64_t ref_ns;
  int64_t offset_ns;
  double rate;
};

void
airptp_callbacks_register(struct airptp_callbacks *cb);

// Returns a handle if it was possible to bind to port 319 and 320. This
// normally requires root privilies. If node is non-NULL will bind to that
// specific address.
struct airptp_handle *
airptp_daemon_bind(const char *node);

// Starts a PTP daemon. Ports must have been bound already. Starting the daemon
// does not require privileges.
int
airptp_daemon_start(struct airptp_handle *hdl, uint64_t clock_id_seed, bool is_shared);

// Returns a handle if the host is running a compatible airptp daemon.
struct airptp_handle *
airptp_daemon_find(void);

int
airptp_peer_add(uint32_t *peer_id, const char *addr, struct airptp_handle *hdl);

void
airptp_peer_remove(uint32_t peer_id, struct airptp_handle *hdl);

// Frees ressources (incl. stops daemon if relevant)
void
airptp_end(struct airptp_handle *hdl);

// exit_cb?

int
airptp_clock_id_get(uint64_t *clock_id, struct airptp_handle *hdl);

// Before airptp_daemon_start(). lock_wait_ms bounds how long, after the first
// peer is added, the timeline stays WAITING for a receiver's clock to lock.
void
airptp_role_set(struct airptp_handle *hdl, enum airptp_role role, int lock_wait_ms);

// For a daemon running in this process; -1 otherwise.
int
airptp_timeline_get(struct airptp_timeline *tl, struct airptp_handle *hdl);

const char *
airptp_errmsg_get(void);

// By default airptp uses ports 319 and 320 as set by the standard, but for
// testing you can override that here.
void
airptp_ports_override(unsigned short event_port, unsigned short general_port);

#endif // __AIRPTP_H__
