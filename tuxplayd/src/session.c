#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include <sodium.h>

#include "pair.h"
#include "log.h"
#include "netwatch.h"
#include "plistx.h"
#include "rtsp.h"
#include "session.h"
#include "sink.h"
#include "stream.h"
#include "timeline.h"
#include "util.h"

#define MOD "session"

#define RTSP_TIMEOUT_MS  10000
#define FEEDBACK_SEC     2
#define ANCHOR_RETRY_MAX 10

// updateMRPlaybackState values
#define MR_PLAYING 1
#define MR_STOPPED 3
// ~100 ms: time for the anchor round trip before the frame is due
#define ANCHOR_RETRY_MARGIN_FRAMES 14

static const uint32_t mrp_cmd_ids[] = {
  0x61bd, 0x61bc, 0x95, 0x87, 0x86, 0x84, 0x83, 0x82, 0x81, 0x80, 0x7f,
  0x7d, 0x7a, 0x1a, 0x19, 0x18, 0x16, 0x15, 0x13, 0x12, 0x11, 0x0a, 0x0b,
  0x08, 0x09, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00
};

enum step
{
  STEP_CONNECT,
  STEP_PAIR_1,
  STEP_PAIR_2,
  STEP_SETUP_SESSION,
  STEP_EVENT_CONNECT,
  STEP_RECORD,
  STEP_SETPEERS,
  STEP_CMD_EMPTY,
  STEP_CMD_FULL,
  STEP_SETUP_STREAM,
  STEP_DATA_CONNECT,
  STEP_AUDIO_MODE,
  STEP_DONE,
};

struct session
{
  struct event_base *evbase;
  struct receiver *receiver;
  session_state_cb cb;
  void *cb_arg;

  enum session_state state;
  enum step step;

  struct rtsp_conn *ctrl;
  struct rtsp_conn *event_conn;
  uint16_t event_port;

  char session_url[128];
  char session_uuid[37];
  char local_addr[INET6_ADDRSTRLEN + 8];

  struct session_params params;
  char **peers;       // the other receivers playing with this one
  int npeers;
  char *pair_id;

  struct pair_setup_context *pair_ctx;
  uint8_t shared_key[64];

  uint16_t data_port;
  int data_fd;
  struct delivery *delivery;

  uint64_t anchor_frame;
  int64_t anchor_render_ns;
  uint64_t anchor_gm;
  uint32_t ptp_peer;
  bool have_ptp_peer;
  bool taken_over;
  bool network_lost;  // its local address or link went away
  int ifindex;        // interface our end of the control connection is on
  bool pausing;
  int mr_state;       // last updateMRPlaybackState sent, 0 = none
  bool volume_sent;
  double volume_db;

  struct event *feedback_ev;
  struct event *anchor_retry_ev;
  int anchor_retries;
  int anchor_retry_delay_ms;

  bool freed;
};

static void step_advance(struct session *s);
static void set_state(struct session *s, enum session_state st);

static const char *
state_names[] = { "STARTING", "READY", "ANCHORING", "PLAYING", "FAILED", "STOPPED" };

const char *
session_state_name(enum session_state st)
{
  return (st >= 0 && st <= SESSION_STOPPED) ? state_names[st] : "?";
}

enum session_state
session_state(const struct session *s)
{
  return s->state;
}

struct receiver *
session_receiver(const struct session *s)
{
  return s->receiver;
}

const char *
session_local_address(const struct session *s)
{
  return s->local_addr;
}

static void
set_state(struct session *s, enum session_state st)
{
  s->state = st;
  log_info(MOD, "%s: %s", s->receiver->name, session_state_name(st));
  s->cb(s, st, s->cb_arg);
}

static void
session_fail(struct session *s, const char *why)
{
  if (s->state == SESSION_FAILED || s->state == SESSION_STOPPED)
    return;
  log_err(MOD, "%s: %s", s->receiver->name, why);
  evtimer_del(s->feedback_ev);
  evtimer_del(s->anchor_retry_ev);
  if (s->delivery)
    delivery_stop(s->delivery);
  set_state(s, SESSION_FAILED);
}

static void
add_default_headers(struct session *s)
{
  char client_id[37];

  // As macOS 27 introduces itself (measured): its AirPlay version, a name and
  // a stable client id, and none of the old iTunes remote-control headers
  // (Client-Instance, DACP-ID, Active-Remote). The id is built from our MAC so
  // it stays the same across restarts.
  snprintf(client_id, sizeof(client_id), "00000000-0000-4000-8000-%s", s->pair_id);
  rtsp_conn_default_header_add(s->ctrl, "User-Agent", "AirPlay/980.77.5");
  rtsp_conn_default_header_add(s->ctrl, "X-Apple-Client-Name", s->params.sender_name);
  rtsp_conn_default_header_add(s->ctrl, "X-Apple-Client-ID", client_id);
}

/* -------------------------------- Feedback -------------------------------- */

static void
feedback_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp)
    log_debug(MOD, "%s: feedback timeout", s->receiver->name);
}

