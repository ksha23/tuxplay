#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/thread-watch.h>
#include <avahi-common/malloc.h>

#include "log.h"
#include "mdns.h"
#include "util.h"

#define MOD "mdns"

// Avahi runs its own thread. Its callbacks queue events here and the main
// thread takes them off with mdns_drain(), so the device registry is only
// ever touched from one thread.
//
// Resolvers are kept for as long as the service exists: Avahi then reports
// every TXT change (a stereo pair's leader hints, a speaker's group id)
// without anyone having to poll. Nothing here opens a connection to a
// receiver - discovery is discovery, and a speaker is not probed just because
// its mDNS record was refreshed.

struct event_node
{
  struct mdns_service svc;
  bool removed;
  struct event_node *next;
};

struct resolver_node
{
  AvahiServiceResolver *r;
  char *name;
  AvahiIfIndex iface;
  AvahiProtocol proto;
  struct resolver_node *next;
};

static AvahiThreadedPoll *tpoll;
static AvahiClient *client;
static AvahiServiceBrowser *browser;
static struct resolver_node *resolvers;

static pthread_mutex_t q_lck = PTHREAD_MUTEX_INITIALIZER;
static struct event_node *q_head, *q_tail;
static int notify_fd = -1;

static mdns_service_cb service_cb;
static void *service_cb_arg;

static void
svc_free(struct mdns_service *svc)
{
  int i;

  free(svc->name);
  free(svc->host);
  free(svc->address);
  for (i = 0; i < svc->ntxt; i++)
    {
      free(svc->txt[i].key);
      free(svc->txt[i].value);
    }
}

static void
enqueue(struct event_node *ev)
{
  uint64_t one = 1;
  ssize_t ret;

  pthread_mutex_lock(&q_lck);
  if (q_tail)
    q_tail->next = ev;
  else
    q_head = ev;
  q_tail = ev;
  pthread_mutex_unlock(&q_lck);

  ret = write(notify_fd, &one, sizeof(one));
  (void)ret;
}

static void
resolve_cb(AvahiServiceResolver *r, AvahiIfIndex iface, AvahiProtocol proto, AvahiResolverEvent event,
           const char *name, const char *type, const char *domain, const char *host, const AvahiAddress *addr,
           uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags flags, void *userdata)
{
  struct event_node *ev;
  char abuf[AVAHI_ADDRESS_STR_MAX];
  char ifname[IF_NAMESIZE];
  AvahiStringList *l;

  if (event != AVAHI_RESOLVER_FOUND)
    {
      log_debug(MOD, "Resolver for '%s' failed: %s", name, avahi_strerror(avahi_client_errno(client)));
      return;
    }

  avahi_address_snprint(abuf, sizeof(abuf), addr);

  ev = xcalloc(1, sizeof(*ev));
  ev->svc.name = xstrdup(name);
  ev->svc.host = xstrdup(host);
  ev->svc.ipv6 = (addr->proto == AVAHI_PROTO_INET6);
  ev->svc.ifindex = iface;
  ev->svc.port = port;

  // A link-local IPv6 address is only usable with its interface
  if (ev->svc.ipv6 && strncasecmp(abuf, "fe80:", 5) == 0 && if_indextoname(iface, ifname))
    {
      size_t n = strlen(abuf) + strlen(ifname) + 2;
      ev->svc.address = malloc(n);
      snprintf(ev->svc.address, n, "%s%%%s", abuf, ifname);
    }
  else
    ev->svc.address = xstrdup(abuf);

  for (l = txt; l && ev->svc.ntxt < MDNS_TXT_MAX; l = avahi_string_list_get_next(l))
    {
      char *key = NULL, *value = NULL;
      size_t size;

      if (avahi_string_list_get_pair(l, &key, &value, &size) < 0)
        continue;
      ev->svc.txt[ev->svc.ntxt].key = xstrdup(key);
      ev->svc.txt[ev->svc.ntxt].value = xstrdup(value ? value : "");
      ev->svc.ntxt++;
      avahi_free(key);
      avahi_free(value);
    }

  enqueue(ev);
}

