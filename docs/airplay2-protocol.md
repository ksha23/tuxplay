# AirPlay 2 notes

Findings from making a HomePod stereo pair work from Linux. Everything here was
measured against two HomePods unless marked otherwise, so treat it as "true of
these devices" rather than "true of the protocol".

## The two transports are not variations of each other

| | realtime | buffered |
|---|---|---|
| stream type | 96 (`0x60`) | 103 (`0x67`) |
| audio on the wire | UDP RTP | TCP |
| render scheduling | `latencyMin` / `latencyMax` in `SETUP(stream)` | `SETRATEANCHORTIME` |
| can it be paused without teardown? | no | yes, `rate: 0` |
| what macOS uses for system audio | this | - |
| what this project uses | **this** | was, until the indicator was understood |

Buffered was chosen because `SETRATEANCHORTIME` gives exact control over *when*
a frame renders, where realtime negotiates a latency and leaves the rest to the
receiver. That reasoning is sound, and it was still the wrong trade.

`rate: 0` is the only thing that darkens a HomePod on buffered, and it tears
down the render schedule - so every resume re-anchored, which destroyed 45-67 ms
of audio (a receiver discards, rather than queues, audio arriving before a
rate=1 anchor - tested) and left ~400 ms of video with no sound. This project
now uses **realtime**, like macOS, with no parking at all: nothing re-anchors,
nothing is lost, and the lights are handled by MediaRemote playback state
instead. See the indicator section below.

Realtime does drive a stereo pair perfectly well - macOS does it, with PTP and
`SETPEERS`, and so do we. The claim that buffered was required for pairs was
never true.

## `start_buffer_ms` is the latency

`payload_make_send_anchor()` computes the anchor as *now + `start_buffer_ms`*, so
that setting is literally how far ahead of render time audio is handed over. It is
the delay you hear. Everything else in the path (the graph quantum, the fifo, the
input buffer) adds tens of milliseconds; this adds hundreds.

## `AIRPLAY_AUDIO_LATENCY_MS = 250` is a guess, not a protocol minimum

It came from shairport-sync, whose comment says so outright: *"The value of 11025
(0.25 seconds) is a guess based on the Audio-Latency parameter returned by an
AE."* An AirPort Express, in other words, carried forward ever since.

A HomePod's own `/info` advertises `arrivalToRenderLatencyMs=84`. And on the
buffered transport the value is never sent on the wire at all -
`latencyMin`/`latencyMax` appear only in the realtime `SETUP(stream)` payload.
All it does on the buffered path is set the floor under `start_buffer_ms`.

## `RECORD` is required, and its absence is silent

Skipping `RECORD` on the buffered transport looks safe: it is an empty request
and `AIRPLAY_STATE_RECORD` is only a startup-range state internally. That
reasoning is about the sender, not the receiver. `RECORD` is what tells the
receiver to start rendering.

Without it: the session establishes, `SETPEERS` goes to both speakers,
`SETRATEANCHORTIME` is accepted, "first buffered frame" is logged, the fifo
drains at exactly realtime rate, packets flow on the wire - and the speakers play
nothing at all. Every sender-side diagnostic stays green. Only the speakers know,
and they report it by staying silent.

## `rate: 0` is a real pause; `FLUSHBUFFERED` is not

`SETRATEANCHORTIME` carries a `rate` field. owntone hardcoded it to 1.

- `rate: 0` on **buffered**: RTSP 200, in-band status 0. The receiver stops
  rendering, the session stays fully alive, and resuming re-anchors at exactly
  the rtptime the pause froze on, so there is no gap. Both halves are needed -
  send the anchor but keep feeding, and the receiver stops draining while you
  keep writing, so its buffer fills and the TCP connection backs up.
- `rate: 0` on **realtime**: RTSP 200, in-band status **-12782**. Refused. Rate
  control does not exist on that transport.
- `FLUSHBUFFERED`: the receiver hangs up about ten seconds later. Dropping the
  keep-alive interval from 25 s to 2 s does not prevent it. It is not a way to
  hold an idle session.

## The white indicator follows MediaRemote playback state

A held-open session that is streaming - even digital silence - leaves the
speakers lit. `rate: 0` turns them dark while the session stays up - but that
is a property of the buffered path, not the answer, and chasing it cost this
project a lot. **What actually drives the indicator is the MediaRemote
now-playing state.**

Measured, from macOS's own unified log driving these same two speakers:

- At a pause, `AirPlayXPCHelper` sends **nothing at all** to the receivers. The
  only traffic is the speakers pushing `updateInfo` events *to* the Mac, and
  `RTAE ... Time announce` shows its audio engine still running afterwards.
- What macOS does run is a MediaRemote NowPlaying controller, reporting
  `playbackState Paused`.

So the HomePods darken **themselves**, in response to the reported playback
state. There is no sender-side "park" in AirPlay; parking was this project's
invention, and it is the reason resumes used to lose audio.

Opening the MediaRemote now-playing channel to HomePods reproduces macOS's
behaviour exactly: lights out on pause, lit on play, nothing re-anchored, no
audio lost. These speakers advertise feature bit 50 (high word `0x3C354BD0`,
bit 18 set); the channel was refused by a devtype gate that assumed
now-playing was "only meaningful to Apple TV class receivers".

That gate is also why this document used to claim MediaRemote had no effect:
with `session->mrp` NULL, nothing on that path was ever sent. The entry below
recorded "no effect" when it should have recorded "never tried".

Ruled out by measurement, with the correction above in mind:

