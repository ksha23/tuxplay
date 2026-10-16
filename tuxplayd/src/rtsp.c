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

#include "pair.h"
#include "log.h"
#include "rtsp.h"
#include "util.h"

#define MOD "rtsp"
#define DEFAULT_HEADERS_MAX 16
#define BODY_MAX (4 * 1024 * 1024)

struct request
{
  char *method;
  char *uri;
  struct evbuffer *wire; // plaintext message, headers + body, CSeq filled in at send
  int cseq;
  int timeout_ms;
  rtsp_response_cb cb;
  void *arg;
  struct request *next;
};

struct rtsp_conn
{
  char *name;
  struct event_base *evbase;
  struct bufferevent *bev;
  rtsp_event_cb event_cb;
  rtsp_request_cb request_cb;
  void *arg;

  struct pair_cipher_context *cipher;
  struct evbuffer *cipher_in;  // encrypted bytes not yet decrypted
  struct evbuffer *plain_in;   // decrypted (or plain) bytes not yet parsed

  int ndefaults;
  struct { char *name; char *value; } defaults[DEFAULT_HEADERS_MAX];

  struct request *queue;    // head is in flight once sent
  bool in_flight;
  int cseq;
  struct event *timeout_ev;

  char local[INET6_ADDRSTRLEN + 8];
  char peer[INET6_ADDRSTRLEN + 8];
  bool closed;
  bool closed_by_peer; // an orderly close from the receiver, not an error

  // Owner callbacks may free the connection. While one runs, a free is only
  // recorded, and done by whoever called the callback once it returns.
  int busy;
  bool free_pending;
};

static void conn_destroy(struct rtsp_conn *c);

#define CALLBACK(c, expr) do { (c)->busy++; expr; (c)->busy--; } while (0)

// True when the connection was freed during a callback and is now gone
static bool
conn_reap(struct rtsp_conn *c)
{
  if (c->busy == 0 && c->free_pending)
    {
      conn_destroy(c);
      return true;
    }
  return c->free_pending;
}

static void
message_clear(struct rtsp_message *m)
{
  int i;

  free(m->method);
  free(m->uri);
  for (i = 0; i < m->nheaders; i++)
    {
      free(m->headers[i].name);
      free(m->headers[i].value);
    }
  free(m->body);
  memset(m, 0, sizeof(*m));
}

const char *
rtsp_message_header(const struct rtsp_message *m, const char *name)
{
  int i;

  for (i = 0; i < m->nheaders; i++)
    if (strcasecmp(m->headers[i].name, name) == 0)
      return m->headers[i].value;
  return NULL;
}

static void
request_free(struct request *r)
{
  free(r->method);
  free(r->uri);
  if (r->wire)
    evbuffer_free(r->wire);
  free(r);
}

static void
conn_fail(struct rtsp_conn *c)
{
  struct request *r;

  if (c->closed)
    return;
  c->closed = true;

  if (c->timeout_ev)
    evtimer_del(c->timeout_ev);

  // Fail everything still waiting, in order
  while ((r = c->queue) && !c->free_pending)
    {
      c->queue = r->next;
      if (r->cb)
        CALLBACK(c, r->cb(c, NULL, r->arg));
      request_free(r);
    }
  c->in_flight = false;

  if (!c->free_pending)
    CALLBACK(c, c->event_cb(c, RTSP_CLOSED, c->arg));
}

static int
raw_write(struct rtsp_conn *c, struct evbuffer *plain)
{
  uint8_t *ciphertext;
  size_t ciphertext_len;
  size_t len = evbuffer_get_length(plain);
  ssize_t ret;

  if (!c->cipher)
    return bufferevent_write_buffer(c->bev, plain);

  ret = pair_encrypt(&ciphertext, &ciphertext_len, evbuffer_pullup(plain, -1), len, c->cipher);
  if (ret < 0 || (size_t)ret != len)
    {
      log_err(MOD, "%s: encryption failed: %s", c->name, pair_cipher_errmsg(c->cipher));
      return -1;
    }
  evbuffer_drain(plain, len);
  ret = bufferevent_write(c->bev, ciphertext, ciphertext_len);
  free(ciphertext);
  return (int)ret;
}

