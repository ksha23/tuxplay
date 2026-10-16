# tuxplayd (v2)

Why the owntone-mini based v1 was replaced, what macOS 27 does on the wire, and
what bringing up the replacement turned up. Measured against two HomePod stereo
pairs (HomePod OS 27.0, AirPlay 980.77.2) and a Mac on macOS 27.0.1, October 2026.

## What was wrong with v1

From captures of v1 driving the Desk pair:

- **Resends inside the audio stream.** Realtime retransmissions went out on the
  audio port interleaved with live packets, where iOS answers on the control
  channel.
- **Pair members anchored separately.** In buffered mode the second speaker
  started about 400 ms after the first and had to be re-anchored.
- **Drift.** The player ticked on `CLOCK_MONOTONIC` while the speakers' clock was
  6 to 39 ppm off, about 100 ms an hour in buffered mode.
- **Probes.** A connect-and-hang-up to port 7000 on every mDNS update, every 5 to
  15 s per speaker.
- **PTP by luck.** It followed whichever member's Sync arrived first, the
  grandmaster or the member relaying it.
- **An input buffer that wandered** between 20 and 70 ms per run, trimmed with an
  audible skip above 150 ms.

## What macOS 27 does

Read from the Mac's own unified log and from captures on the Mac (`dumpcap` on
`en0`, `en7`, `nan0`, `llw1`). The Mac's control channel is encrypted after
pairing, so its content is known only from the log, and its sizes and timing from
the wire.

- **Realtime APAT.** System audio goes out as `lowLatencyAudioStream`: realtime
  UDP, 10 ms packets of 480 samples, RTCP on the same port. The receiver lists it
  under `supportedFormats` with bits 25 (AAC-ELD 48 kHz stereo) and 42.
- **Over Wi-Fi Aware when Wi-Fi is on**, with or without Ethernet:
  `usingLLW=yes (actualTransportType=NAN)`, "Using 150ms latency for RA NAN".
- **Over the LAN when Wi-Fi is off:** the same mode, `actualTransportType=Infra`,
  "Using 500ms latency for RA INFRA", minimum 250 ms. Its adaptive latency
  manager settled at about 285 to 295 ms on Ethernet. So the low latency is
  Wi-Fi Aware's, not the stream type's.
- **Never pauses.** While the output is selected it streams continuously, 100
  packets a second of silence when nothing plays, so the HomePods stay lit. A
  pause sends now-playing updates and no flush.
- **First request:** `POST /pair-verify` with `X-Apple-HKP: 6` (it is a HomeKit
  controller of the Home), `X-Apple-CombineGetInfo: 1` so the device info comes
  back with pairing, `X-Apple-PD: 1`, `X-Apple-Client-ID`, CSeq from 0,
  `User-Agent: AirPlay/980.77.5`, and none of the old DACP headers.
- **Keep-alive** every 2.0 s, answered with stats (`keepAliveSendStatsAsBody`).
- **PTP on the LAN:** follows the grandmaster and sends Delay_Req at 8 per second
  to *both* members of a pair, controlField 0, no Announce, no Signaling. Over
  Wi-Fi Aware it also relays time to the second member, since the Mac is the hub.
- **DSCP 0** on everything.

## How tuxplayd compares

