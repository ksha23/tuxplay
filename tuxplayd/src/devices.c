#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <json-c/json.h>
#include <net/if.h>

#include "devices.h"
#include "log.h"
#include "util.h"

#define MOD "devices"
#define DEFAULT_VOLUME 30

static struct receiver *receivers;
static struct output *outputs;
static char *state_file;
static struct json_object *saved_state; // outputs by id, as last saved
static struct json_object *settings;    // daemon settings (lead, sync), by name
static devices_changed_cb changed_cb;
static void *changed_arg;

static uint64_t
fnv1a(const char *s)
{
  uint64_t h = 1469598103934665603ULL;

  for (; *s; s++)
    {
      h ^= (unsigned char)*s;
      h *= 1099511628211ULL;
    }
  return h & ((1ULL << 53) - 1); // stays exact as a JSON number
}

static void
replace(char **dst, const char *src)
{
  if (*dst && src && strcmp(*dst, src) == 0)
    return;
  free(*dst);
  *dst = src ? xstrdup(src) : NULL;
}

// "0x4A7FCA00,0x3C354BD0": low word first
static uint64_t
features_parse(const char *s)
{
  char *end;
  uint64_t lo, hi = 0;

  if (!s)
    return 0;
  lo = strtoull(s, &end, 16);
  if (*end == ',')
    hi = strtoull(end + 1, NULL, 16);
  return lo | (hi << 32);
}

static uint64_t
if_bit(int ifindex)
{
  return 1ULL << (ifindex > 0 && ifindex < 64 ? ifindex : 63);
}

// A scoped link-local address ("fe80::1%enp7s0") is only usable through its
// own interface
static bool
addr_on_if(const char *addr, int ifindex)
{
  char name[IF_NAMESIZE];
  const char *scope = addr ? strchr(addr, '%') : NULL;

  return scope && if_indextoname((unsigned)ifindex, name) && strcmp(scope + 1, name) == 0;
}

static bool
txt_bool(const char *v)
{
  return v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0);
}

static void
output_restore(struct output *out)
{
  struct json_object *o, *v;

  out->volume = DEFAULT_VOLUME;
  if (!saved_state || !json_object_object_get_ex(saved_state, out->id, &o))
    return;
  if (json_object_object_get_ex(o, "selected", &v))
    out->selected = json_object_get_boolean(v);
  if (json_object_object_get_ex(o, "volume", &v))
    out->volume = json_object_get_int(v);
  if (json_object_object_get_ex(o, "offset_ms", &v))
    out->offset_ms = json_object_get_int(v);
}

static struct output *
output_get(const char *key, const char *name, bool is_pair)
{
  struct output *out;
  char id[24];

  snprintf(id, sizeof(id), "%" PRIu64, fnv1a(key));
  for (out = outputs; out; out = out->next)
    if (strcmp(out->id, id) == 0)
      {
        replace(&out->name, name);
        return out;
      }

  out = xcalloc(1, sizeof(*out));
  memcpy(out->id, id, sizeof(id));
  out->name = xstrdup(name);
  out->is_pair = is_pair;
  output_restore(out);
  out->next = outputs;
  outputs = out;
  log_info(MOD, "New output '%s' (%s, id %s)%s", out->name, is_pair ? "stereo pair" : "single", out->id,
           out->selected ? ", selected" : "");
  return out;
}

static void
output_detach(struct receiver *r)
{
  struct output *out = r->output;
  int i, j;

  if (!out)
    return;
  for (i = 0, j = 0; i < out->nmembers; i++)
    if (out->members[i] != r)
      out->members[j++] = out->members[i];
  out->nmembers = j;
  r->output = NULL;
}

// Which output a receiver belongs to. A stereo pair is the receivers sharing
// a tight-sync id (tsid); its name is the group public name (gpn).
static void
output_assign(struct receiver *r)
{
  struct output *out;

  if (r->tsid)
    out = output_get(r->tsid, r->gpn ? r->gpn : r->name, true);
  else
    out = output_get(r->deviceid ? r->deviceid : r->name, r->name, false);

  if (r->output == out)
    return;

  output_detach(r);
  if (out->nmembers < 2)
    {
      out->members[out->nmembers++] = r;
      r->output = out;
    }
  else
    log_warn(MOD, "Output '%s' already has two members, ignoring '%s'", out->name, r->name);
}

