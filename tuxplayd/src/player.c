#include <math.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <event2/event.h>
#include <json-c/json.h>

#include "devices.h"
#include "log.h"
#include "player.h"
#include "session.h"
#include "sink.h"
#include "stream.h"
#include "timeline.h"
#include "util.h"

#define MOD "player"

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define RESTART_DELAY_MS 5000
// After our own network changed. An address removal comes after its route is
// gone, so the reconnect can follow at once. A link losing its carrier comes
// first, and its address and route only go a moment later.
#define ADDRESS_GONE_RECONNECT_MS 200
#define LINK_DOWN_RECONNECT_MS    1000
// Digital silence that ends a run of sound: new sound after this lets
// tuxplayd take the speakers back from another sender
#define SILENCE_MS       2000
#define PTP_RECHECK_MS   200
// An anchor must reach the receivers before the frame it names is due
#define ANCHOR_MARGIN_NS (80 * 1000000LL)

// Session state changes and audio transitions only schedule a check. Sessions
// are started, anchored, paused and freed from that check, never from inside a
// session's own callback.
static struct
{
  struct event_base *evbase;
  struct player_config cfg;
  struct event *check_ev;
  struct event *timeline_ev;
  char sender_id[18];
  uint32_t anchor_generation; // timeline the current anchors are on
  bool ptp_warned;

  // The input: audible, or digital silence for silence_detect_ms() or more
  bool audible;
  uint64_t audible_frame;   // first audible frame of the current run
  int64_t restart_after_ns; // a failed output is not restarted before this
  int64_t check_due_ns;     // when the pending check runs, 0 if none
  int network_reconnect_ms;  // delay after the latest network change
  bool network_retry;        // reconnect outputs mDNS cannot see yet

  int sync_ms;        // A/V correction: < 0 when the sound is late against the picture
  bool slider_synced; // the desktop slider has been set from a saved volume
  bool muted;
} pl;

static void session_state_changed(struct session *sess, enum session_state state, void *arg);
static void check_schedule(int delay_ms);

static double
volume_to_db(int volume)
{
  if (volume <= 0)
    return -144.0;
  return -30.0 + (double)volume * 30.0 / 100.0;
}

static int
db_to_volume(double db)
{
  if (db <= -30.0)
    return 0;
  if (db >= 0.0)
    return 100;
  return (int)lround((db + 30.0) * 100.0 / 30.0);
}

extern char **environ;

// Flush every member without telling them they are paused, so the check
// anchors them all again on one frame
static void
output_reanchor(struct output *out)
{
  for (int i = 0; i < out->nmembers; i++)
    if (out->members[i]->session)
      session_pause(out->members[i]->session, false);
  check_schedule(0);
}

// A tap on a speaker, or Siri. The command goes to the desktop's media
// player over MPRIS, as a Mac passes it to its now-playing app. A Mac's
// realtime stream simply carries on, so its speakers stay lit. On this
// buffered stream a tap also pauses the speaker's own rendering, which only
// a new anchor ends, so the output is anchored again at once: the speakers
// keep rendering (the paused player's silence, then the music) and stay lit.
static void
media_command(struct session *sess, const char *cmd, void *arg)
{
  struct receiver *r = session_receiver(sess);
  struct output *out = r ? r->output : NULL;

  static const struct { const char *airplay, *mpris; } map[] = {
    { "paus", "pause" }, { "play", "play" }, { "nitm", "next" }, { "pitm", "previous" },
  };
  const char *verb = NULL;
  pid_t pid;

  for (size_t i = 0; i < ARRAY_SIZE(map); i++)
    if (strcmp(cmd, map[i].airplay) == 0)
      verb = map[i].mpris;
  if (!verb)
    return;

  if (out && (strcmp(cmd, "paus") == 0 || strcmp(cmd, "play") == 0))
    output_reanchor(out);

  char *argv[] = { "playerctl", (char *)verb, NULL };
  if (posix_spawnp(&pid, "playerctl", NULL, NULL, argv, environ) != 0)
    log_warn(MOD, "Could not run playerctl %s (is playerctl installed?)", verb);
  else
    log_info(MOD, "Speaker asked to %s: playerctl %s", cmd, verb);
}

// What players are told the latency is. A negative sync_ms (sound late) makes
// it larger, so players hold the picture back by that much more.
static int
advertised_ms(void)
{
  return pl.cfg.lead_ms - pl.sync_ms;
}

