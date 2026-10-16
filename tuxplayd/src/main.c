#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <event2/event.h>
#include <sodium.h>

#include "api.h"
#include "devices.h"
#include "log.h"
#include "mdns.h"
#include "netwatch.h"
#include "player.h"
#include "sink.h"
#include "stream.h"
#include "timeline.h"
#include "util.h"

#define MOD "main"

static struct event_base *evbase;

static void
sink_ev_cb(evutil_socket_t fd, short what, void *arg)
{
  sink_drain();
}

static void
mdns_ev_cb(evutil_socket_t fd, short what, void *arg)
{
  mdns_drain();
}

static void
signal_cb(evutil_socket_t fd, short what, void *arg)
{
  log_info(MOD, "Signal %d, shutting down", (int)fd);
  event_base_loopbreak(evbase);
}

int
main(int argc, char **argv)
{
  struct sink_config scfg = {
    .node_name = "tuxplay",
    .description = "Tuxplay (AirPlay 2)",
    .quantum = 480,
    .latency_ns = 250 * 1000000LL,
  };
  struct player_config pcfg = {
    .lead_ms = 250,
    .join_ms = 100,
    .pause_ms = 0,
    .idle_release_s = 0,
  };
  const char *state_path = NULL;
  char default_state[4096];
  const char *api_addr = "127.0.0.1";
  uint16_t api_port = 3689;
  struct event *ev_sink, *ev_mdns, *ev_int, *ev_term;
  uint64_t clock_id;
  uint8_t mac[6];
  int opt;

  while ((opt = getopt(argc, argv, "vn:d:l:s:p:P:i:")) != -1)
    {
      switch (opt)
        {
          case 'v': log_threshold = LOG_DEBUG; break;
          case 'n': scfg.node_name = optarg; break;
          case 'd': scfg.description = optarg; break;
          case 'l': pcfg.lead_ms = atoi(optarg); break;
          case 's': state_path = optarg; break;
          case 'p': api_port = (uint16_t)atoi(optarg); break;
          case 'P': pcfg.pause_ms = atoi(optarg); break;
          case 'i': pcfg.idle_release_s = atoi(optarg); break;
          default:
            fprintf(stderr, "usage: %s [-v] [-n sink_name] [-d sink_description] [-l lead_ms] [-s state_file]\n"
                            "       [-p api_port] [-P pause_after_ms, default 0 = never] [-i release_after_s]\n", argv[0]);
            return 2;
        }
    }

  scfg.latency_ns = (int64_t)pcfg.lead_ms * 1000000LL;

  // Selected outputs and their volumes, kept across restarts
  if (!state_path)
    {
      const char *xdg = getenv("XDG_STATE_HOME");
      if (xdg && *xdg)
        snprintf(default_state, sizeof(default_state), "%s/tuxplayd.json", xdg);
      else
        snprintf(default_state, sizeof(default_state), "%s/.local/state/tuxplayd.json", getenv("HOME") ? getenv("HOME") : ".");
      state_path = default_state;
    }

  if (sodium_init() < 0)
    return 1;

  evbase = event_base_new();

  // Children (playerctl) are not waited for: let the kernel reap them
  signal(SIGCHLD, SIG_IGN);

  // Apple's form: the MAC followed by 0x0008 (the pair's grandmaster here is
  // acbcb5060f670008)
  if (!local_mac_get(mac))
    memset(mac, 0x5a, sizeof(mac));
  clock_id = 0x0008;
  for (int i = 0; i < 6; i++)
    clock_id |= (uint64_t)mac[i] << (56 - 8 * i);

  if (timeline_start(clock_id) < 0)
    log_warn(MOD, "No PTP, running on our own clock");

  devices_init(state_path, player_output_changed, NULL);

  if (sink_start(&scfg, stream_audio_cb, player_desktop_volume, NULL) < 0)
    return 1;

  player_init(evbase, &pcfg);

  if (mdns_start(devices_mdns_cb, NULL) < 0)
    {
      log_err(MOD, "mDNS failed, cannot discover speakers");
      return 1;
    }

  if (api_start(evbase, api_addr, api_port) < 0)
    return 1;

  // Without it a network change is still survived, only more slowly (a
  // keep-alive has to time out first)
  netwatch_start(evbase, player_network_changed, NULL);

  ev_sink = event_new(evbase, sink_notify_fd(), EV_READ | EV_PERSIST, sink_ev_cb, NULL);
  event_add(ev_sink, NULL);
  ev_mdns = event_new(evbase, mdns_notify_fd(), EV_READ | EV_PERSIST, mdns_ev_cb, NULL);
  event_add(ev_mdns, NULL);
  ev_int = evsignal_new(evbase, SIGINT, signal_cb, NULL);
  ev_term = evsignal_new(evbase, SIGTERM, signal_cb, NULL);
  event_add(ev_int, NULL);
  event_add(ev_term, NULL);

  log_info(MOD, "tuxplayd running (lead %d ms, node %s, API :%u)",
           pcfg.lead_ms, scfg.node_name, api_port);

  event_base_dispatch(evbase);

  log_info(MOD, "Shutting down");
  player_stop();
  netwatch_stop();
  api_stop();
  mdns_stop();
  sink_stop();
  timeline_stop();
  devices_state_save();

  event_free(ev_sink);
  event_free(ev_mdns);
  event_free(ev_int);
  event_free(ev_term);
  event_base_free(evbase);
  return 0;
}