static void
feedback_tick(evutil_socket_t fd, short what, void *arg)
{
  struct session *s = arg;
  struct timeval tv = { FEEDBACK_SEC, 0 };

  if (s->state < SESSION_READY || s->state > SESSION_PLAYING)
    return;
  rtsp_request(s->ctrl, "POST", "/feedback", NULL, NULL, 0, NULL, 0,
               RTSP_TIMEOUT_MS, feedback_response, s);
  evtimer_add(s->feedback_ev, &tv);
}

/* ---------------------------- Playback state ----------------------------- */

static void
command_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp)
    log_warn(MOD, "%s: no reply to playback state", s->receiver->name);
  else if (resp->status != 200)
    log_debug(MOD, "%s: playback state answered %d", s->receiver->name, resp->status);
}

// What the receiver shows (its indicator) and tells its own controllers.
// Sent only on a change.
static void
playback_state_send(struct session *s, int state)
{
  plist_t p, params;
  uint8_t *body;
  size_t body_len;

  if (s->mr_state == state)
    return;
  s->mr_state = state;

  params = plist_new_dict();
  px_set_uint(params, "mrPlaybackState", (uint64_t)state);
  p = plist_new_dict();
  px_set_str(p, "type", "updateMRPlaybackState");
  px_set(p, "params", params);

  if (px_to_bin(p, &body, &body_len) == 0)
    {
      rtsp_request(s->ctrl, "POST", "/command", "application/x-apple-binary-plist", NULL, 0,
                   body, body_len, RTSP_TIMEOUT_MS, command_response, s);
      free(body);
    }
  plist_free(p);
}

static void
flush_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  s->pausing = false;
  if (!resp || resp->status != 200)
    {
      session_fail(s, resp ? "FLUSHBUFFERED rejected" : "FLUSHBUFFERED timeout");
      return;
    }
  playback_state_send(s, MR_STOPPED);
  set_state(s, SESSION_READY);
}

int
session_pause(struct session *s)
{
  plist_t p;
  uint8_t *body;
  size_t body_len;
  uint64_t head = stream_head();
  int ret;

  if (s->state != SESSION_PLAYING || s->pausing)
    return -1;

  // Nothing after this frame is played; the next anchor resumes the receiver
  if (s->delivery)
    delivery_stop(s->delivery);

  p = plist_new_dict();
  px_set_uint(p, "flushUntilSeq", head & 0x7fffff);
  px_set_uint(p, "flushUntilTS", stream_rtptime(head));
  if (px_to_bin(p, &body, &body_len) < 0)
    {
      plist_free(p);
      return -1;
    }
  plist_free(p);

  log_info(MOD, "%s: pausing at frame %llu", s->receiver->name, (unsigned long long)head);
  s->pausing = true;
  ret = rtsp_request(s->ctrl, "FLUSHBUFFERED", s->session_url, "application/x-apple-binary-plist", NULL, 0,
                     body, body_len, RTSP_TIMEOUT_MS, flush_response, s);
  free(body);
  return ret;
}

/* ------------------------------- Anchor ---------------------------------- */

static void
anchor_retry_tick(evutil_socket_t fd, short what, void *arg)
{
  struct session *s = arg;

  uint64_t frame = s->anchor_frame;
  uint64_t soonest = stream_head() + ANCHOR_RETRY_MARGIN_FRAMES;

  if (s->state != SESSION_ANCHORING)
    return;

  // Same line as the first attempt, so a pair stays in step: a later frame
  // renders exactly that much later
  if (frame < soonest)
    frame = soonest;
  session_anchor(s, frame,
                 s->anchor_render_ns + (int64_t)((frame - s->anchor_frame) * STREAM_SPF * NS_PER_SEC / SINK_RATE),
                 s->anchor_gm);
}

static void
anchor_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp)
    {
      session_fail(s, "anchor timeout");
      return;
    }

  if (resp->status != 200)
    goto retry;

  if (resp->body_len > 0)
    {
      plist_t p = px_from_bin(resp->body, resp->body_len);
      if (p)
        {
          uint64_t st = 0;
          px_get_uint(p, "status", &st);
          plist_free(p);
          if (st != 0)
            goto retry;
        }
    }

  log_info(MOD, "%s: anchor accepted", s->receiver->name);

  if (s->delivery)
    delivery_start(s->delivery, s->anchor_frame);

  playback_state_send(s, MR_PLAYING);
  set_state(s, SESSION_PLAYING);
  return;

 retry:
  s->anchor_retries++;
  if (s->anchor_retries > ANCHOR_RETRY_MAX)
    {
      session_fail(s, "anchor rejected too many times");
      return;
    }
  if (s->anchor_retry_delay_ms < 50)
    s->anchor_retry_delay_ms = 50;
  else if (s->anchor_retry_delay_ms < 800)
    s->anchor_retry_delay_ms *= 2;

  log_info(MOD, "%s: anchor rejected, retry %d in %d ms",
           s->receiver->name, s->anchor_retries, s->anchor_retry_delay_ms);

  struct timeval tv;
  tv.tv_sec = s->anchor_retry_delay_ms / 1000;
  tv.tv_usec = (s->anchor_retry_delay_ms % 1000) * 1000;
  evtimer_add(s->anchor_retry_ev, &tv);
}