static void
advertised_latency_push(void)
{
  sink_latency_set((int64_t)advertised_ms() * 1000000LL);
}

// Runs the check within delay_ms. A check already due sooner covers it: each
// check works out on its own what falls due later (a reconnect, a release),
// so a later deadline is never lost to an earlier one.
static void
check_schedule(int delay_ms)
{
  int64_t due = mono_ns() + (int64_t)delay_ms * 1000000LL;
  struct timeval tv = { delay_ms / 1000, (delay_ms % 1000) * 1000 };

  if (pl.check_due_ns && pl.check_due_ns <= due && evtimer_pending(pl.check_ev, NULL))
    return;
  pl.check_due_ns = due;
  evtimer_add(pl.check_ev, &tv);
}

static void
due_at(int64_t *next_ns, int64_t when_ns)
{
  if (when_ns < *next_ns)
    *next_ns = when_ns;
}

static bool
output_has_sessions(const struct output *out)
{
  for (int i = 0; i < out->nmembers; i++)
    if (out->members[i]->session)
      return true;
  return false;
}

// A pair's volume goes to its volume leader, which carries it to the other half
static void
output_volume_send(struct output *out)
{
  struct receiver *leader = output_volume_leader(out);
  enum session_state st;

  if (!leader || !leader->session)
    return;
  st = session_state(leader->session);
  if (st == SESSION_READY || st == SESSION_ANCHORING || st == SESSION_PLAYING)
    session_volume(leader->session, volume_to_db(pl.muted ? 0 : out->volume));
}

static void
slider_sync(const struct output *out)
{
  struct sink_volume v = { .volume = (float)out->volume / 100.0f, .mute = pl.muted };

  sink_volume_set(&v);
  pl.slider_synced = true;
}

/* ------------------------------ Start and stop ---------------------------- */

#define GROUP_MAX 16

// Every receiver of every selected output: one PTP mesh, one grandmaster
static int
group_addresses(const char *addrs[GROUP_MAX])
{
  struct output *out;
  int n = 0;

  for (out = devices_outputs(); out; out = out->next)
    if (out->selected)
      for (int i = 0; i < out->nmembers && n < GROUP_MAX; i++)
        if (receiver_usable(out->members[i]) && receiver_address(out->members[i]))
          addrs[n++] = receiver_address(out->members[i]);
  return n;
}

// Tell the sessions already up that the group changed
static void
group_update(const struct output *changed)
{
  const char *addrs[GROUP_MAX];
  int n = group_addresses(addrs);
  struct output *out;

  for (out = devices_outputs(); out; out = out->next)
    if (out != changed)
      for (int i = 0; i < out->nmembers; i++)
        if (out->members[i]->session)
          session_peers_set(out->members[i]->session, addrs, n);
}

static void
output_stop(struct output *out)
{
  for (int i = 0; i < out->nmembers; i++)
    {
      struct session *sess = out->members[i]->session;
      if (!sess)
        continue;
      session_stop(sess);
      session_free(sess);
    }
}

static void
output_start(struct output *out)
{
  struct session_params sp;
  const char *peers[GROUP_MAX];
  char group_uuid[37];

  // After our own network changed, the speakers' last addresses are tried
  // before mDNS has found them again, which takes several seconds
  if (output_has_sessions(out) || !(output_ready(out) || (pl.network_retry && output_reachable(out))))
    return;

  // One group id for everything started together, as a Mac does
  uuid_make(group_uuid);
  sp.sender_name = "tuxplay";
  sp.sender_id = pl.sender_id;
  sp.group_uuid = group_uuid;
  sp.clock_id = timeline_clock_id();
  sp.peers = peers;
  sp.npeers = group_addresses(peers);
  sp.media_cb = media_command;
  sp.media_arg = NULL;

  log_info(MOD, "%s: starting %d session(s), %d receiver(s) in the group", out->name, out->nmembers, sp.npeers);
  for (int i = 0; i < out->nmembers; i++)
    if (!session_start(pl.evbase, out->members[i], &sp, session_state_changed, NULL))
      {
        output_stop(out);
        pl.restart_after_ns = mono_ns() + RESTART_DELAY_MS * 1000000LL;
        check_schedule(RESTART_DELAY_MS);
        return;
      }
  group_update(out);
}