| | macOS 27 | tuxplayd |
|---|---|---|
| Pairing | HomeKit pair-verify (`HKP 6`) | HomeKit transient (`HKP 4`), the method for senders outside the Home |
| Headers | client headers on `/pair-verify` only, then just CSeq and User-Agent | the same, on pair-setup |
| Keep-alive, CSeq | as above | the same |
| PTP | follower, Delay_Req to every member, no Signaling | the same. The HomePods then treat both senders alike, announcing once a second (to a sender that skips a member's Delay_Req, that member announces four times a second) |
| Stream | realtime APAT, adaptive latency | buffered ALAC over TCP, fixed lead |
| Latency on Ethernet | about 290 ms | 250 ms lead, the whole delay (the HomePod states `arrivalToRenderLatencyMs` 4) |
| Idle | streams silence, stays lit | the same (`-P` sets a pause instead), at 0.06 Mbit/s |
| Volume on connect | adopts the speaker's | the same |
| A tap on the speaker | pauses its player | pauses the desktop's player (MPRIS) |
| Another sender takes over | the Mac switches its output away | stands down, takes the speakers back when something new plays |

The stream type is the one real difference. Realtime APAT is undocumented and
encrypted on the wire. On Ethernet it gains nothing over the buffered stream. Its
advantage is adapting to a busy network, which matters for Wi-Fi, where Linux
has no Wi-Fi Aware data path yet (it is arriving in kernel 7.1 and hostap).

## Bugs found while bringing it up

- **PTP peers forgotten.** A HomePod sends PTP from a different IPv6 address than
  the one it gives in SETUP. libairptp pruned peers it had not heard from in 15 s,
  so the next peer change could leave none, drop the lock and anchor on our own
  clock, which the speakers reject. Peers now live until their session ends, and
  the player anchors only while following the speakers' clock.
- **Latency not reaching players.** See [pipewire-notes.md](pipewire-notes.md):
  `ProcessLatency` is ignored, a port `Latency` param is what clients read.
- **v1's helpers driving v2.** tuxplay-share kept selecting and deselecting
  outputs through the API, which tuxplayd answers like owntone did. They are gone.

## Network changes

Tested on north (wired RTL8125B and Wi-Fi MT7921K on the same LAN) by switching
interfaces with NetworkManager while a stream played, tuxplayd never restarted.

- **What it does.** tuxplayd watches the kernel's address and link events
  (netlink). A session whose local address or link goes away is failed at once
  and reconnected 200 ms later (1 s after a link loses its carrier, since its
  address and route go only after that). The PTP lock is kept for 10 s after the
  last peer goes, so reconnecting sessions do not wait to relock. A speaker
  withdrawn by mDNS on one interface stays present while another interface sees
  it, and after a network loss its last address is tried before mDNS finds it
  again. A new local address triggers an immediate retry.
- **Measured.** Losing the interface in use: sound back after about 1.6 s.
  Both interfaces down for 10 s, then Wi-Fi back: sound back 2 s after Wi-Fi
  reconnected (11 s before the address and retry changes). Adding an interface
  while playing: no interruption.
- **Found on the way.** With both cards on one subnet and the kernel's default
  ARP settings, each card answers ARP for the other's address. NetworkManager's
  duplicate-address check then reports a conflict with the machine's own Wi-Fi
  MAC and brings a reconnected Ethernet up without routes. `arp_ignore = 1` and
  `arp_announce = 2` fix it.

## Found by an independent comparison

- **Bitrate (fixed).** tuxplayd's ALAC frames were uncompressed, 2.31 Mbit/s per
  receiver even in silence, where the Mac's realtime AAC-ELD runs at 0.47. The
  HomePods are on Wi-Fi, so a held pair cost 4.6 Mbit/s of airtime around the
  clock. Frames are now compressed with libavcodec's ALAC encoder. Digital
  silence goes out as a 20-byte frame tuxplayd builds itself (no predictor, one
  zero, then a run of 351 zeros), because the encoder stores the low byte of a
  24-bit sample raw even when it is zero (740 bytes a frame). Measured to White
  Right: silence 0.063 Mbit/s, a noise-like test signal 0.96 Mbit/s. At startup
  both kinds of frame are decoded with libavcodec's ALAC decoder, set up the way
  the receivers are, and anything not bit-exact falls back to uncompressed
  frames. Lossless audio in a 24-bit stream keeps music above the Mac's lossy
  0.47; the HomePods offer no 16-bit ALAC at 48 kHz.
- **Volume on connect (fixed).** The Mac sends none and adopts the receiver's
  level. tuxplayd now reads `initialVolume` from a plain `GET /info` before
  pairing, takes the volume leader's as the output's volume, moves the desktop
  slider to it, and sends volume only when it changes here.
- **MediaRemote (fixed).** A tap on the HomePod sends `paus`, which tuxplayd took
  for a takeover. Speaker commands (`play`, `paus`, `nitm`, `pitm`) now go to the
  desktop's media player through `playerctl`, playback state follows the
  desktop going quiet or audible so the next tap asks for the other, and only
  the commands handled are advertised (play, pause, toggle, next, previous). A
  takeover is the receiver closing the connection. Now-playing text and artwork
  are not sent.
- **Headers (fixed).** Client name and id now go only with pairing, and bodyless
  requests carry no `Content-Length`, as the Mac sends them.
- **Smaller ones, left as they are.** One stable UUID for client and timing-peer
  id, delayed ACKs on the control connection, ECN, and RECORD and `/audioMode`,
  which the Mac skips. All harmless as measured.

## Not done

- Re-anchoring after a grandmaster change is written but has never been
  triggered in a test.
- On Wi-Fi a fixed lead may be too short. An adaptive lead like the Mac's would
  follow the network.
