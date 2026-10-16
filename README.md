# tuxplay

Use AirPlay 2 speakers as ordinary Linux audio outputs. Pick **Tuxplay (AirPlay 2)**
in your sound settings and audio from any app plays on your HomePods, Apple TV or
other AirPlay 2 receivers.

## Features

- **Stereo pairs as one output.** A HomePod pair appears as a single speaker. Both
  halves get the same anchor, so they start and stay together.
- **Several outputs at once.** Play to both of two stereo pairs together. Every
  receiver joins one clock, so they all play in step.
- **Behaves like a Mac.** It speaks AirPlay the way macOS 27 does over a network:
  same headers, same clock exchange. Selected speakers stay lit while it holds
  them, as with a Mac.
- **Video stays in sync.** Players are told the real delay, so they hold the
  picture back to match. Nothing to adjust.
- **Volume that reaches the speakers.** The desktop slider sets the speakers' own
  volume. For a pair, it goes to the pair's leader, as a Mac does.
- **Shares with other devices.** A phone or Mac can take the speakers at any time.
  Tuxplay steps aside and takes them back only when something new plays here.
- **Three ways to control it:** a top bar icon, a web page and a CLI.

## Install

Needs an apt-based distribution (Ubuntu, Debian) running PipeWire, and `sudo`.

```sh
git clone https://github.com/ksha23/tuxplay
cd tuxplay
./install.sh
```

Then choose **Tuxplay (AirPlay 2)** as the output in Sound settings, and pick which
speakers it plays to from the top bar icon or `http://localhost:8730`.

To remove everything, run `./uninstall.sh`.

## Choosing speakers

The sound panel has one entry for this project, **Tuxplay (AirPlay 2)**. Which
receivers it feeds is chosen in Tuxplay:

- **Top bar.** The icon shows the current state. Its menu has a checkbox per
  device, and **Settings...** opens the delay control. Needs a shell that shows
  AppIndicators, which Ubuntu does by default.
- **Web page.** `http://<this-machine>:8730` offers the same controls from a
  browser, a phone, or a desktop without a tray.
- **Command line.** See below.

Selecting a speaker connects to it at once. After a restart, Tuxplay remembers
what was selected but connects only when something plays, so starting the
computer never takes speakers another device is using.

## Tuning

```sh
tuxplay status              # delay, sync correction, what players are told
tuxplay delay 250           # the delay you hear, in ms (50-5000)
```

**delay** trades responsiveness for robustness. Lower starts sooner, too low
gives dropouts on a busy network. 250 ms suits a wired network. A change applies
at once: what is playing pauses for a moment and continues with the new delay,
and players are told the new value at the same time.

For a player that gets picture and sound wrong anyway, `tuxplay sync earlier 20`
(sound is late), `tuxplay sync later 20` (sound is early) and `tuxplay sync reset`
change only what players are told. Normally none is needed.

## What gets installed

| | |
|---|---|
| `tuxplayd` | user service: the PipeWire sink **Tuxplay (AirPlay 2)** and the AirPlay 2 sender, with an API on `localhost:3689` |
| `tuxplay-ui` | user service: the web page on `:8730` and the state the top bar reads |
| `tuxplay-indicator` | user service: the top bar icon and settings window |
| `tuxplay` | the command line tool |

Speaker choices, volumes and the delay are kept in `~/.local/state/tuxplayd.json`.

## How it works

```
app  ->  PipeWire sink "Tuxplay"  ->  tuxplayd  ->  AirPlay 2 (buffered, PTP)  ->  speakers
```

`tuxplayd` is one program. Its sink is a PipeWire driver clocked by the
speakers' own PTP clock, so audio is produced at exactly the rate the speakers
play it: nothing drifts, nothing is resampled. Each 352-sample frame is encoded as
ALAC and sent to every selected receiver over AirPlay 2's buffered transport,
with an anchor that says when it plays on that shared clock. Frame F always plays
at the time it was produced plus the delay, for every receiver, which is what
keeps pairs and groups in step.

Pairing uses HomeKit transient pairing, the method for senders that are not part
of your Home. The pairing and stream code is vendored from
[pair_ap](https://github.com/ejurgensen/pair_ap) and
[owntone-mini](https://github.com/lo-tech-systems/owntone-mini)'s PTP library.

## Troubleshooting

- **Selected but silent.** Another device may be holding the speakers. Play
  something new here, or select the speakers again, to take them back.
- **Logs.** `journalctl --user -u tuxplayd -f`. For protocol detail, stop the
  service and run `tuxplayd -v` in a terminal.
- **State.** `curl -s localhost:3689/api/status` and
  `curl -s localhost:3689/api/outputs`.

## See also

- [docs/](docs/): protocol, PipeWire and measurement notes from building this.

## License

MIT, per `LICENSE`. The vendored code in `tuxplayd/vendor/` is MIT as well, under
its authors' copyright.

AirPlay, Apple TV and HomePod are trademarks of Apple Inc. This project is not
affiliated with or endorsed by Apple. The names are used only to describe what it
works with.
