#include <stdlib.h>
#include <string.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <json-c/json.h>

#include "api.h"
#include "devices.h"
#include "log.h"
#include "player.h"
#include "stream.h"

#define MOD "api"

static struct evhttp *http;

static void
reply_json(struct evhttp_request *req, int code, struct json_object *obj)
{
  struct evbuffer *buf = evbuffer_new();
  const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);

  evbuffer_add(buf, s, strlen(s));
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
  evhttp_add_header(evhttp_request_get_output_headers(req), "Cache-Control", "no-store");
  evhttp_send_reply(req, code, code == 200 ? "OK" : "Error", buf);
  evbuffer_free(buf);
  json_object_put(obj);
}

static void
reply_status(struct evhttp_request *req, int code, const char *msg)
{
  struct json_object *o = json_object_new_object();

  json_object_object_add(o, code < 300 ? "result" : "error", json_object_new_string(msg));
  reply_json(req, code == 204 ? 200 : code, o);
}

static struct json_object *
body_json(struct evhttp_request *req)
{
  struct evbuffer *in = evhttp_request_get_input_buffer(req);
  size_t len = evbuffer_get_length(in);
  char *s;
  struct json_object *o;

  if (len == 0 || len > 65536)
    return NULL;
  s = malloc(len + 1);
  evbuffer_copyout(in, s, len);
  s[len] = '\0';
  o = json_tokener_parse(s);
  free(s);
  return o;
}

static struct json_object *
output_json(const struct output *out)
{
  struct json_object *o = json_object_new_object();
  struct json_object *members = json_object_new_array();
  int i;

  json_object_object_add(o, "id", json_object_new_string(out->id));
  json_object_object_add(o, "name", json_object_new_string(out->name));
  json_object_object_add(o, "type", json_object_new_string("AirPlay 2"));
  json_object_object_add(o, "stereo_pair", json_object_new_boolean(out->is_pair));
  json_object_object_add(o, "selected", json_object_new_boolean(out->selected));
  json_object_object_add(o, "volume", json_object_new_int(out->volume));
  json_object_object_add(o, "available", json_object_new_boolean(output_ready(out)));
  json_object_object_add(o, "state", json_object_new_string(player_output_state(out)));
  for (i = 0; i < out->nmembers; i++)
    json_object_array_add(members, json_object_new_string(out->members[i]->name));
  json_object_object_add(o, "members", members);
  return o;
}

static void
outputs_cb(struct evhttp_request *req, void *arg)
{
  const char *uri = evhttp_request_get_uri(req);
  enum evhttp_cmd_type cmd = evhttp_request_get_command(req);
  struct output *out;

  if (strcmp(uri, "/api/outputs") == 0 && cmd == EVHTTP_REQ_GET)
    {
      struct json_object *root = json_object_new_object();
      struct json_object *arr = json_object_new_array();

      for (out = devices_outputs(); out; out = out->next)
        if (out->nmembers > 0)
          json_object_array_add(arr, output_json(out));
      json_object_object_add(root, "outputs", arr);
      reply_json(req, 200, root);
      return;
    }

  if (strncmp(uri, "/api/outputs/", 13) == 0)
    {
      struct json_object *body, *v;
      char id[32];
      const char *q;

      snprintf(id, sizeof(id), "%s", uri + 13);
      if ((q = strchr(id, '?')))
        id[q - id] = '\0';

      out = devices_output_find(id);
      if (!out)
        {
          reply_status(req, 404, "no such output");
          return;
        }
      if (cmd == EVHTTP_REQ_GET)
        {
          reply_json(req, 200, output_json(out));
          return;
        }
      if (cmd != EVHTTP_REQ_PUT && cmd != EVHTTP_REQ_POST)
        {
          reply_status(req, 405, "use GET or PUT");
          return;
        }

      body = body_json(req);
      if (!body)
        {
          reply_status(req, 400, "expected a JSON body");
          return;
        }
      if (json_object_object_get_ex(body, "volume", &v))
        player_output_volume(out, json_object_get_int(v));
      if (json_object_object_get_ex(body, "selected", &v))
        player_output_select(out, json_object_get_boolean(v));
      json_object_put(body);
      reply_status(req, 200, "ok");
      return;
    }

  reply_status(req, 404, "not found");
}