static void
queue_kick(struct rtsp_conn *c)
{
  struct request *r = c->queue;
  struct evbuffer *msg;
  struct timeval tv;

  if (c->closed || c->in_flight || !r || !c->bev)
    return;

  msg = evbuffer_new();
  r->cseq = c->cseq++; // from 0, as macOS sends it
  evbuffer_add_printf(msg, "%s %s RTSP/1.0\r\nCSeq: %d\r\n", r->method, r->uri, r->cseq);
  evbuffer_add_buffer(msg, r->wire);

  log_debug(MOD, "%s: > %s %s (CSeq %d)", c->name, r->method, r->uri, r->cseq);

  if (raw_write(c, msg) < 0)
    {
      evbuffer_free(msg);
      conn_fail(c);
      return;
    }
  evbuffer_free(msg);

  c->in_flight = true;
  tv.tv_sec = r->timeout_ms / 1000;
  tv.tv_usec = (r->timeout_ms % 1000) * 1000;
  evtimer_add(c->timeout_ev, &tv);
}

static void
timeout_cb(evutil_socket_t fd, short what, void *arg)
{
  struct rtsp_conn *c = arg;

  if (c->queue)
    log_warn(MOD, "%s: no response to %s %s within %d ms", c->name, c->queue->method, c->queue->uri, c->queue->timeout_ms);
  c->busy++;
  conn_fail(c);
  c->busy--;
  conn_reap(c);
}

// Parses one complete message from plain_in, or returns 0 if more is needed
static int
message_parse(struct rtsp_conn *c, struct rtsp_message *m)
{
  struct evbuffer_ptr eoh;
  size_t hlen;
  char *head, *line, *save, *p;
  const char *cl;
  size_t body_len = 0;

  eoh = evbuffer_search(c->plain_in, "\r\n\r\n", 4, NULL);
  if (eoh.pos < 0)
    return (evbuffer_get_length(c->plain_in) > 64 * 1024) ? -1 : 0;

  hlen = (size_t)eoh.pos + 4;
  head = malloc(hlen + 1);
  evbuffer_copyout(c->plain_in, head, hlen);
  head[hlen] = '\0';

  memset(m, 0, sizeof(*m));
  line = strtok_r(head, "\r\n", &save);
  if (!line)
    goto bad;

  if (strncmp(line, "RTSP/", 5) == 0 || strncmp(line, "HTTP/", 5) == 0)
    {
      p = strchr(line, ' ');
      if (!p)
        goto bad;
      m->status = atoi(p + 1);
    }
  else
    {
      char *sp1 = strchr(line, ' ');
      char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
      if (!sp1 || !sp2)
        goto bad;
      m->is_request = true;
      m->method = strndup(line, (size_t)(sp1 - line));
      m->uri = strndup(sp1 + 1, (size_t)(sp2 - sp1 - 1));
    }

  while ((line = strtok_r(NULL, "\r\n", &save)) && m->nheaders < RTSP_HEADERS_MAX)
    {
      p = strchr(line, ':');
      if (!p)
        continue;
      *p++ = '\0';
      while (*p == ' ')
        p++;
      m->headers[m->nheaders].name = xstrdup(line);
      m->headers[m->nheaders].value = xstrdup(p);
      m->nheaders++;
    }
  free(head);

  cl = rtsp_message_header(m, "Content-Length");
  if (cl)
    body_len = strtoul(cl, NULL, 10);
  if (body_len > BODY_MAX)
    {
      message_clear(m);
      return -1;
    }

  if (evbuffer_get_length(c->plain_in) < hlen + body_len)
    {
      message_clear(m);
      return 0;
    }

  evbuffer_drain(c->plain_in, hlen);
  if (body_len)
    {
      m->body = malloc(body_len + 1);
      evbuffer_remove(c->plain_in, m->body, body_len);
      m->body[body_len] = '\0';
      m->body_len = body_len;
    }
  return 1;

 bad:
  free(head);
  message_clear(m);
  return -1;
}

static void
handle_request(struct rtsp_conn *c, struct rtsp_message *m)
{
  struct evbuffer *msg = evbuffer_new();
  const char *cseq = rtsp_message_header(m, "CSeq");
  int status = 501;

  if (c->request_cb)
    CALLBACK(c, status = c->request_cb(c, m, c->arg));
  if (c->free_pending)
    {
      evbuffer_free(msg);
      return;
    }

  evbuffer_add_printf(msg, "RTSP/1.0 %d %s\r\nContent-Length: 0\r\n", status, status == 200 ? "OK" : "Error");
  if (cseq)
    evbuffer_add_printf(msg, "CSeq: %s\r\n", cseq);
  evbuffer_add_printf(msg, "Server: AirTunes/980.77.5\r\n\r\n");
  if (raw_write(c, msg) < 0)
    conn_fail(c);
  evbuffer_free(msg);
}

static void read_process(struct rtsp_conn *c);

static void
read_cb(struct bufferevent *bev, void *arg)
{
  struct rtsp_conn *c = arg;

  c->busy++;
  read_process(c);
  c->busy--;
  conn_reap(c);
}