void
devices_mdns_cb(const struct mdns_service *svc, bool removed, void *arg)
{
  struct receiver *r;
  const char *v;
  bool was_present;

  for (r = receivers; r; r = r->next)
    if (strcmp(r->name, svc->name) == 0)
      break;

  if (removed)
    {
      if (!r)
        return;
      // Gone from one interface (a cable unplugged, Wi-Fi off) is not gone:
      // the same receiver is often still seen on another
      if (svc->ipv6)
        {
          r->seen6 &= ~if_bit(svc->ifindex);
          if (addr_on_if(r->addr6, svc->ifindex))
            replace(&r->addr6, NULL);
        }
      else
        r->seen4 &= ~if_bit(svc->ifindex);
      // The address is kept: when our own network is what went away, it is
      // still the best guess for reconnecting once it is back (see
      // output_reachable())
      was_present = r->present;
      r->present = (r->seen4 || r->seen6);
      if (was_present && !r->present)
        {
          log_info(MOD, "Receiver '%s' gone", r->name);
          if (r->output && changed_cb)
            changed_cb(r->output, changed_arg);
        }
      return;
    }

  if (!r)
    {
      r = xcalloc(1, sizeof(*r));
      r->name = xstrdup(svc->name);
      r->next = receivers;
      receivers = r;
    }

  was_present = r->present;

  replace(&r->deviceid, mdns_txt_get(svc, "deviceid"));
  replace(&r->model, mdns_txt_get(svc, "model"));
  replace(&r->srcvers, mdns_txt_get(svc, "srcvers"));
  replace(&r->pi, mdns_txt_get(svc, "pi"));
  replace(&r->gid, mdns_txt_get(svc, "gid"));
  replace(&r->tsid, mdns_txt_get(svc, "tsid"));
  // Only replace the pair name when one is given: the hints are not advertised
  // continuously, and a missing one means no new information, not no name
  if ((v = mdns_txt_get(svc, "gpn")))
    replace(&r->gpn, v);
  r->features = features_parse(mdns_txt_get(svc, "features"));
  r->flags = (v = mdns_txt_get(svc, "flags")) ? (uint32_t)strtoul(v, NULL, 16) : 0;
  if ((v = mdns_txt_get(svc, "igl")))
    r->igl = txt_bool(v);
  if ((v = mdns_txt_get(svc, "gcgl")))
    r->gcgl = txt_bool(v);
  r->port = svc->port;

  if (svc->ipv6)
    r->seen6 |= if_bit(svc->ifindex);
  else
    r->seen4 |= if_bit(svc->ifindex);

  // Prefer a global or ULA IPv6 address over a link-local one; keep IPv4 as is
  if (svc->ipv6)
    {
      if (!r->addr6 || strchr(r->addr6, '%') || !strchr(svc->address, '%'))
        replace(&r->addr6, svc->address);
    }
  else
    replace(&r->addr4, svc->address);

  r->present = true;
  output_assign(r);

  if (!was_present)
    log_info(MOD, "Receiver '%s' (%s, %s) at %s:%u%s%s", r->name, r->model ? r->model : "?",
             r->srcvers ? r->srcvers : "?", receiver_address(r), r->port,
             r->tsid ? ", pair " : "", r->tsid ? (r->gpn ? r->gpn : r->tsid) : "");

  if (changed_cb && r->output)
    changed_cb(r->output, changed_arg);
}

bool
receiver_usable(const struct receiver *r)
{
  return r->present && (r->features & FEATURE_BUFFERED_AUDIO) && (r->features & FEATURE_PTP);
}

const char *
receiver_address(const struct receiver *r)
{
  return r->addr4 ? r->addr4 : r->addr6;
}

bool
output_ready(const struct output *out)
{
  int i;

  if (out->nmembers == 0 || (out->is_pair && out->nmembers < 2))
    return false;
  for (i = 0; i < out->nmembers; i++)
    if (!receiver_usable(out->members[i]))
      return false;
  return true;
}