int
session_anchor(struct session *s, uint64_t frame, int64_t render_gm_ns, uint64_t gm_id)
{
  plist_t p;
  uint8_t *body;
  size_t body_len;

  if (s->state != SESSION_READY && s->state != SESSION_ANCHORING)
    return -1;
  if (s->state == SESSION_READY)
    {
      s->anchor_retries = 0;
      s->anchor_retry_delay_ms = 0;
    }

  uint64_t secs = (uint64_t)(render_gm_ns / NS_PER_SEC);
  uint64_t nsec = (uint64_t)(render_gm_ns % NS_PER_SEC);
  if (render_gm_ns < 0)
    {
      secs = 0;
      nsec = 0;
    }
  uint64_t frac = ((nsec << 32) / 1000000000ULL) << 32;

  uint32_t rtptime = stream_rtptime(frame);
  s->anchor_frame = frame;
  s->anchor_render_ns = render_gm_ns;
  s->anchor_gm = gm_id;

  p = plist_new_dict();
  px_set_uint(p, "networkTimeSecs", secs);
  px_set_u64_as_int(p, "networkTimeFrac", frac);
  px_set_u64_as_int(p, "networkTimeTimelineID", gm_id);
  px_set_uint(p, "networkTimeFlags", 0);
  px_set_uint(p, "rtpTime", rtptime);
  px_set_uint(p, "rate", 1);

  if (px_to_bin(p, &body, &body_len) < 0)
    {
      plist_free(p);
      return -1;
    }
  plist_free(p);

  log_info(MOD, "%s: anchor frame %llu rtptime %u at %llu.%09llu + gm %016llx",
           s->receiver->name, (unsigned long long)frame, rtptime,
           (unsigned long long)secs, (unsigned long long)nsec,
           (unsigned long long)gm_id);

  int ret = rtsp_request(s->ctrl, "SETRATEANCHORTIME", s->session_url,
                         "application/x-apple-binary-plist", NULL, 0,
                         body, body_len, RTSP_TIMEOUT_MS, anchor_response, s);
  free(body);

  if (ret == 0 && s->state == SESSION_READY)
    s->state = SESSION_ANCHORING;
  return ret;
}

/* -------------------------------- Volume --------------------------------- */

static void
volume_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;
  if (!resp)
    log_warn(MOD, "%s: volume timeout", s->receiver->name);
  else if (resp->status != 200)
    log_warn(MOD, "%s: volume rejected (%d)", s->receiver->name, resp->status);
}

int
session_volume(struct session *s, double db)
{
  char body[64];
  int whole = (int)(-db);
  int frac = (int)(1000000.0 * (-db - whole));

  if (s->state < SESSION_READY || s->state > SESSION_PLAYING)
    return -1;
  if (s->volume_sent && s->volume_db == db)
    return 0;
  s->volume_sent = true;
  s->volume_db = db;

  snprintf(body, sizeof(body), "volume: -%d.%06d\r\n", whole, frac);
  return rtsp_request(s->ctrl, "SET_PARAMETER", s->session_url,
                      "text/parameters", NULL, 0,
                      body, strlen(body), RTSP_TIMEOUT_MS, volume_response, s);
}

/* -------------------------------- Peers ---------------------------------- */

static void
peers_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  /* no-op: SETPEERS is fire-and-forget in tuxplayd */
}

static void
peers_store(struct session *s, const char *const *addrs, int n)
{
  for (int i = 0; i < s->npeers; i++)
    free(s->peers[i]);
  free(s->peers);
  s->peers = n > 0 ? xcalloc((size_t)n, sizeof(char *)) : NULL;
  s->npeers = n;
  for (int i = 0; i < n; i++)
    s->peers[i] = xstrdup(addrs[i]);
}

// The PTP mesh this receiver is to take part in, as a Mac lists it: the
// receiver itself, us, then every other receiver playing in the group. All of
// them then elect one grandmaster, the timeline every anchor names.
static int
setpeers_send(struct session *s, rtsp_response_cb cb)
{
  const char *self = rtsp_peer_address(s->ctrl);
  plist_t arr = plist_new_array();
  uint8_t *body;
  size_t body_len;
  int ret;

  plist_array_append_item(arr, plist_new_string(self));
  plist_array_append_item(arr, plist_new_string(s->local_addr));
  for (int i = 0; i < s->npeers; i++)
    if (strcmp(s->peers[i], self) != 0 && strcmp(s->peers[i], s->receiver->addr4 ? s->receiver->addr4 : "") != 0 &&
        strcmp(s->peers[i], s->receiver->addr6 ? s->receiver->addr6 : "") != 0)
      plist_array_append_item(arr, plist_new_string(s->peers[i]));

  ret = px_to_bin(arr, &body, &body_len);
  plist_free(arr);
  if (ret < 0)
    return -1;

  ret = rtsp_request(s->ctrl, "SETPEERS", s->session_url, "/peer-list-changed", NULL, 0,
                     body, body_len, RTSP_TIMEOUT_MS, cb, s);
  free(body);
  return ret;
}

