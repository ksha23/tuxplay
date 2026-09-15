# Debugging this

## The failure mode to expect

Every sender-side diagnostic can read perfectly healthy while the speakers are
silent. This is not hypothetical - it happened, and the checks below were all
green at the time:

```
session established, SETPEERS sent to both speakers
SETRATEANCHORTIME accepted after 0 retries
"First buffered frame" logged
fifo draining at exactly 176400 B/s
216 packets in 4 s on the wire to both speakers
real audio in the sink (monitor peak 15748)
```

The cause was a missing `RECORD`, and the only component that knew was the one
with no logs. **A sender that looks healthy is not evidence that sound is coming
out.** Ask someone to listen.

## Useful checks

```sh
# is anything connected, and to what
ss -tn state established | grep -c :7000      # one control connection per receiver
curl -s localhost:3689/api/outputs | python3 -m json.tool

# is the pipeline carrying audio, without opening a capture stream
curl -s localhost:3689/api/player/silence     # {"silent_msec": N}, -1 = never

# where the latency is going, printed every 30 s
journalctl -u owntone-mini | grep '\[stats\] latency'

# park / unpark and handover
journalctl -u owntone-mini | grep -E 'Parked|Unparked|Not resurrecting'
journalctl --user -u tuxplay-share -f

# what the sink advertises (on the PORTS, not the node)
pw-dump | python3 -c '...'   # see pipewire-notes.md
```

## Measuring latency

The anchor lead is exact by construction: the receivers are PTP-locked and render
at the transmitted network time. So the only estimated term is upstream of the
fifo. owntone prints the breakdown itself:

```
[stats] latency: fifo 0 ms + input buffer 43 ms + anchor lead 400 ms = 443 ms
```

Add roughly one PipeWire quantum (~21 ms at 1024 frames / 48 kHz) for the graph.

## Lessons about measurement itself

Every wrong conclusion in this project came from an instrument that could not see
the answer, not from bad reasoning about the data:

- `lsof` without `sudo` cannot see root-owned processes, and returns nothing
  rather than an error.
- A filter on IPv4 addresses misses devices reachable over IPv6.
- `pw-cli enum-params <node> Latency` reports the node's output latency; the
  value that matters is on the input ports.
- `pactl`'s `Latency:` field is the measured latency of an idle sink, not the
  declared one.
- Grepping a 6 GB dyld shared cache finds strings from unrelated frameworks that
  look exactly like the ones you want.

When a measurement returns "nothing", check that it *could* have returned
something.

## Shell traps that bit this project twice

Under `set -e`, a bare `[ test ] && command` as the last statement of a function -
or as a statement in a loop - **exits the script** when the test is false. Both
helpers crash-looped on exactly this, in different places, weeks apart in
appearance but the same shape:

```sh
# kills the service the first time $ids is empty
remember() { ids=$(selected_ids); [ -n "$ids" ] && printf ... > "$CONF"; }

# fine: inside `if`, or guarded
if [ -n "$ids" ]; then printf ... ; fi
[ -f "$CONF" ] && . "$CONF" || true
```

Symptom: a service that appears to work intermittently, with a changing MainPID.
Check `systemctl --user show <unit> -p MainPID --value` twice.

## Timing races worth knowing

- owntone reconnects a failed device after `PLAYER_SPEAKER_RESURRECT_TIME` (5 s),
  and only stands down if the device is **no longer selected** by then. Anything
  detecting a takeover has to conclude and deselect inside that window. A poll
  loop doing several `curl` calls per iteration is slower than its nominal
  interval - four "one second" polls landed at 5-6 s and lost the race every time.
- During a real takeover, owntone reports *nothing selected* for a second or two.
  Do not use "outputs still selected" to distinguish a takeover from a manual
  deselect; it suppresses the detection it is meant to protect.
- A newly selected output arrives mid-negotiation. Parking it before it has ever
  rendered sends `rate: 0` as its first anchor, which the receiver rejects
  repeatedly until the device is dropped.