/* -------------------------------- Anchoring -------------------------------- */

// Every output is anchored on one line: frame F renders at the time F was
// produced plus the lead. Outputs started at different moments are therefore
// in step, and a resume plays from the first audible frame instead of
// clipping the start of the sound.
static void
output_anchor(struct output *out, const struct timeline_snapshot *snap)
{
  int64_t lead_ns = (int64_t)pl.cfg.lead_ms * 1000000LL;
  int64_t offset_ns = (int64_t)out->offset_ms * 1000000LL;
  uint64_t join_samples = (uint64_t)((int64_t)pl.cfg.join_ms * 1000000LL * SINK_RATE / NS_PER_SEC);
  int64_t now_gm = timeline_local_to_gm(snap, mono_ns());
  uint64_t frame = stream_head() + (join_samples + STREAM_SPF - 1) / STREAM_SPF;
  int64_t render_gm_ns;

  if (pl.audible_frame < frame &&
      stream_frame_gm(pl.audible_frame) + lead_ns + offset_ns - now_gm > ANCHOR_MARGIN_NS)
    frame = pl.audible_frame;
  render_gm_ns = stream_frame_gm(frame) + lead_ns + offset_ns;

  pl.network_retry = false;
  log_info(MOD, "%s: anchoring %d member(s) at frame %llu, renders in %lld ms (lead %d ms)",
           out->name, out->nmembers, (unsigned long long)frame,
           (long long)((render_gm_ns - now_gm) / 1000000), pl.cfg.lead_ms);

  for (int i = 0; i < out->nmembers; i++)
    session_anchor(out->members[i]->session, frame, render_gm_ns, snap->gm_id);
  pl.anchor_generation = snap->generation;
}

/* --------------------------------- Check ---------------------------------- */

static void
output_check(struct output *out, const struct timeline_snapshot *snap, int64_t *next_ns)
{
  int64_t now = mono_ns();
  int64_t release_ms = (int64_t)pl.cfg.idle_release_s * 1000;
  bool failed = false, taken = false, lost_net = false;
  int n = 0, ready = 0, playing = 0;

  for (int i = 0; i < out->nmembers; i++)
    {
      struct session *sess = out->members[i]->session;

      if (!sess)
        continue;
      n++;
      switch (session_state(sess))
        {
        case SESSION_FAILED:
        case SESSION_STOPPED:
          failed = true;
          taken |= session_taken_over(sess);
          lost_net |= session_lost_network(sess);
          break;
        case SESSION_READY:
          ready++;
          break;
        case SESSION_PLAYING:
          playing++;
          break;
        default:
          break;
        }
    }

  // A pair plays together or not at all
  if (failed)
    {
      output_stop(out);
      if (taken)
        {
          // Another sender has the speakers. Stay selected, but leave them
          // alone until there is new sound here, as pressing play would.
          log_info(MOD, "%s: taken over by another sender, standing down until there is new sound here", out->name);
          out->claimed = false;
        }
      else if (out->claimed)
        {
          int delay = lost_net ? pl.network_reconnect_ms : RESTART_DELAY_MS;

          if (lost_net)
            pl.network_retry = true;

          log_info(MOD, "%s: %s, reconnecting in %d ms", out->name,
                   lost_net ? "our network changed" : "lost", delay);
          pl.restart_after_ns = mono_ns() + delay * 1000000LL;
          check_schedule(delay);
        }
      return;
    }

  if (!out->selected || !out->claimed)
    return;

  if (n == 0)
    {
      if (now >= pl.restart_after_ns)
        output_start(out);
      else
        due_at(next_ns, pl.restart_after_ns);
      return;
    }

  // Nothing played for the idle time: release the speakers. The output stays
  // selected, and the next sound here takes them again.
  if (!pl.audible && release_ms > 0)
    {
      int64_t silent_ms = stream_silence_ms();

      if (silent_ms >= release_ms)
        {
          log_info(MOD, "%s: nothing played for %d min, releasing the speakers", out->name,
                   pl.cfg.idle_release_s / 60);
          output_stop(out);
          out->claimed = false;
          return;
        }
      due_at(next_ns, now + (release_ms - silent_ms + 50) * 1000000LL);
    }

  if (!pl.audible && pl.cfg.pause_ms > 0)
    {
      // Pausing on silence: the speakers go idle, the sessions stay
      if (playing)
        for (int i = 0; i < out->nmembers; i++)
          session_pause(out->members[i]->session, true);
      return;
    }

  // Every member set up and waiting: start them together. Unless pausing on
  // silence, that is straight away, and silence is streamed like anything
  // else, as macOS 27 does.
  if (ready == out->nmembers)
    {
      uint32_t sink_generation;

      // Frame times come from the sink, which moves to a new timeline on its
      // next cycle: anchor only once both agree
      // Receivers only take an anchor on the grandmaster they follow
      sink_position(NULL, NULL, &sink_generation);
      if (snap->state != TIMELINE_FOLLOWING || sink_generation != snap->generation)
        {
          if (snap->state == TIMELINE_OWN && !pl.ptp_warned)
            log_warn(MOD, "%s: not following the receivers' clock yet, waiting to anchor", out->name);
          pl.ptp_warned = snap->state == TIMELINE_OWN;
          due_at(next_ns, now + PTP_RECHECK_MS * 1000000LL);
          return;
        }
      pl.ptp_warned = false;
      output_anchor(out, snap);
    }
}