int
session_peers_set(struct session *s, const char *const *addrs, int n)
{
  peers_store(s, addrs, n);
  if (s->state < SESSION_READY || s->state > SESSION_PLAYING)
    return 0; // the startup SETPEERS will carry it
  return setpeers_send(s, peers_response);
}

/* ------------------------------ Event channel ---------------------------- */

static int
event_request_handler(struct rtsp_conn *c, const struct rtsp_message *req, void *arg)
{
  struct session *s = arg;

  if (!req->body || req->body_len < 8)
    return 200;

  plist_t p = px_from_bin(req->body, req->body_len);
  if (!p)
    {
      const uint8_t *bpl = memmem(req->body, req->body_len, "bplist", 6);
      if (bpl)
        p = px_from_bin(bpl, req->body_len - (size_t)(bpl - req->body));
    }
  if (!p)
    return 200;

  const char *type = px_get_str(p, "type");
  const char *value = px_get_str(p, "value");

  if (type && strcmp(type, "sendMediaRemoteCommand") == 0 && value)
    {
      log_info(MOD, "%s: event command: %s", s->receiver->name, value);
      if (strcmp(value, "paus") == 0)
        {
          log_info(MOD, "%s: receiver asked us to stop (takeover)", s->receiver->name);
          s->taken_over = true;
          plist_free(p);
          session_fail(s, "receiver takeover");
          return 200;
        }
    }

  plist_free(p);
  return 200;
}

static void
event_conn_event(struct rtsp_conn *c, enum rtsp_event ev, void *arg)
{
  struct session *s = arg;

  if (ev == RTSP_CONNECTED)
    {
      log_info(MOD, "%s: event channel connected", s->receiver->name);
      return;
    }
  log_info(MOD, "%s: event channel closed", s->receiver->name);
}

/* ----------------------------- Startup steps ----------------------------- */

static void
ctrl_event(struct rtsp_conn *c, enum rtsp_event ev, void *arg)
{
  struct session *s = arg;

  if (ev == RTSP_CONNECTED)
    {
      char *scope;
      uint32_t session_id = (uint32_t)random_u64();

      // Our address as the receiver is told it: no %scope, bracketed in a URL
      snprintf(s->local_addr, sizeof(s->local_addr), "%s", rtsp_local_address(c));
      if ((scope = strchr(s->local_addr, '%')))
        *scope = '\0';
      if (strchr(s->local_addr, ':'))
        snprintf(s->session_url, sizeof(s->session_url), "rtsp://[%s]/%u", s->local_addr, session_id);
      else
        snprintf(s->session_url, sizeof(s->session_url), "rtsp://%s/%u", s->local_addr, session_id);
      s->ifindex = netwatch_ifindex_of(s->local_addr);
      s->step = STEP_PAIR_1;
      step_advance(s);
      return;
    }
  // A receiver hangs up in an orderly way when another sender takes it over
  if (rtsp_closed_by_peer(c))
    s->taken_over = true;
  if (s->state < SESSION_FAILED)
    session_fail(s, s->taken_over ? "closed by the receiver (another sender?)" : "control connection lost");
}

static void
pair_1_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp)
    {
      session_fail(s, "pair-setup M1 timeout");
      return;
    }
  if (pair_setup_response1(s->pair_ctx, resp->body, resp->body_len) < 0)
    {
      session_fail(s, pair_setup_errmsg(s->pair_ctx));
      return;
    }
  s->step = STEP_PAIR_2;
  step_advance(s);
}

static void
pair_2_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp || resp->status != 200)
    {
      session_fail(s, resp ? "pair-setup M3 rejected" : "pair-setup M3 timeout");
      return;
    }
  if (pair_setup_response2(s->pair_ctx, resp->body, resp->body_len) < 0)
    {
      session_fail(s, pair_setup_errmsg(s->pair_ctx));
      return;
    }

  struct pair_result *result = NULL;
  if (pair_setup_result(NULL, &result, s->pair_ctx) < 0 || !result)
    {
      session_fail(s, "pair-setup result missing");
      return;
    }
  memcpy(s->shared_key, result->shared_secret, 64);

  struct pair_cipher_context *cipher = pair_cipher_new(
    PAIR_CLIENT_HOMEKIT_TRANSIENT, 0, s->shared_key, 64);
  if (!cipher)
    {
      session_fail(s, "cipher init failed");
      return;
    }
  rtsp_conn_cipher_set(s->ctrl, cipher);

  log_info(MOD, "%s: paired (transient)", s->receiver->name);
  s->step = STEP_SETUP_SESSION;
  step_advance(s);
}

