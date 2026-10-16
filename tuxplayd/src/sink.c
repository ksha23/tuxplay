#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include "log.h"
#include "sink.h"
#include "timeline.h"
#include "util.h"

#define MOD "sink"

// Sample ring between the PipeWire data thread and the main thread. A power of
// two, 1.4 s at 48 kHz: the main thread only has to keep up on average.
#define RING_FRAMES (1 << 16)
#define RING_MASK (RING_FRAMES - 1)

// A trigger that has not produced a cycle by then is counted as missed
#define CYCLE_STALL_NS (100 * 1000000LL)

static struct
{
  struct sink_config cfg;
  sink_audio_cb audio_cb;
  sink_volume_cb volume_cb;
  void *cb_arg;

  struct pw_thread_loop *loop;
  struct pw_stream *stream;
  struct spa_source *timer;
  int notify_fd;
  bool connected;

  // Ring: written by the data thread, read by the main thread
  int32_t ring[RING_FRAMES * SINK_CHANNELS];
  // The ring is addressed by sample index: slot = index & RING_MASK. A
  // skipped cycle leaves a hole, and ring_epoch marks where the latest
  // contiguous run of samples began, so the reader never hands on stale slots.
  _Atomic uint64_t ring_write; // index one past the last sample written
  _Atomic uint64_t ring_epoch; // index the current contiguous run starts at
  uint64_t ring_read;          // main thread only

  // Pacing, owned by the loop thread. Sample index n of the current timeline
  // generation is due at gm_base + (n - index_base) / rate.
  pthread_mutex_t pos_lck;
  uint32_t generation;
  int64_t gm_base;
  uint64_t index_base;
  uint64_t next_index;    // first sample of the next cycle
  int64_t triggered_ns;   // local time of the outstanding trigger, 0 if none
  _Atomic uint64_t cycle_index; // index the data thread writes the next cycle at
  _Atomic bool cycle_done;

  // Volume, written in the loop thread, read on the main thread
  pthread_mutex_t vol_lck;
  struct sink_volume vol;
  bool vol_pending;
} s = {
  .pos_lck = PTHREAD_MUTEX_INITIALIZER,
  .vol_lck = PTHREAD_MUTEX_INITIALIZER,
};

static void
notify(void)
{
  uint64_t one = 1;
  ssize_t ret = write(s.notify_fd, &one, sizeof(one));
  (void)ret;
}

/* ------------------------------- Data thread ------------------------------ */

static void
ring_put(uint64_t w, const int32_t *frames, uint32_t n)
{
  uint32_t i;

  // A reader slower than a whole ring is overwritten rather than allowed to
  // block the graph; it notices from the indices.
  for (i = 0; i < n; i++)
    {
      uint64_t slot = (w + i) & RING_MASK;
      if (frames)
        {
          s.ring[slot * 2] = frames[i * 2];
          s.ring[slot * 2 + 1] = frames[i * 2 + 1];
        }
      else
        {
          s.ring[slot * 2] = 0;
          s.ring[slot * 2 + 1] = 0;
        }
    }
}

// One graph cycle has finished: whatever the clients wrote is in the buffer.
// Exactly one quantum is appended per cycle - padded with silence when the
// graph produced less, or nothing is linked - so the sample count stays
// locked to the schedule the cycles were triggered on.
static void
on_process(void *data)
{
  struct pw_buffer *b;
  uint32_t want = s.cfg.quantum;
  uint32_t have = 0;
  const int32_t *frames = NULL;
  uint64_t index = atomic_load(&s.cycle_index);

  if (index != atomic_load_explicit(&s.ring_write, memory_order_relaxed))
    atomic_store(&s.ring_epoch, index);

  b = pw_stream_dequeue_buffer(s.stream);
  if (b)
    {
      struct spa_data *d = &b->buffer->datas[0];
      if (d->data && d->chunk)
        {
          uint32_t off = SPA_MIN(d->chunk->offset, d->maxsize);
          have = SPA_MIN(d->chunk->size, d->maxsize - off) / (sizeof(int32_t) * SINK_CHANNELS);
          frames = SPA_PTROFF(d->data, off, const int32_t);
        }
    }

  if (have > want)
    have = want;
  if (have)
    ring_put(index, frames, have);
  if (have < want)
    ring_put(index + have, NULL, want - have);
  atomic_store_explicit(&s.ring_write, index + want, memory_order_release);

  if (b)
    pw_stream_queue_buffer(s.stream, b);

  atomic_store(&s.cycle_done, true);
  notify();
}

