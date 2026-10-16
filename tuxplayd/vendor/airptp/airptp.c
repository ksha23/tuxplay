/*
MIT License

Copyright (c) 2026 OwnTone

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

// For shm_open
#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>

#include "airptp_internal.h"
#include "ptp_definitions.h"
#include "daemon.h"
#include "ptp_msg_handle.h"


/* -------------------------------- Globals --------------------------------- */

unsigned short airptp_event_port = PTP_EVENT_PORT;
unsigned short airptp_general_port = PTP_GENERAL_PORT;

struct airptp_callbacks __thread airptp_cb;
const char __thread *airptp_errmsg;

void
airptp_hexdump(const char *msg, void *data, size_t data_len)
{
  if (!airptp_cb.hexdump)
    return;

  airptp_cb.hexdump(msg, data, data_len);
}

void
airptp_logmsg(const char *fmt, ...)
{
  va_list ap;
  char content[2048];
  int ret;

  if (!airptp_cb.logmsg)
    return;

  va_start(ap, fmt);
  ret = vsnprintf(content, sizeof(content), fmt, ap);
  va_end(ap);

  if (ret < 0)
    airptp_cb.logmsg("[error printing log message]");
  else
    airptp_cb.logmsg("%s", content);
}

void
airptp_lognotice(const char *fmt, ...)
{
  va_list ap;
  char content[2048];
  int ret;

  if (!airptp_cb.lognotice)
    return;

  va_start(ap, fmt);
  ret = vsnprintf(content, sizeof(content), fmt, ap);
  va_end(ap);

  if (ret >= 0)
    airptp_cb.lognotice("%s", content);
}

void
airptp_thread_name_set(const char *name)
{
  if (!airptp_cb.thread_name_set)
    return;

  airptp_cb.thread_name_set(name);
}

/* ----------------------------------- API ---------------------------------- */

void
airptp_callbacks_register(struct airptp_callbacks *cb)
{
  if (cb->thread_name_set)
    airptp_cb.thread_name_set = cb->thread_name_set;
  if (cb->hexdump)
    airptp_cb.hexdump = cb->hexdump;
  if (cb->logmsg)
    airptp_cb.logmsg = cb->logmsg;
  if (cb->lognotice)
    airptp_cb.lognotice = cb->lognotice;
}

struct airptp_handle *
airptp_daemon_bind(const char *node)
{
  struct airptp_handle *hdl = NULL;
  struct utils_net_socket event_socket = UTILS_NET_SOCKET_INIT;
  struct utils_net_socket general_socket = UTILS_NET_SOCKET_INIT;
  int ret;

  ret = utils_net_bind(&event_socket, node, airptp_event_port);
  if (ret < 0)
    RETURN_ERROR(AIRPTP_ERR_INVALID, "Could not bind to PTP event port, usually 319. Check privileges and that it's free.");

  if (event_socket.fd4 < 0 || event_socket.fd6 < 0)
    airptp_logmsg("Couldn't bind port %hu for %s", airptp_event_port, (event_socket.fd6 < 0) ? "ipv6" : "ipv4");

  ret = utils_net_bind(&general_socket, node, airptp_general_port);
  if (ret < 0)
    RETURN_ERROR(AIRPTP_ERR_INVALID, "Could not bind to PTP general port, usually 320. Check privileges and that it's free.");

  if (general_socket.fd4 < 0 || general_socket.fd6 < 0)
    airptp_logmsg("Couldn't bind port %hu for %s", airptp_general_port, (general_socket.fd6 < 0) ? "ipv6" : "ipv4");

  hdl = calloc(1, sizeof(struct airptp_handle));
  if (!hdl)
    RETURN_ERROR(AIRPTP_ERR_OOM, "Out of memory");

  hdl->daemon.event_svc.port = airptp_event_port;
  hdl->daemon.event_svc.socket = event_socket;

  hdl->daemon.general_svc.port = airptp_general_port;
  hdl->daemon.general_svc.socket = general_socket;

  pthread_mutex_init(&hdl->daemon.timeline_lck, NULL);
  hdl->daemon.role = AIRPTP_ROLE_MASTER;
  hdl->daemon.follower.master = -1;

  hdl->state = AIRPTP_STATE_PORTS_BOUND;
  hdl->is_daemon = true;

  return hdl;

 error:
  free(hdl);
  utils_net_socket_close(&event_socket);
  utils_net_socket_close(&general_socket);
  return NULL;
}