| candidate | result |
|---|---|
| dither vs digital silence | no effect on either transport |
| suppressing transmission entirely for 79 s (realtime) | no effect - lights stayed on |
| `audioMode: ambient` instead of `default` | no effect |
| `isMedia: false` | does not darken - **silences the speaker entirely** |
| MediaRemote `PlaybackState` | **this is the mechanism** - previously recorded as "no effect" only because it was never sent |

On realtime the indicator does not follow audio level or packet flow: dither,
true digital zero (verified, peak 0 of 32767) and fully suppressed
transmission all leave it lit. `FLUSH` darkens it, but the receiver then hangs
up within about ten seconds, so it is not usable as a pause.

## Why one speaker of a pair used to light alone

Resolved, and it was never intermittent.

`payload_make_send_anchor()` paid `AIRPLAY_BUFFERED_JOIN_SLACK_MS` whenever the
master session already had a rtptime->networkTime mapping. On a group unpark
every member is gated waiting for its own anchor, so the write head is frozen
and the slack is not merely unnecessary: it anchored the second member a full
slack later than the first. Measured as exactly 9856 samples - 28 frames of 352
spf, 205 ms - on 5 of 5 resume cycles and on cold starts.

So one speaker played alone for 205 ms on *every* resume. Whether anyone
noticed depended on whether they were listening during those 205 ms, which is
what made it look intermittent. The slack is now paid only when a sibling is
genuinely streaming and moving the head.

Theories that were wrong, each disproven by its own instrumentation after being
implemented: SETPEERS asymmetry (both members always get their sibling), join
anchor overshoot (guard added, fires zero times), members anchoring at
different times (barrier added, fires correctly), and a per-session groupUUID
(shared UUID implemented). The fixes are all still in place because each is
correct in itself; none of them was the cause.

## The event channel is how a receiver asks to be let go

A receiver keeps a reverse channel to the sender. A `pause` event on it means
*this receiver wants to stop taking audio* - which is what arrives when another
device takes the speakers.

Handle it by deselecting **that output only**, not by pausing everything. And do
not treat `EAGAIN` on that socket as fatal: it is a normal wakeup with nothing
readable yet, and tearing the channel down on it means never hearing the request.

## Stereo pairs

Worth knowing before assuming this part is hard: **sending stereo to a paired
HomePod already works in stock owntone** by selecting both halves as separate
outputs - the speakers apply their own channel assignment. This is not widely
known; owntone#1413 is about showing the pair as *one* output, not about stereo
working at all, and it has been open since 2022.

The grouping work below - collapsing a pair into a single output with group-aware
`SETPEERS` - comes from owntone-mini, whose author has offered the approach
upstream on that issue.

- Members share a `gid` and advertise `gcgl` / `igl` leader hints over mDNS.
- **The hints are not advertised continuously.** The same pair was observed
  advertising `gcgl=0, igl=0` and `gcgl=1, igl=1` at different moments. Treat a
  missing hint as "no new information", never as "no leader" - otherwise the pair
  disappears from the output list, which is self-reinforcing: no leader means not
  selectable, means no session, means still no hint.
- Each member must be told about the other via `SETPEERS` or they run on
  uncorrelated clocks. This is what no AirPlay 1 sink can do, and the whole
  reason a pair needs AirPlay 2.
- Do not start a group before every member is discovered: the first one's session
  gets built with an empty `SETPEERS`.

## The sender follows the receivers' clock

A pair elects its own PTP grandmaster (one member's clock; the other member
follows it, `stepsRemoved 1`). A Mac announces an ordinary clock (priority1 250,
class 248, priority2 239, time source 0xA0), loses to the HomePods' 248, and
follows. Every type 215 sync packet it sends, to both members, carries **the
grandmaster's clock identity** and a time on the grandmaster's timeline.

owntone's PTP library instead always made itself grandmaster, announcing a GPS
clock. Each member then tracked the sender over its own wireless link, and the
two tracking errors are independent: every so often a pair came up a
millisecond or two apart, which is enough to lose the centre image (two
speakers either side instead of a sound between them), until a reconnect. The
mappings sent were identical and the sender's timestamps were within 0.1 ms of
the wire, so nothing on the sending side showed it.

Following the pair's grandmaster instead means any error in the sender's own
estimate moves both members together. Details that matter when doing it:

- **Correction fields are large.** A member relaying its partner's clock stamps
  its `Follow_Up` and `Delay_Resp` with its own clock and puts the difference,
  tens to hundreds of ms, in `correctionField`. It is added to both the origin
  and the receive time; only that convention gives a plausible path delay.
- **A HomePod moves to a new PTP port** a moment after its first announce for a
  session. Match messages by clock identity, not port identity.
- **The first `Delay_Resp` can be on the member's own clock**, hours away from
  the rest. Discard samples implausibly far from the others before taking the
  fastest exchange.
- Locking takes about 0.4 s from the first announce, which lands inside session
  setup, so holding a stream's start until then costs 0 to 150 ms.
- `TUXPLAY_PTP_ROLE=master` restores always being grandmaster.

## Receivers check the sender's User-Agent

HomePod software from AirTunes 980 answers `403 Forbidden` to the first
`GET /info` unless the `User-Agent` is Apple's: `AirPlay/<version>` above a
minimum version (100 is refused, 300 accepted) or `iTunes/`. Anything else,
including `owntone-mini/1.3.0`, gets nothing. owntone sends what macOS 26.5
sends, `AirPlay/950.7.1`; `TUXPLAY_AIRPLAY_USER_AGENT` overrides it.

## Things that look like useful signals and are not

- **`GET /info` `statusFlags`.** Two identical held states read `0x2BAC04` and
  `0x3BA404`. Not a usable proxy for session or indicator state.
- **`/playback-info` and `/server-info`.** Return 455 on these devices.