/* ------------------------------- Loop thread ------------------------------ */

static void
schedule(int64_t local_ns)
{
  struct timespec ts = { .tv_sec = local_ns / NS_PER_SEC, .tv_nsec = local_ns % NS_PER_SEC };

  pw_loop_update_timer(pw_thread_loop_get_loop(s.loop), s.timer, &ts, NULL, true);
}

static int64_t
index_to_gm_locked(uint64_t index)
{
  return s.gm_base + (int64_t)((double)(int64_t)(index - s.index_base) * (double)NS_PER_SEC / SINK_RATE);
}

static void
on_timer(void *data, uint64_t expirations)
{
  struct timeline_snapshot tl;
  int64_t now = mono_ns();
  int64_t due_gm;
  int64_t now_gm;

  timeline_get(&tl);

  pthread_mutex_lock(&s.pos_lck);

  // A new timeline (first lock, or another grandmaster): carry on from the
  // next sample, now on that timeline. Nothing downstream holds an anchor on
  // the old one that survives this - the caller re-anchors on a generation
  // change (see stream.c).
  if (tl.generation != s.generation)
    {
      s.generation = tl.generation;
      s.index_base = s.next_index;
      s.gm_base = timeline_local_to_gm(&tl, now);
      log_info(MOD, "Timeline generation %u (%s %016llx) starts at sample %llu",
               tl.generation, tl.state == TIMELINE_FOLLOWING ? "grandmaster" : "own clock",
               (unsigned long long)tl.gm_id, (unsigned long long)s.next_index);
    }

  // The previous cycle has not come back. Wait a little, then give it up so
  // one stall in the graph cannot stop the schedule.
  if (s.triggered_ns && !atomic_load(&s.cycle_done))
    {
      if (now - s.triggered_ns < CYCLE_STALL_NS)
        {
          pthread_mutex_unlock(&s.pos_lck);
          schedule(now + 1000000);
          return;
        }
      log_warn(MOD, "PipeWire cycle did not complete within %lld ms, skipping it", CYCLE_STALL_NS / 1000000);
    }

  due_gm = index_to_gm_locked(s.next_index);
  now_gm = timeline_local_to_gm(&tl, now);

  if (now_gm >= due_gm)
    {
      // Way behind (suspend, a large timeline step): don't try to catch up
      // by bursting cycles, rejoin the schedule from here
      if (now_gm - due_gm > 200 * 1000000LL)
        {
          uint64_t late = (uint64_t)((double)(now_gm - due_gm) * SINK_RATE / NS_PER_SEC);
          log_warn(MOD, "Schedule %lld ms behind, skipping %llu samples",
                   (long long)((now_gm - due_gm) / 1000000), (unsigned long long)late);
          s.next_index += late;
          due_gm = index_to_gm_locked(s.next_index);
        }

      atomic_store(&s.cycle_index, s.next_index);
      atomic_store(&s.cycle_done, false);
      s.triggered_ns = now;
      s.next_index += s.cfg.quantum;
      pw_stream_trigger_process(s.stream);
      due_gm = index_to_gm_locked(s.next_index);
    }

  pthread_mutex_unlock(&s.pos_lck);

  schedule(timeline_gm_to_local(&tl, due_gm));
}

static void
min_volume_assert(void)
{
  uint8_t buffer[512];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  struct spa_pod_frame f, f2;
  const struct spa_pod *param;

  // The slider controls the speakers (see main.c), so PipeWire must not also
  // attenuate the samples. channelmix.min-volume is the floor the mixer
  // clamps its gain to: at 1.0 no slider position attenuates, while the
  // slider itself still moves and reports where it is.
  spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
  spa_pod_builder_prop(&b, SPA_PROP_params, 0);
  spa_pod_builder_push_struct(&b, &f2);
  spa_pod_builder_string(&b, "channelmix.min-volume");
  spa_pod_builder_float(&b, 1.0f);
  spa_pod_builder_pop(&b, &f2);
  param = spa_pod_builder_pop(&b, &f);

  pw_stream_set_param(s.stream, SPA_PARAM_Props, param);
}

static void
latency_push(void)
{
  uint8_t buffer[256];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod *params[1];
  struct spa_latency_info info = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);

  // How long after our input ports a sample is heard, which clients add to
  // their own delay for A/V sync. The adapter in front of a stream ignores
  // ProcessLatency; a port Latency param is what reaches clients, as
  // PipeWire's own network sinks report it.
  info.min_ns = info.max_ns = (uint64_t)s.cfg.latency_ns;
  params[0] = spa_latency_build(&b, SPA_PARAM_Latency, &info);
  if (pw_stream_update_params(s.stream, params, 1) < 0)
    log_warn(MOD, "Could not publish the latency");
}

