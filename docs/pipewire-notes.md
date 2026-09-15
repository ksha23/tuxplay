# PipeWire notes

Things that cost a while to work out.

## A sink with no client does not run

`session.suspend-timeout-seconds = 0` stops WirePlumber *suspending an idle
node*. It does not *activate* a node nothing is linked to, so a sink with no
client sits in `SUSPENDED` and no bytes reach its far end. The property that
keeps it processing regardless is:

```
node.always-process = true
```

Without it the fifo goes idle whenever nothing is playing, and everything
downstream - the reader, the player, the AirPlay sessions - tears down with it.

## Declared latency has to be re-pushed after negotiation

Passing latency params to `pw_stream_connect()` is not enough: the adapter
recomputes port latency during format negotiation and zeroes them. Push again
when the stream reaches `PAUSED`/`STREAMING`.

## Latency lives on the ports, not the node

`pw-cli enum-params <node> Latency` shows zeros for a sink no matter what is
declared - that is the node's *output* latency, and a sink has no output. The
value clients read is on the **input ports**:

```
port playback_FL  ->  direction Input: minNs = 465000000
```

And `pactl list sinks` reporting `Latency: 0 usec` means the *measured* latency of
an idle sink, not the declared one. PipeWire's own RAOP sinks report the same.
Checking the wrong object here produced a completely wrong conclusion that stood
for hours.

## `module-raop-discover` rules cannot template the device name

`stream.props` in a match rule takes literal values; there is no interpolation of
`raop.name` or similar. So you cannot relabel the discovered sinks generically -
setting `node.description` gives every one of them the same name. Per-device
rules work but break whenever the devices change.

## PipeWire's RAOP is AirPlay 1

`module-raop-sink` contains no `SETPEERS`, no `SETRATEANCHORTIME` and no
`bufferStream` - only the classic `ANNOUNCE`/`RECORD` flow. One speaker each, no
stereo pairs. This is the reason this project exists rather than being a config
file.

## The default fd limit is too low

PipeWire's default `LimitNOFILE=1024` is not enough once a handful of AirPlay
devices are discovered. It dies with `can't DUP fd` and takes the session with
it. 65536 via a systemd drop-in.

## Restarting PipeWire disturbs clients

Destroying and recreating a sink can leave a playing stream corked and unable to
recover - Firefox is particularly prone, and needs a tab reload. Anything a user
will change while watching something must not require it. That is why the sync
correction moves the anchor (restarting only owntone, which is just the reader on
the far end of a fifo) instead of rewriting the advertised latency.

## Capturing a monitor shows the microphone indicator

Sampling a sink's monitor with `parec` opens a recording stream, and the desktop
shows its microphone-in-use indicator for as long as it is open. Polling once a
second makes it flash once a second. Measure the samples where they already are
instead.

## AppIndicator menus are not GTK menus

They are serialised over D-Bus and rendered by the shell, so only standard item
properties survive: label, checkbox, separator, submenu, icon. Custom widgets,
markup and sliders are dropped **silently** - no error, they simply do not
appear. Anything wanting real layout has to be a window.

## Cancelling a sink's volume so a device can do it instead

For a device with its own volume control - an ALSA card with a hardware mixer,
or an AirPlay receiver - the sink's slider should choose a number without also
multiplying it into the samples, or the attenuation happens twice.

**`channelmix.lock-volumes` is not that lever**, despite the name. It does not
mean "display the value but do not apply it"; it means "ignore volume changes".
An ALSA node can afford that because its slider lives on the device's Route
param rather than on the node, so the UI still moves. Set it on an ordinary
sink and the slider freezes solid - `pactl set-sink-volume` silently does
nothing, and anything reading the slider back to drive a device will keep
reading the stale value and send that instead. That failure is quiet and it
is loud: the slider reads 88%, it cannot be turned down, and 88 is what gets
pushed to the speakers.

The lever is `channelmix.min-volume`: the floor the mixer clamps its volume to.
Set it to 1.0 and no slider position can attenuate, while `channelVolumes` is
left alone so the slider still moves and still reports where it is. Set once,
never touched again:

| | |
|---|---|
| slider 100%, `min-volume = 1.0` | +0.0 dB |
| slider 20%, `min-volume = 1.0` | -0.0 dB |
| slider 60%, `min-volume = 1.0` | -0.0 dB |
| slider 10%, `min-volume = 1.0` | -0.0 dB |

```
pw-cli s <id> Props '{ params = [ "channelmix.min-volume", "1.0" ] }'
```

### Writing softVolumes works, and still is not the answer

`softVolumes` is the gain actually applied.
`channelVolumes` is what the slider holds and the desktop shows. Measured on a
pipe-tunnel sink writing to a FIFO, playing a fixed tone and taking the RMS of
the bytes:

| | |
|---|---|
| slider 100% | +0.0 dB |
| slider 50% | -18.1 dB |
| slider 50%, `softVolumes = [1.0, 1.0]` | -0.0 dB |

So writing unity into `softVolumes` cancels the gain while the slider stays
free to move. The catch is that it does not stay:

| | |
|---|---|
| slider 60%, unity asserted | -0.0 dB |
| moved to 30%, not re-asserted | **-31.4 dB** |
| 30%, unity re-asserted | -0.0 dB |

With a loop re-asserting unity every 200 ms, the cancellation holds across
moves - which is what the daemon has to do:

| | |
|---|---|
| slider 20%, nothing re-asserting | -41.9 dB |
| slider 20%, re-asserting | -0.0 dB |
| moved to 50% while re-asserting | -0.0 dB |
| moved to 10% while re-asserting | -0.0 dB |

PipeWire recomputes `softVolumes` from `channelVolumes` on every change, so
unity has to be re-asserted after each slider move rather than set once - and
that is why it is the wrong lever even though it works. The reset and the
re-assert are both asynchronous, so while the volume is moving the applied gain
oscillates between the slider's value and unity. At 20% that is bouncing
between -42 dB and 0 dB several times a second, which is plainly audible as
cutouts. There is no poll rate that fixes it; the gap is inherent. Clamp the
floor instead and nothing has to be written while the audio is running.

Two further things:

- **`pw-dump` does not re-report `softVolumes` reliably, and believing it is
  silence.** It will hand back a stale `[1.0, 1.0]` for a node PipeWire has
  since reset to track the slider. Gate the re-assert on that read and it never
  fires, so the slider's full attenuation stays on top of the speaker's own: at
  20% that is -42 dB of it, and -42 plus the speaker's -24 is -66 dB, which is
  nothing at all. Re-assert unconditionally - on every move, and slowly the
  rest of the time - and never verify this by reading the value back. Verify it
  by measuring the samples; a null sink is no use either, since it discards the
  audio and its volumes never move.
- **Order the writes so a crash leaves things quieter.** Update the device
  first and cancel the gain second; a process that dies in between leaves
  attenuation in place rather than removing it. On exit, write `channelVolumes`
  back into `softVolumes` so the slider goes back to being an ordinary one.