// The receiver's PTP address: the one we already talk to it on if it lists
// it, else its first one of the same family
static const char *
timing_address_pick(plist_t addrs, const char *rtsp_addr)
{
  bool v6 = strchr(rtsp_addr, ':') != NULL;
  const char *same_family = NULL;
  uint32_t n = plist_array_get_size(addrs);

  for (uint32_t i = 0; i < n; i++)
    {
      plist_t item = plist_array_get_item(addrs, i);
      const char *a;

      if (plist_get_node_type(item) != PLIST_STRING || !(a = plist_get_string_ptr(item, NULL)))
        continue;
      if (strcmp(a, rtsp_addr) == 0)
        return a;
      if (!same_family && (strchr(a, ':') != NULL) == v6)
        same_family = a;
    }
  return same_family;
}

static void
setup_session_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp || resp->status != 200)
    {
      session_fail(s, resp ? "SETUP session rejected" : "SETUP session timeout");
      return;
    }

  plist_t p = px_from_bin(resp->body, resp->body_len);
  if (!p)
    {
      session_fail(s, "SETUP session: bad bplist");
      return;
    }

  uint64_t ep = 0;
  px_get_uint(p, "eventPort", &ep);
  s->event_port = (uint16_t)ep;

  plist_t tpi = plist_dict_get_item(p, "timingPeerInfo");
  if (tpi)
    {
      plist_t addrs = plist_dict_get_item(tpi, "Addresses");
      if (addrs && plist_get_node_type(addrs) == PLIST_ARRAY)
        {
          const char *addr = timing_address_pick(addrs, rtsp_peer_address(c));
          if (addr && !s->have_ptp_peer && timeline_peer_add(addr, &s->ptp_peer) == 0)
            {
              s->have_ptp_peer = true;
              log_info(MOD, "%s: PTP peer %s added", s->receiver->name, addr);
            }
        }
    }
  plist_free(p);

  if (s->event_port == 0)
    {
      session_fail(s, "SETUP session: no eventPort");
      return;
    }

  log_info(MOD, "%s: session set up, eventPort %u", s->receiver->name, s->event_port);
  s->step = STEP_EVENT_CONNECT;
  step_advance(s);
}

static void
record_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp || resp->status != 200)
    {
      session_fail(s, resp ? "RECORD rejected" : "RECORD timeout");
      return;
    }
  s->step = STEP_SETPEERS;
  step_advance(s);
}

static void
setpeers_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp || resp->status != 200)
    {
      session_fail(s, resp ? "SETPEERS rejected" : "SETPEERS timeout");
      return;
    }
  s->step = STEP_CMD_EMPTY;
  step_advance(s);
}

static void
cmd_empty_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp || resp->status != 200)
    {
      session_fail(s, resp ? "command (empty) rejected" : "command timeout");
      return;
    }
  s->step = STEP_CMD_FULL;
  step_advance(s);
}

static void
cmd_full_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp || resp->status != 200)
    {
      session_fail(s, resp ? "command (full) rejected" : "command timeout");
      return;
    }
  s->step = STEP_SETUP_STREAM;
  step_advance(s);
}

static void
setup_stream_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp || resp->status != 200)
    {
      session_fail(s, resp ? "SETUP stream rejected" : "SETUP stream timeout");
      return;
    }

  plist_t p = px_from_bin(resp->body, resp->body_len);
  if (!p)
    {
      session_fail(s, "SETUP stream: bad bplist");
      return;
    }
  if (log_threshold >= LOG_DEBUG)
    {
      char *xml = px_describe(p);
      log_debug(MOD, "%s: SETUP stream reply:\n%s", s->receiver->name, xml);
      free(xml);
    }

  plist_t streams = plist_dict_get_item(p, "streams");
  plist_t s0 = streams ? plist_array_get_item(streams, 0) : NULL;
  uint64_t dp = 0;

  if (s0)
    {
      plist_t sc = plist_dict_get_item(s0, "streamConnections");
      if (sc)
        {
          plist_t rtp = plist_dict_get_item(sc, "streamConnectionTypeRTP");
          if (rtp)
            px_get_uint(rtp, "streamConnectionKeyPort", &dp);
        }
      if (dp == 0)
        px_get_uint(s0, "dataPort", &dp);
    }
  plist_free(p);

  if (dp == 0)
    {
      session_fail(s, "SETUP stream: no data port");
      return;
    }
  s->data_port = (uint16_t)dp;

  log_info(MOD, "%s: stream set up, data port %u", s->receiver->name, s->data_port);
  s->step = STEP_DATA_CONNECT;
  step_advance(s);
}

static void
audio_mode_response(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg)
{
  struct session *s = arg;

  if (!resp || resp->status != 200)
    {
      session_fail(s, resp ? "audioMode rejected" : "audioMode timeout");
      return;
    }

  log_info(MOD, "%s: ready", s->receiver->name);

  struct timeval tv = { FEEDBACK_SEC, 0 };
  evtimer_add(s->feedback_ev, &tv);

  set_state(s, SESSION_READY);
}

/* ----------------------------- Data connection --------------------------- */