// Starts a PTP daemon. Ports must have been bound already. Starting the daemon
// does not require privileges.
int
airptp_daemon_start(struct airptp_handle *hdl, uint64_t clock_id_seed, bool is_shared)
{
  int ret;

  if (!hdl->is_daemon || hdl->state != AIRPTP_STATE_PORTS_BOUND)
    RETURN_ERROR(AIRPTP_ERR_INVALID, "Can't start daemon, ports not bound not in daemon mode");

  // From IEEE EUI-64 clockIdentity values: "The most significant 3 octets of
  // the clockIdentity shall be an OUI. The least significant two bits of the
  // most significant octet of the OUI shall both be 0. The least significant
  // bit of the most significant octet of the OUI is used to distinguish
  // clockIdentity values specified by this subclause from those specified in
  // 7.5.2.2.3 [Non-IEEE EUI-64 clockIdentity values]".
  // If we had the MAC address at this point we, could make a valid EUI-48 based
  // clocked from mac[0..2] + 0xFFFE + mac[3..5]. However, since we don't, we
  // create a non-EUI-64 clock ID from 0xFFFF + 6 byte seed, ref 7.5.2.2.3.
  // tuxplay: a seed with its top 16 bits set is taken as a complete identity.
  // Apple's own senders and receivers use their MAC address followed by 0x0008,
  // and tuxplayd passes an identity built the same way.
  ret = daemon_start(&hdl->daemon, &hdl->daemon_info, is_shared,
                     (clock_id_seed >> 48) ? clock_id_seed : (clock_id_seed | 0xFFFF000000000000), airptp_cb);
  if (ret < 0)
    goto error; // errmsg set by daemon_start

  hdl->state = AIRPTP_STATE_RUNNING;

  return 0;

 error:
  return -1;
}

struct airptp_handle *
airptp_daemon_find(void)
{
  struct airptp_handle *hdl = NULL;
  struct airptp_daemon_info *daemon_info = MAP_FAILED;
  time_t now;
  int fd = -1;
  int ret __attribute__((unused));

  fd = shm_open(AIRPTP_SHM_NAME, O_RDONLY, 0);
  if (fd < 0)
    RETURN_ERROR(AIRPTP_ERR_NOTFOUND, "No airptp daemon found");

  daemon_info = mmap(NULL, sizeof(struct airptp_daemon_info), PROT_READ, MAP_SHARED, fd, 0);
  if (daemon_info == MAP_FAILED)
    RETURN_ERROR(AIRPTP_ERR_INTERNAL, "mmap() of shared memory returned an error");

  if (daemon_info->version_major != AIRPTP_SHM_STRUCTS_VERSION_MAJOR)
    RETURN_ERROR(AIRPTP_ERR_NOTFOUND, "The host is running an incompatible airptp daemon");

  now = time(NULL);
  if (daemon_info->ts + AIRPTP_STALE_SECS < now)
    RETURN_ERROR(AIRPTP_ERR_NOTFOUND, "No airptp daemon found (shared mem is stale)");

  hdl = calloc(1, sizeof(struct airptp_handle));
  if (!hdl)
    RETURN_ERROR(AIRPTP_ERR_OOM, "Out of memory");

  hdl->state = AIRPTP_STATE_RUNNING;
  hdl->is_daemon = false;
  memcpy(&hdl->daemon_info, daemon_info, sizeof(struct airptp_daemon_info));

  munmap(daemon_info, sizeof(struct airptp_daemon_info));
  close(fd);

  airptp_event_port = hdl->daemon_info.event_port;
  airptp_general_port = hdl->daemon_info.general_port;

  return hdl;

 error:
  free(hdl);
  if (daemon_info != MAP_FAILED)
    munmap(daemon_info, sizeof(struct airptp_daemon_info));
  if (fd >= 0)
    close(fd);
  return NULL;
}