static void
read_process(struct rtsp_conn *c)
{
  struct bufferevent *bev = c->bev;
  struct evbuffer *in = bufferevent_get_input(bev);
  struct rtsp_message m;
  struct request *r;
  int ret;

  if (c->cipher)
    {
      uint8_t *plain;
      size_t plain_len;
      ssize_t used;

      evbuffer_add_buffer(c->cipher_in, in);
      if (evbuffer_get_length(c->cipher_in) > 0)
        {
          used = pair_decrypt(&plain, &plain_len, evbuffer_pullup(c->cipher_in, -1), evbuffer_get_length(c->cipher_in), c->cipher);
          if (used < 0)
            {
              log_err(MOD, "%s: decryption failed: %s", c->name, pair_cipher_errmsg(c->cipher));
              conn_fail(c);
              return;
            }
          evbuffer_drain(c->cipher_in, (size_t)used);
          evbuffer_add(c->plain_in, plain, plain_len);
          free(plain);
        }
    }
  else
    evbuffer_add_buffer(c->plain_in, in);

  while (!c->closed && (ret = message_parse(c, &m)) != 0)
    {
      if (ret < 0)
        {
          log_err(MOD, "%s: malformed message from receiver", c->name);
          conn_fail(c);
          return;
        }

      if (m.is_request)
        {
          log_debug(MOD, "%s: < request %s %s", c->name, m.method, m.uri);
          handle_request(c, &m);
          message_clear(&m);
          continue;
        }

      r = c->queue;
      if (!r || !c->in_flight)
        {
          log_warn(MOD, "%s: unsolicited response %d", c->name, m.status);
          message_clear(&m);
          continue;
        }

      evtimer_del(c->timeout_ev);
      c->queue = r->next;
      c->in_flight = false;
      log_debug(MOD, "%s: < %d for %s %s (%zu bytes)", c->name, m.status, r->method, r->uri, m.body_len);
      if (r->cb)
        CALLBACK(c, r->cb(c, &m, r->arg));
      request_free(r);
      message_clear(&m);

      if (c->closed || c->free_pending)
        return;
      queue_kick(c);
    }
}

static void
addresses_store(struct rtsp_conn *c)
{
  struct sockaddr_storage ss;
  socklen_t len;
  int fd = bufferevent_getfd(c->bev);
  char host[INET6_ADDRSTRLEN];

  len = sizeof(ss);
  if (getsockname(fd, (struct sockaddr *)&ss, &len) == 0 &&
      getnameinfo((struct sockaddr *)&ss, len, host, sizeof(host), NULL, 0, NI_NUMERICHOST) == 0)
    snprintf(c->local, sizeof(c->local), "%s", host);
  len = sizeof(ss);
  if (getpeername(fd, (struct sockaddr *)&ss, &len) == 0 &&
      getnameinfo((struct sockaddr *)&ss, len, host, sizeof(host), NULL, 0, NI_NUMERICHOST) == 0)
    snprintf(c->peer, sizeof(c->peer), "%s", host);
}