static int
tcp_connect_blocking(const char *address, uint16_t port, int timeout_ms)
{
  struct addrinfo hints = { .ai_flags = AI_NUMERICHOST, .ai_socktype = SOCK_STREAM }, *ai;
  char portstr[8];
  int fd, ret;

  snprintf(portstr, sizeof(portstr), "%u", port);
  ret = getaddrinfo(address, portstr, &hints, &ai);
  if (ret != 0)
    return -1;

  fd = socket(ai->ai_family, SOCK_STREAM, 0);
  if (fd < 0)
    {
      freeaddrinfo(ai);
      return -1;
    }

  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  ret = connect(fd, ai->ai_addr, ai->ai_addrlen);
  freeaddrinfo(ai);
  if (ret < 0)
    {
      close(fd);
      return -1;
    }
  return fd;
}

/* ----------------------------- Step machine ------------------------------ */

static int
send_pair_1(struct session *s)
{
  size_t len;
  uint8_t *body;
  struct rtsp_header hdr = { "X-Apple-HKP", "4" };

  s->pair_ctx = pair_setup_new(PAIR_CLIENT_HOMEKIT_TRANSIENT,
                               NULL, NULL, NULL, s->pair_id);
  if (!s->pair_ctx)
    return -1;

  body = pair_setup_request1(&len, s->pair_ctx);
  if (!body)
    return -1;

  int ret = rtsp_request(s->ctrl, "POST", "/pair-setup",
                         "application/octet-stream", &hdr, 1,
                         body, len, RTSP_TIMEOUT_MS, pair_1_response, s);
  free(body);
  return ret;
}

static int
send_pair_2(struct session *s)
{
  size_t len;
  uint8_t *body;
  struct rtsp_header hdr = { "X-Apple-HKP", "4" };

  body = pair_setup_request2(&len, s->pair_ctx);
  if (!body)
    return -1;

  int ret = rtsp_request(s->ctrl, "POST", "/pair-setup",
                         "application/octet-stream", &hdr, 1,
                         body, len, RTSP_TIMEOUT_MS, pair_2_response, s);
  free(body);
  return ret;
}

static int
send_setup_session(struct session *s)
{
  plist_t p, tpi, tpl, tpi_copy, addrs;
  uint8_t *body;
  size_t body_len;
  uint8_t mac[6];
  char macstr[18];

  if (!local_mac_get(mac))
    memset(mac, 0x5a, 6);
  mac_format(macstr, mac);
  for (char *c = macstr; *c; c++)
    *c = (*c >= 'A' && *c <= 'Z') ? (*c + 32) : *c;

  uuid_make(s->session_uuid);

  p = plist_new_dict();
  px_set_str(p, "name", s->params.sender_name);

  char devid[24];
  snprintf(devid, sizeof(devid), "%s", s->params.sender_id);
  px_set_str(p, "deviceID", devid);
  px_set_str(p, "sessionUUID", s->session_uuid);
  px_set_str(p, "timingProtocol", "PTP");
  px_set_str(p, "macAddress", macstr);
  px_set_str(p, "groupUUID", s->params.group_uuid);

  bool has_group = s->receiver->tsid && strlen(s->receiver->tsid) > 0;
  px_set_bool(p, "groupContainsGroupLeader", has_group);

  char clock_uuid[37];
  uuid_make(clock_uuid);

  tpi = plist_new_dict();
  px_set_str(tpi, "ID", clock_uuid);
  px_set_uint(tpi, "DeviceType", 0);
  px_set_u64_as_int(tpi, "ClockID", s->params.clock_id);
  px_set_bool(tpi, "SupportsClockPortMatchingOverride", true);

  addrs = plist_new_array();
  plist_array_append_item(addrs, plist_new_string(s->local_addr));
  px_set(tpi, "Addresses", addrs);

  px_set(p, "timingPeerInfo", plist_copy(tpi));

  tpl = plist_new_array();
  tpi_copy = plist_copy(tpi);
  plist_array_append_item(tpl, tpi_copy);
  px_set(p, "timingPeerList", tpl);

  plist_free(tpi);

  if (px_to_bin(p, &body, &body_len) < 0)
    {
      plist_free(p);
      return -1;
    }
  plist_free(p);

  return rtsp_request(s->ctrl, "SETUP", s->session_url,
                      "application/x-apple-binary-plist", NULL, 0,
                      body, body_len, RTSP_TIMEOUT_MS, setup_session_response, s);
}

static int
connect_event_channel(struct session *s)
{
  const char *peer = rtsp_peer_address(s->ctrl);

  s->event_conn = rtsp_conn_new(s->evbase, s->receiver->name, event_conn_event, s);
  rtsp_conn_request_handler_set(s->event_conn, event_request_handler);

  struct pair_cipher_context *ev_cipher = pair_cipher_new(
    PAIR_CLIENT_HOMEKIT_NORMAL, 1, s->shared_key, 64);
  if (ev_cipher)
    rtsp_conn_cipher_set(s->event_conn, ev_cipher);

  if (rtsp_conn_connect(s->event_conn, peer, s->event_port) < 0)
    {
      log_warn(MOD, "%s: event channel connect failed (continuing)", s->receiver->name);
      rtsp_conn_free(s->event_conn);
      s->event_conn = NULL;
    }
  return 0;
}