static void
browse_cb(AvahiServiceBrowser *b, AvahiIfIndex iface, AvahiProtocol proto, AvahiBrowserEvent event,
          const char *name, const char *type, const char *domain, AvahiLookupResultFlags flags, void *userdata)
{
  struct resolver_node *rn, **prev;
  struct event_node *ev;

  switch (event)
    {
      case AVAHI_BROWSER_NEW:
        for (rn = resolvers; rn; rn = rn->next)
          if (rn->iface == iface && rn->proto == proto && strcmp(rn->name, name) == 0)
            return;

        rn = xcalloc(1, sizeof(*rn));
        rn->name = xstrdup(name);
        rn->iface = iface;
        rn->proto = proto;
        rn->r = avahi_service_resolver_new(client, iface, proto, name, type, domain, proto, 0, resolve_cb, NULL);
        if (!rn->r)
          {
            log_warn(MOD, "Could not resolve '%s': %s", name, avahi_strerror(avahi_client_errno(client)));
            free(rn->name);
            free(rn);
            return;
          }
        rn->next = resolvers;
        resolvers = rn;
        break;

      case AVAHI_BROWSER_REMOVE:
        for (prev = &resolvers; (rn = *prev); prev = &rn->next)
          {
            if (rn->iface == iface && rn->proto == proto && strcmp(rn->name, name) == 0)
              {
                *prev = rn->next;
                avahi_service_resolver_free(rn->r);
                free(rn->name);
                free(rn);
                break;
              }
          }

        ev = xcalloc(1, sizeof(*ev));
        ev->svc.name = xstrdup(name);
        ev->svc.ipv6 = (proto == AVAHI_PROTO_INET6);
        ev->svc.ifindex = iface;
        ev->removed = true;
        enqueue(ev);
        break;

      case AVAHI_BROWSER_FAILURE:
        log_err(MOD, "Browser failure: %s", avahi_strerror(avahi_client_errno(client)));
        break;

      default:
        break;
    }
}

static void
client_cb(AvahiClient *c, AvahiClientState state, void *userdata)
{
  switch (state)
    {
      case AVAHI_CLIENT_S_RUNNING:
        if (!browser)
          {
            browser = avahi_service_browser_new(c, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_airplay._tcp", NULL, 0, browse_cb, NULL);
            if (!browser)
              log_err(MOD, "Could not browse _airplay._tcp: %s", avahi_strerror(avahi_client_errno(c)));
          }
        break;

      case AVAHI_CLIENT_FAILURE:
        log_err(MOD, "Avahi client failure: %s", avahi_strerror(avahi_client_errno(c)));
        break;

      default:
        break;
    }
}

int
mdns_start(mdns_service_cb cb, void *arg)
{
  int err;

  service_cb = cb;
  service_cb_arg = arg;

  notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (notify_fd < 0)
    return -1;

  tpoll = avahi_threaded_poll_new();
  if (!tpoll)
    return -1;

  client = avahi_client_new(avahi_threaded_poll_get(tpoll), AVAHI_CLIENT_NO_FAIL, client_cb, NULL, &err);
  if (!client)
    {
      log_err(MOD, "Could not create Avahi client: %s", avahi_strerror(err));
      return -1;
    }

  if (avahi_threaded_poll_start(tpoll) < 0)
    return -1;

  log_info(MOD, "Browsing for AirPlay receivers");
  return 0;
}

void
mdns_stop(void)
{
  if (tpoll)
    avahi_threaded_poll_stop(tpoll);
  if (client)
    avahi_client_free(client);
  if (tpoll)
    avahi_threaded_poll_free(tpoll);
  tpoll = NULL;
  client = NULL;
}

int
mdns_notify_fd(void)
{
  return notify_fd;
}

void
mdns_drain(void)
{
  struct event_node *ev, *next;
  uint64_t counter;
  ssize_t ret;

  ret = read(notify_fd, &counter, sizeof(counter));
  (void)ret;

  pthread_mutex_lock(&q_lck);
  ev = q_head;
  q_head = q_tail = NULL;
  pthread_mutex_unlock(&q_lck);

  for (; ev; ev = next)
    {
      next = ev->next;
      service_cb(&ev->svc, ev->removed, service_cb_arg);
      svc_free(&ev->svc);
      free(ev);
    }
}

const char *
mdns_txt_get(const struct mdns_service *svc, const char *key)
{
  int i;

  for (i = 0; i < svc->ntxt; i++)
    if (strcmp(svc->txt[i].key, key) == 0)
      return svc->txt[i].value;
  return NULL;
}