static void
event_cb(struct bufferevent *bev, short what, void *arg)
{
  struct rtsp_conn *c = arg;
  int one = 1;

  if (what & BEV_EVENT_CONNECTED)
    {
      setsockopt(bufferevent_getfd(bev), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      addresses_store(c);
      log_debug(MOD, "%s: connected %s -> %s", c->name, c->local, c->peer);
      CALLBACK(c, c->event_cb(c, RTSP_CONNECTED, c->arg));
      if (conn_reap(c))
        return;
      queue_kick(c);
      return;
    }

  if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
    {
      if (what & BEV_EVENT_ERROR)
        log_info(MOD, "%s: connection error: %s", c->name, evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
      else
        {
          log_info(MOD, "%s: closed by receiver", c->name);
          c->closed_by_peer = true;
        }
      c->busy++;
      conn_fail(c);
      c->busy--;
      conn_reap(c);
    }
}

struct rtsp_conn *
rtsp_conn_new(struct event_base *evbase, const char *name, rtsp_event_cb ev_cb, void *arg)
{
  struct rtsp_conn *c = xcalloc(1, sizeof(*c));

  c->name = xstrdup(name);
  c->evbase = evbase;
  c->event_cb = ev_cb;
  c->arg = arg;
  c->cipher_in = evbuffer_new();
  c->plain_in = evbuffer_new();
  c->timeout_ev = evtimer_new(evbase, timeout_cb, c);
  return c;
}

int
rtsp_conn_connect(struct rtsp_conn *c, const char *address, uint16_t port)
{
  struct sockaddr_storage ss;
  struct addrinfo hints = { .ai_flags = AI_NUMERICHOST, .ai_socktype = SOCK_STREAM }, *ai;
  char portstr[8];
  int ret;

  snprintf(portstr, sizeof(portstr), "%u", port);
  ret = getaddrinfo(address, portstr, &hints, &ai);
  if (ret != 0)
    {
      log_err(MOD, "%s: bad address %s: %s", c->name, address, gai_strerror(ret));
      return -1;
    }
  memcpy(&ss, ai->ai_addr, ai->ai_addrlen);

  c->bev = bufferevent_socket_new(c->evbase, -1, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(c->bev, read_cb, NULL, event_cb, c);
  bufferevent_enable(c->bev, EV_READ | EV_WRITE);
  ret = bufferevent_socket_connect(c->bev, (struct sockaddr *)&ss, (int)ai->ai_addrlen);
  freeaddrinfo(ai);
  if (ret < 0)
    {
      log_err(MOD, "%s: connect to %s:%u failed", c->name, address, port);
      return -1;
    }
  return 0;
}

void
rtsp_conn_free(struct rtsp_conn *c)
{
  if (!c)
    return;
  if (c->busy)
    {
      c->free_pending = true;
      c->closed = true;
      return;
    }
  conn_destroy(c);
}

static void
conn_destroy(struct rtsp_conn *c)
{
  struct request *r;
  int i;

  // Silently: the owner is going away and wants no callbacks
  while ((r = c->queue))
    {
      c->queue = r->next;
      request_free(r);
    }
  if (c->bev)
    bufferevent_free(c->bev);
  if (c->timeout_ev)
    event_free(c->timeout_ev);
  if (c->cipher)
    pair_cipher_free(c->cipher);
  evbuffer_free(c->cipher_in);
  evbuffer_free(c->plain_in);
  for (i = 0; i < c->ndefaults; i++)
    {
      free(c->defaults[i].name);
      free(c->defaults[i].value);
    }
  free(c->name);
  free(c);
}

void
rtsp_conn_request_handler_set(struct rtsp_conn *c, rtsp_request_cb cb)
{
  c->request_cb = cb;
}

void
rtsp_conn_default_header_add(struct rtsp_conn *c, const char *name, const char *value)
{
  if (c->ndefaults >= DEFAULT_HEADERS_MAX)
    return;
  c->defaults[c->ndefaults].name = xstrdup(name);
  c->defaults[c->ndefaults].value = xstrdup(value);
  c->ndefaults++;
}

void
rtsp_conn_cipher_set(struct rtsp_conn *c, struct pair_cipher_context *cipher)
{
  if (c->cipher)
    pair_cipher_free(c->cipher);
  c->cipher = cipher;
}

int
rtsp_request(struct rtsp_conn *c, const char *method, const char *uri,
             const char *content_type, const struct rtsp_header *extra, int nextra,
             const void *body, size_t body_len, int timeout_ms,
             rtsp_response_cb cb, void *arg)
{
  struct request *r, **tail;
  int i;

  if (c->closed)
    return -1;

  r = xcalloc(1, sizeof(*r));
  r->method = xstrdup(method);
  r->uri = xstrdup(uri);
  r->timeout_ms = timeout_ms > 0 ? timeout_ms : 10000;
  r->cb = cb;
  r->arg = arg;
  r->wire = evbuffer_new();

  for (i = 0; i < c->ndefaults; i++)
    evbuffer_add_printf(r->wire, "%s: %s\r\n", c->defaults[i].name, c->defaults[i].value);
  for (i = 0; i < nextra; i++)
    evbuffer_add_printf(r->wire, "%s: %s\r\n", extra[i].name, extra[i].value);
  if (content_type)
    evbuffer_add_printf(r->wire, "Content-Type: %s\r\n", content_type);
  evbuffer_add_printf(r->wire, "Content-Length: %zu\r\n\r\n", body_len);
  if (body_len)
    evbuffer_add(r->wire, body, body_len);

  for (tail = &c->queue; *tail; tail = &(*tail)->next)
    ;
  *tail = r;

  queue_kick(c);
  return 0;
}

int
rtsp_pending(const struct rtsp_conn *c)
{
  const struct request *r;
  int n = 0;

  for (r = c->queue; r; r = r->next)
    n++;
  return n;
}

const char *
rtsp_local_address(const struct rtsp_conn *c)
{
  return c->local;
}

const char *
rtsp_peer_address(const struct rtsp_conn *c)
{
  return c->peer;
}

bool
rtsp_closed_by_peer(const struct rtsp_conn *c)
{
  return c->closed_by_peer;
}

int
rtsp_fd(const struct rtsp_conn *c)
{
  return c->bev ? bufferevent_getfd(c->bev) : -1;
}