static int
send_record(struct session *s)
{
  return rtsp_request(s->ctrl, "RECORD", s->session_url,
                      NULL, NULL, 0, NULL, 0,
                      RTSP_TIMEOUT_MS, record_response, s);
}

static int
send_setpeers(struct session *s)
{
  return setpeers_send(s, setpeers_response);
}

static plist_t
make_cmd_entry(uint32_t id)
{
  plist_t d = plist_new_dict();
  px_set_uint(d, "kCommandInfoCommandKey", id);
  px_set_bool(d, "kCommandInfoEnabledKey", true);

  uint8_t *bin;
  size_t bin_len;
  if (px_to_bin(d, &bin, &bin_len) < 0)
    {
      plist_free(d);
      return NULL;
    }
  plist_free(d);

  plist_t data = plist_new_data((const char *)bin, bin_len);
  free(bin);
  return data;
}

static int
send_cmd_table(struct session *s, bool full, rtsp_response_cb cb)
{
  plist_t p, params_d, arr;
  uint8_t *body;
  size_t body_len;

  arr = plist_new_array();
  if (full)
    {
      for (size_t i = 0; i < ARRAY_SIZE(mrp_cmd_ids); i++)
        {
          plist_t entry = make_cmd_entry(mrp_cmd_ids[i]);
          if (entry)
            plist_array_append_item(arr, entry);
        }
    }

  params_d = plist_new_dict();
  px_set(params_d, "mrSupportedCommandsFromSender", arr);

  p = plist_new_dict();
  px_set_str(p, "type", "updateMRSupportedCommands");
  px_set(p, "params", params_d);

  if (px_to_bin(p, &body, &body_len) < 0)
    {
      plist_free(p);
      return -1;
    }
  plist_free(p);

  int ret = rtsp_request(s->ctrl, "POST", "/command",
                         "application/x-apple-binary-plist", NULL, 0,
                         body, body_len, RTSP_TIMEOUT_MS, cb, s);
  free(body);
  return ret;
}

static int
send_setup_stream(struct session *s)
{
  plist_t p, streams, sd, sc, rtp;
  uint8_t *body;
  size_t body_len;

  rtp = plist_new_dict();
  px_set_bool(rtp, "streamConnectionKeyUseStreamEncryptionKey", true);

  sc = plist_new_dict();
  px_set(sc, "streamConnectionTypeRTP", rtp);

  sd = plist_new_dict();
  px_set_uint(sd, "audioFormat", 1ULL << 21);
  px_set_bool(sd, "isMedia", true);
  px_set_uint(sd, "audioFormatIndex", 21);
  px_set_str(sd, "audioMode", "default");
  px_set_uint(sd, "ct", 2);
  px_set_data(sd, "shk", s->shared_key, 32);
  px_set_uint(sd, "spf", STREAM_SPF);
  px_set_uint(sd, "type", 103);
  px_set(sd, "streamConnections", sc);
  px_set_bool(sd, "supportsDynamicStreamID", true);
  px_set_str(sd, "clientID", "com.apple.Music");
  px_set_u64_as_int(sd, "streamConnectionID", (int64_t)random_u64());

  streams = plist_new_array();
  plist_array_append_item(streams, sd);

  p = plist_new_dict();
  px_set(p, "streams", streams);

  if (px_to_bin(p, &body, &body_len) < 0)
    {
      plist_free(p);
      return -1;
    }
  plist_free(p);

  int ret = rtsp_request(s->ctrl, "SETUP", s->session_url,
                         "application/x-apple-binary-plist", NULL, 0,
                         body, body_len, RTSP_TIMEOUT_MS, setup_stream_response, s);
  free(body);
  return ret;
}

static void
delivery_failed(struct delivery *d, bool by_peer, void *arg)
{
  struct session *s = arg;

  if (by_peer)
    s->taken_over = true;
  session_fail(s, by_peer ? "audio connection closed by the receiver (another sender?)" : "audio connection failed");
}

static int
connect_data(struct session *s)
{
  const char *peer = rtsp_peer_address(s->ctrl);

  s->data_fd = tcp_connect_blocking(peer, s->data_port, 3000);
  if (s->data_fd < 0)
    return -1;

  s->delivery = delivery_new(s->evbase, s->receiver->name,
                             s->data_fd, s->shared_key,
                             delivery_failed, s);
  return 0;
}

static int
send_audio_mode(struct session *s)
{
  plist_t p;
  uint8_t *body;
  size_t body_len;

  p = plist_new_dict();
  px_set_str(p, "audioMode", "default");

  if (px_to_bin(p, &body, &body_len) < 0)
    {
      plist_free(p);
      return -1;
    }
  plist_free(p);

  int ret = rtsp_request(s->ctrl, "POST", "/audioMode",
                         "application/x-apple-binary-plist", NULL, 0,
                         body, body_len, RTSP_TIMEOUT_MS, audio_mode_response, s);
  free(body);
  return ret;
}

