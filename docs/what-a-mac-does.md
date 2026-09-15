# What a Mac actually does

Measured, because assuming was wrong three times in a row. The failures are
recorded here too, since the way these measurements go wrong is the interesting
part.

## How to measure it at all

Two traps, both of which produced confident and completely wrong conclusions:

1. **The sender is root.** `/usr/libexec/AirPlayXPCHelper` owns the AirPlay
   sockets, and `lsof` without `sudo` cannot see another user's processes. Every
   check run as a normal user returns nothing, which reads exactly like "macOS
   holds no connection". Use `sudo lsof -nP -i -a -c AirPlayXPCHelper -c coreaudiod`.
2. **HomePods are reachable over IPv6.** A filter on `192.168.x.y` misses the
   traffic entirely. Filter by process, or by port, not by address.

## Idle behaviour depends on the keep-alive

These are different states, not a contradiction:

**Preroll's keep-alive off.** No TCP, no UDP, nothing held. The session is built
on demand when playback starts.

**Preroll's keep-alive on.** ESTABLISHED TCP to *both* speakers on port 7000, and
full-rate audio over UDP, continuously, forever. Measured over 115 s covering
idle, playing and idle again:

```
UDP 32931 pkts / TCP 776
audio      ~285-300 pkt/s, unbroken across idle and playing
idle       mean packet 1410 bytes, constant
playing    mean packet 1209-1321 bytes, variable
RTSP :7000 4-8 pkt/s throughout, never torn down
PTP        319/320 continuously
```

Idle packets being *larger* than music is the giveaway: the keep-alive feeds
sub-LSB dither, which is incompressible, so ALAC emits full frames. Music
actually compresses.

So a Mac in that state is doing exactly what this project does - holding
sessions and streaming continuously - and its speakers still go dark when
nothing is playing.

## What the keep-alive actually is

From Preroll's own source: feed continuous, inaudible, **non-zero** dither to the
AirPlay output device. Apple's HAL driver reads `enableSilenceDetection` and
`enableNonZeroPCMSampleDetection`; digital silence lets it idle the stream, and
idling costs a full re-establish on resume.

It is a CoreAudio-side mechanism, and there is no Linux equivalent to port: our
sink already never idles. The *effect* we wanted from it - dark speakers while
holding - comes from `rate: 0`, not from the dither.

## macOS uses the realtime transport

Audio is UDP RTP (type 96), not the buffered TCP transport this project uses.
That matters because `SETRATEANCHORTIME` does not exist there, so whatever keeps
a Mac's idle speakers dark, it is not the mechanism we use. See
[airplay2-protocol.md](airplay2-protocol.md) for what the receiver-side code
says about realtime and the indicator.

## What a Mac does at a pause: nothing

Captured from macOS's own unified log, driving this same HomePod pair:

    log stream --info --debug --style compact \
      --predicate 'subsystem CONTAINS "airplay" OR process == "AirPlayXPCHelper"'

At the moment playback pauses, `AirPlayXPCHelper` sends **nothing** to the
speakers. No teardown, no flush, no rate change, no command of any kind. The
only traffic is the receivers pushing `updateInfo` events *to* the Mac, and

    RTAE ['HLA'-0xDB6E] Time announce: rtpTime 2899679859

shows the realtime audio engine still running afterwards.

What it does run is a MediaRemote NowPlaying controller:

    [MRNowPlayingPlayerClientRequests] ... UpdatingCache: playbackState Paused

So the speakers darken **themselves**, in response to the reported playback
state. Every sender-side mechanism this project hunted for - parking, dither,
suppressing transmission, audioMode - was looking for something that does not
exist. See [airplay2-protocol.md](airplay2-protocol.md) for the measurements.

Also visible in the same capture: `AudioEngineRealTime using audio latency
400 ms`, which is the figure a Mac actually runs at on this network.

This is by far the highest-value instrument available for this project, and it
was not used until very late. macOS narrates its own AirPlay internals at info
and debug level; both are excluded from `log show` by default, which is why
casual looks come back empty.

## A Mac follows the pair's clock

Captured on the Mac with `tcpdump` on ports 319/320 while it played to a pair:
the Mac announces priority1 250, loses to the HomePods (248), and within 0.3 s
is sending `Delay_Req` to one member, about eight a second, as an ordinary
follower. It never exchanges PTP with the other member at all. Its sync packets
to both members name the pair's grandmaster (the other member's clock), with
times on that clock. See `airplay2-protocol.md` for why this matters for the
stereo image.

## Session setup speed

A Mac establishes in roughly 250-300 ms; this project takes about 1.4 s. The
remaining difference is genuine receiver round trips - `SETUP (session)`,
`SETPEERS`, `SETUP (stream)` - inflated by the pair arbitrating between its own
two members. Our side is already concurrent: `GET /info` reaches both speakers
within 2 ms of the select.

The likely source of the gap is that macOS keeps its RTSP **control connection**
open and builds a fresh session over it, skipping TCP connect and the pair-verify
crypto. Closing that would mean keeping owntone's `evrtsp` connection alive
across sessions, which is real surgery in the session state machine.