int
airptp_peer_add(uint32_t *peer_id, const char *addr, struct airptp_handle *hdl)
{
  struct airptp_peer peer = { 0 };
  int ret;

  if (hdl->state != AIRPTP_STATE_RUNNING)
    RETURN_ERROR(AIRPTP_ERR_INVALID, "Can't add peer, no airptp daemon");

  if (utils_net_sockaddr_get(&peer.naddr, addr, 0) < 0)
    RETURN_ERROR(AIRPTP_ERR_INVALID, "Can't add peer, address is invalid");

  if (peer.naddr.sa.sa_family == AF_INET) {
    if (!hdl->daemon_info.ipv4_enabled)
      RETURN_ERROR(AIRPTP_ERR_INVALID, "Can't add peer with ipv4 address, daemon in ipv6-only mode");
    peer.naddr_len = sizeof(peer.naddr.sin);
  } else if (peer.naddr.sa.sa_family == AF_INET6) {
    if (!hdl->daemon_info.ipv6_enabled)
      RETURN_ERROR(AIRPTP_ERR_INVALID, "Can't add peer with ipv6 address, daemon in ipv4-only mode");
    peer.naddr_len = sizeof(peer.naddr.sin6);
  } else {
    RETURN_ERROR(AIRPTP_ERR_INVALID, "Can't add peer, invalid address family");
  }

  peer.id = utils_djb_hash(addr, strlen(addr));

  ret = ptp_msg_peer_add_send(&peer, hdl, airptp_general_port);
  if (ret < 0)
    RETURN_ERROR(AIRPTP_ERR_NOCONNECTION, "Can't add peer, connection to airptp daemon broken");

  *peer_id = peer.id;

  return 0;

 error:
  return -1;
}

void
airptp_peer_remove(uint32_t peer_id, struct airptp_handle *hdl)
{
  struct airptp_peer peer = { 0 };

  if (hdl->state != AIRPTP_STATE_RUNNING)
    return;

  peer.id = peer_id;

  ptp_msg_peer_del_send(&peer, hdl, airptp_general_port);
}

void
airptp_end(struct airptp_handle *hdl)
{
  if (!hdl)
    return;

  if (hdl->is_daemon) {
    daemon_stop(&hdl->daemon);
    utils_net_socket_close(&hdl->daemon.event_svc.socket);
    utils_net_socket_close(&hdl->daemon.general_svc.socket);
  }

  free(hdl);
}

int
airptp_clock_id_get(uint64_t *clock_id, struct airptp_handle *hdl)
{
  if (hdl->state != AIRPTP_STATE_RUNNING)
    return -1;

  *clock_id = hdl->daemon_info.clock_id;
  return 0;
}

void
airptp_role_set(struct airptp_handle *hdl, enum airptp_role role, int lock_wait_ms)
{
  if (!hdl->is_daemon || hdl->state != AIRPTP_STATE_PORTS_BOUND)
    return;

  hdl->daemon.role = role;
  hdl->daemon.lock_wait_ms = lock_wait_ms;
}

int
airptp_timeline_get(struct airptp_timeline *tl, struct airptp_handle *hdl)
{
  struct airptp_daemon *daemon = &hdl->daemon;
  struct timespec ts;
  int64_t now_ns;

  if (!hdl->is_daemon || hdl->state != AIRPTP_STATE_RUNNING)
    return -1;

  pthread_mutex_lock(&daemon->timeline_lck);
  *tl = daemon->timeline;
  if (tl->state != AIRPTP_TIMELINE_FOLLOWING)
    {
      tl->state = AIRPTP_TIMELINE_OWN;
      tl->clock_id = daemon->clock_id;

      // Waiting is bounded: a receiver that announces a better clock but never
      // completes a timing exchange must not hold playback back for good
      clock_gettime(CLOCK_MONOTONIC, &ts);
      now_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
      if (daemon->role == AIRPTP_ROLE_FOLLOW && daemon->timeline_first_peer_ns &&
	  now_ns - daemon->timeline_first_peer_ns < (int64_t)daemon->lock_wait_ms * 1000000LL)
	tl->state = AIRPTP_TIMELINE_WAITING;
    }
  pthread_mutex_unlock(&daemon->timeline_lck);

  return 0;
}

const char *
airptp_errmsg_get(void)
{
  return airptp_errmsg;
}

void
airptp_ports_override(unsigned short event_port, unsigned short general_port)
{
  airptp_event_port = event_port;
  airptp_general_port = general_port;
}