static void
check_cb(evutil_socket_t fd, short what, void *arg)
{
  struct timeline_snapshot snap;
  struct output *out;
  int64_t next_ns = INT64_MAX;

  pl.check_due_ns = 0;
  timeline_get(&snap);
  for (out = devices_outputs(); out; out = out->next)
    {
      output_check(out, &snap, &next_ns);

      // The slider follows the speakers from the moment they are reachable
      if (!pl.slider_synced && out->selected && output_has_sessions(out))
        slider_sync(out);
    }

  if (next_ns != INT64_MAX)
    check_schedule((int)(MAX(next_ns - mono_ns(), 0) / 1000000));
}

// A new grandmaster (one left, or a better one appeared): anchors on the old
// timeline no longer say when anything plays. Pause what is playing; the
// check then anchors it again on the new one.
static void
timeline_cb(evutil_socket_t fd, short what, void *arg)
{
  struct timeline_snapshot snap;
  struct timeval tv = { 1, 0 };
  struct output *out;
  bool logged = false;

  timeline_get(&snap);
  if (snap.generation != pl.anchor_generation)
    for (out = devices_outputs(); out; out = out->next)
      for (int i = 0; i < out->nmembers; i++)
        {
          struct session *sess = out->members[i]->session;
          if (!sess || (session_state(sess) != SESSION_PLAYING && session_state(sess) != SESSION_ANCHORING))
            continue;
          if (!logged)
            log_info(MOD, "Timeline changed (now %016llx), re-anchoring", (unsigned long long)snap.gm_id);
          logged = true;
          session_pause(sess, false);
        }
  evtimer_add(pl.timeline_ev, &tv);
}

// A local address was removed, or a link went down: sessions on it are dead
// even if their sockets do not know yet. A new address may mean the network
// is back: retry what is waiting at once rather than on the next tick.
void
player_network_changed(enum netwatch_event ev, const char *addr, int ifindex, void *arg)
{
  struct output *out;

  if (ev == NETWATCH_ADDR_NEW)
    {
      for (out = devices_outputs(); out; out = out->next)
        if (out->selected && out->claimed && !output_has_sessions(out))
          {
            // The address comes a moment before its routes
            pl.restart_after_ns = mono_ns() + ADDRESS_GONE_RECONNECT_MS * 1000000LL;
            check_schedule(ADDRESS_GONE_RECONNECT_MS);
            break;
          }
      return;
    }

  for (out = devices_outputs(); out; out = out->next)
    for (int i = 0; i < out->nmembers; i++)
      {
        struct session *sess = out->members[i]->session;

        if (!sess || session_state(sess) == SESSION_FAILED || session_state(sess) == SESSION_STOPPED)
          continue;
        if (addr ? strcmp(session_local_address(sess), addr) == 0
                 : (ifindex > 0 && session_ifindex(sess) == ifindex))
          {
            pl.network_reconnect_ms = addr ? ADDRESS_GONE_RECONNECT_MS : LINK_DOWN_RECONNECT_MS;
            session_network_lost(sess);
          }
      }
}