static void
on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
  log_info(MOD, "Stream %s -> %s%s%s", pw_stream_state_as_string(old), pw_stream_state_as_string(state),
           error ? ": " : "", error ? error : "");

  if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
    {
      // Re-pushed after every negotiation: the adapter recomputes latency
      // during format negotiation
      latency_push();
      min_volume_assert();
    }

  // A driver's cycles only run once it is streaming; a trigger before that
  // never completes
  if (state == PW_STREAM_STATE_STREAMING && !s.connected)
    {
      s.connected = true;
      schedule(mono_ns() + 1000000);
    }
}

static void
on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
  const struct spa_pod_prop *prop;
  struct sink_volume v;
  bool changed = false;

  if (id != SPA_PARAM_Props || !param || !spa_pod_is_object(param))
    return;

  pthread_mutex_lock(&s.vol_lck);
  v = s.vol;
  SPA_POD_OBJECT_FOREACH((const struct spa_pod_object *)param, prop)
    {
      if (prop->key == SPA_PROP_channelVolumes)
        {
          float vols[SPA_AUDIO_MAX_CHANNELS];
          uint32_t n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, vols, SPA_AUDIO_MAX_CHANNELS);
          float max = 0;
          uint32_t i;

          if (n == 0)
            continue;
          for (i = 0; i < n; i++)
            max = SPA_MAX(max, vols[i]);
          v.volume = cbrtf(max); // linear gain to slider position
          changed = true;
        }
      else if (prop->key == SPA_PROP_mute)
        {
          bool mute;
          if (spa_pod_get_bool(&prop->value, &mute) == 0)
            {
              v.mute = mute;
              changed = true;
            }
        }
    }
  if (changed && (fabsf(v.volume - s.vol.volume) > 0.0005f || v.mute != s.vol.mute || !s.vol_pending))
    {
      s.vol = v;
      s.vol_pending = true;
    }
  pthread_mutex_unlock(&s.vol_lck);

  if (changed)
    notify();
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_state_changed,
  .param_changed = on_param_changed,
  .process = on_process,
};

/* ------------------------------- Main thread ------------------------------ */

int
sink_start(const struct sink_config *cfg, sink_audio_cb audio_cb, sink_volume_cb volume_cb, void *arg)
{
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod *params[1];
  struct pw_properties *props;
  char quantum[32];
  char latency[32];
  int ret;

  s.cfg = *cfg;
  s.audio_cb = audio_cb;
  s.volume_cb = volume_cb;
  s.cb_arg = arg;
  s.vol.volume = -1; // unknown until PipeWire reports it
  s.generation = UINT32_MAX; // the first timer tick bases the schedule on the timeline

  s.notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (s.notify_fd < 0)
    return -1;

  pw_init(NULL, NULL);

  s.loop = pw_thread_loop_new("tuxplay-pw", NULL);
  if (!s.loop)
    return -1;

  snprintf(quantum, sizeof(quantum), "%u", cfg->quantum);
  snprintf(latency, sizeof(latency), "%u/%u", cfg->quantum, SINK_RATE);

  props = pw_properties_new(
    PW_KEY_MEDIA_TYPE, "Audio",
    PW_KEY_MEDIA_CLASS, "Audio/Sink",
    PW_KEY_NODE_NAME, cfg->node_name,
    PW_KEY_NODE_DESCRIPTION, cfg->description,
    PW_KEY_NODE_NICK, cfg->description,
    PW_KEY_NODE_LATENCY, latency,
    "node.force-quantum", quantum,
    "node.force-rate", "48000",
    "node.always-process", "true",
    "node.pause-on-idle", "false",
    "node.suspend-on-idle", "false",
    "session.suspend-timeout-seconds", "0",
    // Win driver election in any graph this sink is part of: the speakers'
    // clock must be the one that paces it
    "priority.driver", "20000",
    "priority.session", "1500",
    PW_KEY_DEVICE_ICON_NAME, "audio-speakers",
    NULL);

  s.stream = pw_stream_new_simple(pw_thread_loop_get_loop(s.loop), cfg->node_name, props, &stream_events, NULL);
  if (!s.stream)
    {
      log_err(MOD, "Could not create the PipeWire stream");
      return -1;
    }

  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
    &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S32, .channels = SINK_CHANNELS, .rate = SINK_RATE,
                             .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }));

  s.timer = pw_loop_add_timer(pw_thread_loop_get_loop(s.loop), on_timer, NULL);

  ret = pw_stream_connect(s.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
                          params, 1);
  if (ret < 0)
    {
      log_err(MOD, "Could not connect the PipeWire stream: %s", spa_strerror(ret));
      return -1;
    }

  if (pw_thread_loop_start(s.loop) < 0)
    return -1;

  log_info(MOD, "Sink '%s' up: %u Hz, %u frames per cycle (%.1f ms), reported latency %.0f ms",
           cfg->node_name, SINK_RATE, cfg->quantum, cfg->quantum * 1000.0 / SINK_RATE, cfg->latency_ns / 1e6);
  return 0;
}

