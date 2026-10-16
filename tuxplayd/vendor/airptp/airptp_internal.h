#ifndef __AIRPTP_INTERNAL_H__
#define __AIRPTP_INTERNAL_H__

#include <event2/event.h>
#include <pthread.h>
#include <inttypes.h>

#include "airptp.h"
#include "utils.h"

#define AIRPTP_SHM_NAME "/airptp_shm"

#define AIRPTP_SHM_STRUCTS_VERSION_MAJOR 0
#define AIRPTP_SHM_STRUCTS_VERSION_MINOR 1

// If the ts is older than this we consider the daemon or peer gone
#define AIRPTP_STALE_SECS 15

#define AIRPTP_DOMAIN 0
#define AIRPTP_MAX_PEERS 32

#define RETURN_ERROR(r, m) \
  do { ret = (r); airptp_errmsg = (m); goto error; } while(0)

extern const char __thread *airptp_errmsg;

// The log2 of the announce message interval in seconds. AirPlay 2 receivers
// expect a grandmaster to announce at 0.25 sec (-2), matching the interval an
// AirPlay 2 sender advertises.
#define AIRPTP_LOGMESSAGEINT_ANNOUNCE -2
// Must match the interval implied by AIRPTP_LOGMESSAGEINT_ANNOUNCE above: a
// PTP receiver times out a grandmaster once it stops hearing announces within
// a small multiple of the advertised interval, so advertising 0.25 sec while
// actually sending every 1 sec causes the receiver to declare the grandmaster
// gone and refuse to slave to its timeline.
#define AIRPTP_INTERVAL_MS_ANNOUNCE 250
// Both iOS, ATV, amp and nqptp use -3, so 0.125 sec.
#define AIRPTP_LOGMESSAGEINT_SYNC -3
#define AIRPTP_INTERVAL_MS_SYNC 125
// Used by iOS
#define AIRPTP_LOGMESSAGEINT_SIGNALING -128
#define AIRPTP_INTERVAL_MS_SIGNALING 1000
#define AIRPTP_LOGMESSAGEINT_DELAY_RESP -3

enum airptp_error
{
  AIRPTP_OK           = 0,
  AIRPTP_ERR_INVALID  = -1,
  AIRPTP_ERR_NOCONNECTION = -2,
  AIRPTP_ERR_NOTFOUND = -3,
  AIRPTP_ERR_OOM = -4,
  AIRPTP_ERR_INTERNAL = -5,
};

enum airptp_state
{
  AIRPTP_STATE_NONE = 0,
  AIRPTP_STATE_PORTS_BOUND,
  AIRPTP_STATE_RUNNING,
};

struct airptp_daemon_info
{
  uint16_t version_major;
  uint16_t version_minor;
  uint64_t clock_id;
  time_t ts;
  uint16_t event_port;
  uint16_t general_port;
  bool ipv4_enabled;
  bool ipv6_enabled;
};

struct airptp_service
{
  struct utils_net_socket socket;
  unsigned short port;
  struct event *ev4;
  struct event *ev6;
};

struct airptp_peer
{
  uint32_t id;
  union utils_net_sockaddr naddr;
  socklen_t naddr_len;
  bool is_active;
  uint64_t last_seen;
};

#define AIRPTP_PORT_ID_SIZE 10
// Receivers offering a clock that we keep track of at once
#define AIRPTP_FOREIGN_MAX 8
// Timing exchange samples kept, at 8 per second
#define AIRPTP_SAMPLES_MAX 64
// Offset estimates kept for the rate fit, one per second
#define AIRPTP_FIT_POINTS_MAX 60

// A clock some receiver announces, either its own or one it follows
struct airptp_foreign
{
  bool in_use;
  uint8_t port_id[AIRPTP_PORT_ID_SIZE]; // as on the wire, the latest one seen
  union utils_net_sockaddr naddr;
  uint64_t gm_id;
  uint8_t p1;
  uint32_t quality; // class, accuracy, variance
  uint8_t p2;
  uint16_t steps;
  int8_t logint;
  int64_t last_announce_ns;
};

struct airptp_sample
{
  int64_t local_ns;
  int64_t value_ns;
};

// Owned by the daemon thread
struct airptp_follower
{
  struct airptp_foreign foreign[AIRPTP_FOREIGN_MAX];
  int master; // index in foreign, -1 if we are grandmaster
  int64_t master_since_ns;
  int64_t first_peer_ns; // start of the current run of peers, 0 if none

  bool sync_pending;
  uint16_t sync_seq;
  int64_t sync_rx_ns;
  int64_t sync_corr_ns;

  bool dreq_pending;
  uint16_t dreq_seq;
  int64_t dreq_tx_ns;

  // Sync direction: local receive - grandmaster send = offset + path delay
  struct airptp_sample fwd[AIRPTP_SAMPLES_MAX];
  int fwd_n;
  // Delay_Req direction: grandmaster receive - local send = path delay - offset
  struct airptp_sample rev[AIRPTP_SAMPLES_MAX];
  int rev_n;
  // One windowed offset estimate per second
  struct airptp_sample fit[AIRPTP_FIT_POINTS_MAX];
  int fit_n;

  bool locked;
  double delay_ms;
  double jitter_ms;
  int64_t stats_ns;
};

struct airptp_daemon
{
  bool is_shared;
  struct airptp_daemon_info *info;

  uint64_t clock_id;

  bool is_running;
  pthread_t tid;
  struct event_base *evbase;

  int start_pipe[2];
  int exit_pipe[2];
  struct event *start_stop_ev;

  struct airptp_service event_svc;
  struct airptp_service general_svc;

  struct event *shm_update_timer;

  struct event *send_announce_timer;
  struct event *send_signaling_timer;
  struct event *send_sync_timer;
  struct event *send_delay_req_timer;
  struct event *servo_timer;

  uint16_t announce_seq;
  uint16_t signaling_seq;
  uint16_t sync_seq;
  uint16_t delay_req_seq;

  enum airptp_role role;
  int lock_wait_ms;
  struct airptp_follower follower;

  // What other threads read, see airptp_timeline_get()
  pthread_mutex_t timeline_lck;
  struct airptp_timeline timeline;
  int64_t timeline_first_peer_ns;

  struct airptp_callbacks cb;

  struct airptp_peer peers[AIRPTP_MAX_PEERS];
  int num_peers;
};

struct airptp_handle
{
  bool is_daemon;
  enum airptp_state state;

  struct airptp_daemon daemon;

  struct airptp_daemon_info daemon_info;
};

void
airptp_hexdump(const char *msg, void *data, size_t data_len);

void
airptp_logmsg(const char *fmt, ...);

void
airptp_lognotice(const char *fmt, ...);

void
airptp_thread_name_set(const char *name);

#endif // __AIRPTP_INTERNAL_H__