static void
player_cb(struct evhttp_request *req, void *arg)
{
  const char *uri = evhttp_request_get_uri(req);

  if (strcmp(uri, "/api/player/silence") == 0)
    {
      struct json_object *o = json_object_new_object();
      json_object_object_add(o, "silent_msec", json_object_new_int64(stream_silence_ms()));
      reply_json(req, 200, o);
      return;
    }

  // tuxplay-share used to tell owntone when the desktop went quiet. The
  // daemon sees the audio itself now; accept the calls so old tools work.
  if (strcmp(uri, "/api/player/active") == 0 || strcmp(uri, "/api/player/idle") == 0)
    {
      reply_status(req, 200, "handled by tuxplayd");
      return;
    }

  reply_status(req, 404, "not found");
}

static void
status_cb(struct evhttp_request *req, void *arg)
{
  reply_json(req, 200, player_status_json());
}

// GET: {"ms": lead, "sync_ms": correction, "advertised_ms": what players are told}
// PUT: either or both of "ms" (50-5000) and "sync_ms" (-500..500)
static void
latency_cb(struct evhttp_request *req, void *arg)
{
  struct json_object *body, *v;
  bool any = false, bad = false;

  if (evhttp_request_get_command(req) == EVHTTP_REQ_GET)
    {
      struct json_object *o = json_object_new_object();
      json_object_object_add(o, "ms", json_object_new_int(player_lead_ms()));
      json_object_object_add(o, "sync_ms", json_object_new_int(player_sync_ms()));
      json_object_object_add(o, "advertised_ms", json_object_new_int(player_lead_ms() - player_sync_ms()));
      reply_json(req, 200, o);
      return;
    }

  body = body_json(req);
  if (body && json_object_object_get_ex(body, "ms", &v))
    {
      any = true;
      bad |= player_lead_set(json_object_get_int(v)) < 0;
    }
  if (body && json_object_object_get_ex(body, "sync_ms", &v))
    {
      any = true;
      bad |= player_sync_set(json_object_get_int(v)) < 0;
    }
  if (body)
    json_object_put(body);

  if (!any)
    reply_status(req, 400, "expected {\"ms\": N} and/or {\"sync_ms\": N}");
  else if (bad)
    reply_status(req, 400, "out of range");
  else
    reply_status(req, 200, "ok");
}

// GET: {"minutes": N}; PUT {"minutes": N} releases the speakers after N
// minutes without sound, 0 = never
static void
idle_cb(struct evhttp_request *req, void *arg)
{
  struct json_object *body, *v;

  if (evhttp_request_get_command(req) == EVHTTP_REQ_GET)
    {
      struct json_object *o = json_object_new_object();
      json_object_object_add(o, "minutes", json_object_new_int(player_idle_release_s() / 60));
      reply_json(req, 200, o);
      return;
    }

  body = body_json(req);
  if (!body || !json_object_object_get_ex(body, "minutes", &v))
    reply_status(req, 400, "expected {\"minutes\": N}, 0 = never");
  else if (player_idle_release_set(json_object_get_int(v) * 60) < 0)
    reply_status(req, 400, "out of range (0-1440)");
  else
    reply_status(req, 200, "ok");
  if (body)
    json_object_put(body);
}

int
api_start(struct event_base *evbase, const char *address, uint16_t port)
{
  http = evhttp_new(evbase);
  if (!http)
    return -1;

  evhttp_set_allowed_methods(http, EVHTTP_REQ_GET | EVHTTP_REQ_PUT | EVHTTP_REQ_POST);
  evhttp_set_cb(http, "/api/outputs", outputs_cb, NULL);
  evhttp_set_cb(http, "/api/status", status_cb, NULL);
  evhttp_set_cb(http, "/api/latency", latency_cb, NULL);
  evhttp_set_cb(http, "/api/idle", idle_cb, NULL);
  evhttp_set_cb(http, "/api/player/silence", player_cb, NULL);
  evhttp_set_cb(http, "/api/player/active", player_cb, NULL);
  evhttp_set_cb(http, "/api/player/idle", player_cb, NULL);
  evhttp_set_gencb(http, outputs_cb, NULL); // /api/outputs/{id}

  if (evhttp_bind_socket(http, address, port) < 0)
    {
      log_err(MOD, "Could not listen on %s:%u", address, port);
      return -1;
    }
  log_info(MOD, "API on http://%s:%u", address, port);
  return 0;
}

void
api_stop(void)
{
  if (http)
    evhttp_free(http);
  http = NULL;
}