static void
step_advance(struct session *s)
{
  int ret = 0;

  if (s->state >= SESSION_FAILED)
    return;

  switch (s->step)
    {
    case STEP_PAIR_1:
      ret = send_pair_1(s);
      break;
    case STEP_PAIR_2:
      ret = send_pair_2(s);
      break;
    case STEP_SETUP_SESSION:
      ret = send_setup_session(s);
      break;
    case STEP_EVENT_CONNECT:
      ret = connect_event_channel(s);
      s->step = STEP_RECORD;
      step_advance(s);
      return;
    case STEP_RECORD:
      ret = send_record(s);
      break;
    case STEP_SETPEERS:
      ret = send_setpeers(s);
      break;
    case STEP_CMD_EMPTY:
      ret = send_cmd_table(s, false, cmd_empty_response);
      break;
    case STEP_CMD_FULL:
      ret = send_cmd_table(s, true, cmd_full_response);
      break;
    case STEP_SETUP_STREAM:
      ret = send_setup_stream(s);
      break;
    case STEP_DATA_CONNECT:
      ret = connect_data(s);
      if (ret < 0)
        {
          session_fail(s, "data connection failed");
          return;
        }
      s->step = STEP_AUDIO_MODE;
      step_advance(s);
      return;
    case STEP_AUDIO_MODE:
      ret = send_audio_mode(s);
      break;
    default:
      return;
    }

  if (ret < 0)
    session_fail(s, "failed to send request");
}

/* ----------------------------- Public API -------------------------------- */

struct session *
session_start(struct event_base *evbase, struct receiver *r, const struct session_params *p,
              session_state_cb cb, void *arg)
{
  struct session *s = xcalloc(1, sizeof(*s));
  const char *addr;

  s->evbase = evbase;
  s->receiver = r;
  s->cb = cb;
  s->cb_arg = arg;
  s->state = SESSION_STARTING;
  s->data_fd = -1;

  s->params.sender_name = xstrdup(p->sender_name);
  s->params.sender_id = xstrdup(p->sender_id);
  s->params.group_uuid = xstrdup(p->group_uuid);
  s->params.clock_id = p->clock_id;
  peers_store(s, p->peers, p->npeers);

  s->pair_id = xstrdup(p->sender_id);
  for (char *c = s->pair_id; *c; c++)
    if (*c == ':') memmove(c, c + 1, strlen(c));

  s->feedback_ev = evtimer_new(evbase, feedback_tick, s);
  s->anchor_retry_ev = evtimer_new(evbase, anchor_retry_tick, s);

  s->ctrl = rtsp_conn_new(evbase, r->name, ctrl_event, s);
  add_default_headers(s);

  addr = receiver_address(r);
  if (!addr || rtsp_conn_connect(s->ctrl, addr, r->port) < 0)
    {
      log_err(MOD, "%s: cannot connect to receiver", r->name);
      s->state = SESSION_STOPPED;
      session_free(s);
      return NULL;
    }

  r->session = s;
  return s;
}

void
session_stop(struct session *s)
{
  if (!s || s->state == SESSION_STOPPED)
    return;

  evtimer_del(s->feedback_ev);
  evtimer_del(s->anchor_retry_ev);
  if (s->delivery)
    delivery_stop(s->delivery);

  set_state(s, SESSION_STOPPED);
}

bool
session_taken_over(const struct session *s)
{
  return s->taken_over;
}

int
session_ifindex(const struct session *s)
{
  return s->ifindex;
}

void
session_network_lost(struct session *s)
{
  s->network_lost = true;
  session_fail(s, "its network went away");
}

bool
session_lost_network(const struct session *s)
{
  return s->network_lost;
}

// Closing the connections is the teardown: no TEARDOWN is sent (a receiver
// handles a closed control connection the same way)
void
session_free(struct session *s)
{
  if (!s)
    return;

  if (s->receiver && s->receiver->session == s)
    s->receiver->session = NULL;
  if (s->delivery)
    delivery_free(s->delivery);
  if (s->have_ptp_peer)
    timeline_peer_remove(s->ptp_peer);
  if (s->ctrl)
    rtsp_conn_free(s->ctrl);
  if (s->event_conn)
    rtsp_conn_free(s->event_conn);
  if (s->pair_ctx)
    pair_setup_free(s->pair_ctx);
  if (s->feedback_ev)
    event_free(s->feedback_ev);
  if (s->anchor_retry_ev)
    event_free(s->anchor_retry_ev);
  sodium_memzero(s->shared_key, sizeof(s->shared_key));

  free((char *)s->params.sender_name);
  free((char *)s->params.sender_id);
  free((char *)s->params.group_uuid);
  peers_store(s, NULL, 0);
  free(s->pair_id);
  free(s);
}