// The speakers keep their own volume, as with a Mac: on connecting, the
// volume leader's level becomes the output's, and the desktop slider moves to
// it. Volume is sent only when someone changes it here.
static void
session_state_changed(struct session *sess, enum session_state state, void *arg)
{
  struct receiver *r = session_receiver(sess);
  struct output *out = r ? r->output : NULL;
  double db;

  if (state == SESSION_READY && out && output_volume_leader(out) == r && session_initial_volume(sess, &db))
    {
      int volume = db_to_volume(db);
      if (volume != out->volume)
        {
          log_info(MOD, "%s: the speakers are at %.1f dB, volume %d", out->name, db, volume);
          out->volume = volume;
          devices_state_save();
        }
      if (out->selected)
        slider_sync(out);
    }
  check_schedule(0);
}

static int
silence_detect_ms(void)
{
  return pl.cfg.pause_ms > 0 ? pl.cfg.pause_ms : SILENCE_MS;
}

static void
activity_cb(bool audible, uint64_t frame, void *arg)
{
  struct output *out;

  pl.audible = audible;
  log_info(MOD, "Input %s at frame %llu", audible ? "audible" : "silent", (unsigned long long)frame);

  // New sound claims every selected output, even one another sender took
  if (audible)
    {
      pl.audible_frame = frame;
      for (out = devices_outputs(); out; out = out->next)
        if (out->selected)
          out->claimed = true;
    }

  check_schedule(0);
}

/* ---------------------------- Public interface ---------------------------- */

int
player_init(struct event_base *evbase, const struct player_config *cfg)
{
  uint8_t mac[6];

  pl.evbase = evbase;
  pl.cfg = *cfg;
  if (pl.cfg.lead_ms <= 0)
    pl.cfg.lead_ms = 250;
  if (pl.cfg.join_ms <= 0)
    pl.cfg.join_ms = 100;
  if (pl.cfg.pause_ms < 0)
    pl.cfg.pause_ms = 0;
  pl.check_ev = evtimer_new(evbase, check_cb, NULL);
  pl.timeline_ev = evtimer_new(evbase, timeline_cb, NULL);
  {
    struct timeval tv = { 1, 0 };
    evtimer_add(pl.timeline_ev, &tv);
  }

  if (!local_mac_get(mac))
    memset(mac, 0x5a, sizeof(mac));
  mac_format(pl.sender_id, mac);

  pl.cfg.lead_ms = devices_setting_get("lead_ms", pl.cfg.lead_ms);
  pl.cfg.idle_release_s = devices_setting_get("idle_release_s", pl.cfg.idle_release_s);
  pl.sync_ms = devices_setting_get("sync_ms", 0);
  advertised_latency_push();

  stream_init();
  stream_activity_cb_set(silence_detect_ms(), activity_cb, NULL);
  return 0;
}

void
player_stop(void)
{
  struct output *out;

  for (out = devices_outputs(); out; out = out->next)
    output_stop(out);
  event_free(pl.check_ev);
  event_free(pl.timeline_ev);
}

// Discovery: a remembered output comes back, or a pair's second half appears.
// A remembered output is only connected once something plays (or the user
// selects it), so starting tuxplayd never takes speakers from another sender.
void
player_output_changed(struct output *out, void *arg)
{
  if (out->selected && out->claimed && output_ready(out) && !output_has_sessions(out) &&
      mono_ns() >= pl.restart_after_ns)
    output_start(out);
}

int
player_output_select(struct output *out, bool selected)
{
  // Selecting what is already selected still means "use these speakers now",
  // for instance after another sender took them
  if (out->selected && selected)
    {
      out->claimed = true;
      output_start(out);
      return 0;
    }
  if (out->selected == selected)
    return 0;

  out->selected = selected;
  out->claimed = selected;
  log_info(MOD, "%s: %s", out->name, selected ? "selected" : "deselected");
  devices_state_save();

  if (selected)
    output_start(out);
  else
    {
      output_stop(out);
      group_update(out);
    }
  return 0;
}

int
player_output_volume(struct output *out, int volume)
{
  if (volume < 0)
    volume = 0;
  if (volume > 100)
    volume = 100;
  if (out->volume == volume)
    return 0;
  out->volume = volume;
  devices_state_save();
  output_volume_send(out);
  return 0;
}