void
sink_stop(void)
{
  if (!s.loop)
    return;
  pw_thread_loop_stop(s.loop);
  if (s.stream)
    pw_stream_destroy(s.stream);
  pw_thread_loop_destroy(s.loop);
  s.loop = NULL;
  pw_deinit();
}

int
sink_notify_fd(void)
{
  return s.notify_fd;
}

void
sink_drain(void)
{
  uint64_t counter;
  uint64_t w;
  uint64_t epoch;
  struct sink_volume v;
  bool vol_pending;
  ssize_t ret;

  ret = read(s.notify_fd, &counter, sizeof(counter));
  (void)ret;

  w = atomic_load_explicit(&s.ring_write, memory_order_acquire);
  epoch = atomic_load(&s.ring_epoch);
  if (s.ring_read < epoch && epoch <= w)
    {
      if (s.ring_read)
        log_warn(MOD, "%llu samples missing from the sink (skipped cycles)", (unsigned long long)(epoch - s.ring_read));
      s.ring_read = epoch;
    }
  if (w - s.ring_read > RING_FRAMES)
    {
      log_warn(MOD, "Main thread fell %llu frames behind the sink, dropping them",
               (unsigned long long)(w - s.ring_read - RING_FRAMES));
      s.ring_read = w - RING_FRAMES;
    }

  while (s.ring_read < w)
    {
      uint64_t slot = s.ring_read & RING_MASK;
      uint32_t n = (uint32_t)SPA_MIN(w - s.ring_read, (uint64_t)(RING_FRAMES - slot));

      s.audio_cb(s.ring_read, &s.ring[slot * 2], n, s.cb_arg);
      s.ring_read += n;
    }

  pthread_mutex_lock(&s.vol_lck);
  v = s.vol;
  vol_pending = s.vol_pending;
  s.vol_pending = false;
  pthread_mutex_unlock(&s.vol_lck);

  if (vol_pending && s.volume_cb && v.volume >= 0)
    s.volume_cb(&v, s.cb_arg);
}

void
sink_position(uint64_t *next_index, int64_t *next_gm_ns, uint32_t *generation)
{
  pthread_mutex_lock(&s.pos_lck);
  if (next_index)
    *next_index = s.next_index;
  if (next_gm_ns)
    *next_gm_ns = index_to_gm_locked(s.next_index);
  if (generation)
    *generation = s.generation;
  pthread_mutex_unlock(&s.pos_lck);
}

int64_t
sink_index_to_gm(uint64_t index)
{
  int64_t gm;

  pthread_mutex_lock(&s.pos_lck);
  gm = index_to_gm_locked(index);
  pthread_mutex_unlock(&s.pos_lck);
  return gm;
}

void
sink_latency_set(int64_t latency_ns)
{
  pw_thread_loop_lock(s.loop);
  s.cfg.latency_ns = latency_ns;
  latency_push();
  pw_thread_loop_unlock(s.loop);
}

void
sink_volume_set(const struct sink_volume *vol)
{
  uint8_t buffer[512];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  float vols[SINK_CHANNELS];
  float gain = vol->volume * vol->volume * vol->volume;
  const struct spa_pod *param;
  int i;

  for (i = 0; i < SINK_CHANNELS; i++)
    vols[i] = gain;

  param = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
                                     SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, SINK_CHANNELS, vols),
                                     SPA_PROP_mute, SPA_POD_Bool(vol->mute));

  pw_thread_loop_lock(s.loop);
  pthread_mutex_lock(&s.vol_lck);
  s.vol = *vol; // so the echo of our own change is not reported as a new one
  pthread_mutex_unlock(&s.vol_lck);
  pw_stream_set_param(s.stream, SPA_PARAM_Props, param);
  pw_thread_loop_unlock(s.loop);
}