// Every member has an address we last saw it at, whether or not mDNS sees it
// right now
bool
output_reachable(const struct output *out)
{
  if (out->nmembers == 0 || (out->is_pair && out->nmembers < 2))
    return false;
  for (int i = 0; i < out->nmembers; i++)
    {
      const struct receiver *r = out->members[i];
      if (!receiver_address(r) || !(r->features & FEATURE_BUFFERED_AUDIO) || !(r->features & FEATURE_PTP))
        return false;
    }
  return true;
}

struct receiver *
output_volume_leader(const struct output *out)
{
  int i;

  if (out->nmembers == 0)
    return NULL;
  if (!out->is_pair)
    return out->members[0];

  if (out->nmembers == 1)
    return out->members[0];

  // The member advertising itself as group leader; when both or neither do,
  // the first by name, so the choice is the same every time
  if (out->members[0]->igl != out->members[1]->igl)
    return out->members[0]->igl ? out->members[0] : out->members[1];
  i = (strcmp(out->members[1]->name, out->members[0]->name) < 0) ? 1 : 0;
  return out->members[i];
}

struct output *
devices_outputs(void)
{
  return outputs;
}

struct output *
devices_output_find(const char *id)
{
  struct output *out;

  for (out = outputs; out; out = out->next)
    if (strcmp(out->id, id) == 0)
      return out;
  return NULL;
}

struct receiver *
devices_receivers(void)
{
  return receivers;
}

void
devices_state_save(void)
{
  struct json_object *root, *outs;
  struct output *out;
  char tmp[4096];

  if (!state_file)
    return;

  // Start from what was saved, so outputs not seen this run keep their state
  root = json_object_new_object();
  outs = saved_state ? json_object_get(saved_state) : json_object_new_object();
  for (out = outputs; out; out = out->next)
    {
      struct json_object *o = json_object_new_object();
      json_object_object_add(o, "name", json_object_new_string(out->name));
      json_object_object_add(o, "selected", json_object_new_boolean(out->selected));
      json_object_object_add(o, "volume", json_object_new_int(out->volume));
      if (out->offset_ms)
        json_object_object_add(o, "offset_ms", json_object_new_int(out->offset_ms));
      json_object_object_add(outs, out->id, o);
    }
  json_object_object_add(root, "outputs", outs);
  if (settings)
    json_object_object_add(root, "settings", json_object_get(settings));

  snprintf(tmp, sizeof(tmp), "%s.tmp", state_file);
  if (json_object_to_file_ext(tmp, root, JSON_C_TO_STRING_PRETTY) < 0 || rename(tmp, state_file) < 0)
    log_warn(MOD, "Could not save state to %s: %s", state_file, strerror(errno));

  if (saved_state)
    json_object_put(saved_state);
  saved_state = json_object_get(outs);
  json_object_put(root);
}

int
devices_init(const char *state_path, devices_changed_cb cb, void *arg)
{
  struct json_object *root, *outs;
  char dir[4096];
  char *slash;

  changed_cb = cb;
  changed_arg = arg;
  state_file = xstrdup(state_path);

  snprintf(dir, sizeof(dir), "%s", state_path);
  slash = strrchr(dir, '/');
  if (slash)
    {
      *slash = '\0';
      mkdir(dir, 0755);
    }

  root = json_object_from_file(state_path);
  if (root && json_object_object_get_ex(root, "outputs", &outs))
    saved_state = json_object_get(outs);
  if (root && json_object_object_get_ex(root, "settings", &outs))
    settings = json_object_get(outs);
  if (root)
    json_object_put(root);
  if (!settings)
    settings = json_object_new_object();

  return 0;
}

int
devices_setting_get(const char *key, int def)
{
  struct json_object *v;

  if (settings && json_object_object_get_ex(settings, key, &v))
    return json_object_get_int(v);
  return def;
}

void
devices_setting_set(const char *key, int value)
{
  json_object_object_add(settings, key, json_object_new_int(value));
  devices_state_save();
}