// The slider on tuxplayd's own sink sets the speakers' volume: PipeWire does
// not attenuate (see sink.c), so the speakers are the only volume there is
void
player_desktop_volume(const struct sink_volume *vol, void *arg)
{
  struct output *out;
  int volume = (int)lroundf(vol->volume * 100.0f);
  bool mute_changed = vol->mute != pl.muted;

  log_debug(MOD, "desktop volume %.2f%s", vol->volume, vol->mute ? " (muted)" : "");

  // Until the slider has been set from a saved volume it says nothing about
  // what the speakers should play at (a fresh sink starts at 100%)
  if (!pl.slider_synced)
    return;

  pl.muted = vol->mute;
  for (out = devices_outputs(); out; out = out->next)
    if (out->selected)
      {
        if (out->volume != volume)
          player_output_volume(out, volume);
        else if (mute_changed)
          output_volume_send(out);
      }
}

const char *
player_output_state(const struct output *out)
{
  bool any_starting = false, any_playing = false, any_ready = false;

  for (int i = 0; i < out->nmembers; i++)
    {
      struct session *s = out->members[i]->session;
      if (!s)
        continue;
      switch (session_state(s))
        {
        case SESSION_PLAYING:
          any_playing = true;
          break;
        case SESSION_READY:
          any_ready = true;
          break;
        case SESSION_STARTING:
        case SESSION_ANCHORING:
          any_starting = true;
          break;
        default:
          break;
        }
    }
  if (any_playing)
    return "playing";
  if (any_starting)
    return "starting";
  if (any_ready)
    return "connected";
  if (out->selected)
    return "selected";
  return "idle";
}

int
player_lead_ms(void)
{
  return pl.cfg.lead_ms;
}

int
player_sync_ms(void)
{
  return pl.sync_ms;
}

int
player_idle_release_s(void)
{
  return pl.cfg.idle_release_s;
}

// 0 = never release
int
player_idle_release_set(int seconds)
{
  if (seconds < 0 || seconds > 24 * 3600)
    return -1;
  pl.cfg.idle_release_s = seconds;
  devices_setting_set("idle_release_s", seconds);
  if (seconds)
    log_info(MOD, "Releasing the speakers after %d min without sound", seconds / 60);
  else
    log_info(MOD, "Never releasing the speakers when idle");
  check_schedule(0);
  return 0;
}

// Takes effect at once: what is playing is paused and anchored again with the
// new lead (a short gap), since an anchor cannot be moved while it plays
int
player_lead_set(int ms)
{
  struct output *out;

  if (ms < 50 || ms > 5000)
    return -1;
  if (ms == pl.cfg.lead_ms)
    return 0;
  pl.cfg.lead_ms = ms;
  devices_setting_set("lead_ms", ms);
  advertised_latency_push();
  log_info(MOD, "Lead set to %d ms", ms);

  for (out = devices_outputs(); out; out = out->next)
    for (int i = 0; i < out->nmembers; i++)
      if (out->members[i]->session)
        session_pause(out->members[i]->session, false);
  return 0;
}

// Only what players are told changes; the sound itself is not touched
int
player_sync_set(int ms)
{
  if (ms < -500 || ms > 500)
    return -1;
  pl.sync_ms = ms;
  devices_setting_set("sync_ms", ms);
  advertised_latency_push();
  log_info(MOD, "Sync correction %+d ms", ms);
  return 0;
}

struct json_object *
player_status_json(void)
{
  struct json_object *o = json_object_new_object();
  struct timeline_snapshot snap;
  char gm[17];

  timeline_get(&snap);
  snprintf(gm, sizeof(gm), "%016llx", (unsigned long long)snap.gm_id);
  json_object_object_add(o, "lead_ms", json_object_new_int(pl.cfg.lead_ms));
  json_object_object_add(o, "sync_ms", json_object_new_int(pl.sync_ms));
  json_object_object_add(o, "advertised_ms", json_object_new_int(advertised_ms()));
  json_object_object_add(o, "idle_release_s", json_object_new_int(pl.cfg.idle_release_s));
  json_object_object_add(o, "audible", json_object_new_boolean(pl.audible));
  json_object_object_add(o, "silence_ms", json_object_new_int64(stream_silence_ms()));
  json_object_object_add(o, "ptp", json_object_new_string(
    snap.state == TIMELINE_FOLLOWING ? "following" : snap.state == TIMELINE_WAITING ? "waiting" : "own clock"));
  json_object_object_add(o, "timeline_id", json_object_new_string(gm));
  return o;
}
