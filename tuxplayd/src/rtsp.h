#ifndef TUXPLAY_RTSP_H
#define TUXPLAY_RTSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <event2/event.h>

// RTSP (HTTP-shaped) connection to a receiver, optionally encrypted with the
// pairing cipher. Requests go out one at a time, in order; each gets a
// callback with the response or NULL on failure. The same connection can also
// receive requests from the peer (the event channel works that way), which go
// to the request handler.

#define RTSP_HEADERS_MAX 32

struct pair_cipher_context;

struct rtsp_message
{
  bool is_request;
  int status;          // responses
  char *method;        // requests
  char *uri;           // requests
  int nheaders;
  struct { char *name; char *value; } headers[RTSP_HEADERS_MAX];
  uint8_t *body;
  size_t body_len;
};

struct rtsp_header
{
  const char *name;
  const char *value;
};

enum rtsp_event
{
  RTSP_CONNECTED,
  RTSP_CLOSED,   // peer closed, or an I/O or protocol error
};

struct rtsp_conn;

typedef void (*rtsp_event_cb)(struct rtsp_conn *c, enum rtsp_event ev, void *arg);
typedef void (*rtsp_response_cb)(struct rtsp_conn *c, const struct rtsp_message *resp, void *arg);
// Fill in the response to an incoming request. Return the status code.
typedef int (*rtsp_request_cb)(struct rtsp_conn *c, const struct rtsp_message *req, void *arg);

struct rtsp_conn *
rtsp_conn_new(struct event_base *evbase, const char *name, rtsp_event_cb event_cb, void *arg);

// Asynchronous; RTSP_CONNECTED or RTSP_CLOSED follows
int
rtsp_conn_connect(struct rtsp_conn *c, const char *address, uint16_t port);

void
rtsp_conn_free(struct rtsp_conn *c);

void
rtsp_conn_request_handler_set(struct rtsp_conn *c, rtsp_request_cb cb);

// Added to every request: User-Agent, X-Apple-* and the like
void
rtsp_conn_default_header_add(struct rtsp_conn *c, const char *name, const char *value);

// From now on everything sent and received is encrypted. Takes ownership.
void
rtsp_conn_cipher_set(struct rtsp_conn *c, struct pair_cipher_context *cipher);

int
rtsp_request(struct rtsp_conn *c, const char *method, const char *uri,
             const char *content_type, const struct rtsp_header *extra, int nextra,
             const void *body, size_t body_len, int timeout_ms,
             rtsp_response_cb cb, void *arg);

// Requests queued or in flight
int
rtsp_pending(const struct rtsp_conn *c);

const char *
rtsp_message_header(const struct rtsp_message *m, const char *name);

// Numeric local and peer address of the connected socket
const char *
rtsp_local_address(const struct rtsp_conn *c);

const char *
rtsp_peer_address(const struct rtsp_conn *c);

// The receiver closed the connection in an orderly way (EOF), as it does
// when another sender takes it over
bool
rtsp_closed_by_peer(const struct rtsp_conn *c);

int
rtsp_fd(const struct rtsp_conn *c);

#endif
