# tuxplay

Use AirPlay 2 speakers as ordinary Linux audio outputs. Pick **Tuxplay (AirPlay 2)**
in your sound settings and audio from any app plays on your HomePods, Apple TV or
other AirPlay 2 receivers.

## Features

- **Stereo pairs as one output.** A HomePod pair appears as a single speaker, with
  both halves kept in step.
- **Several receivers at once.** Play to a pair, an Apple TV and a single HomePod
  together.
- **Behaves like a Mac.** The speakers light while playing and go dark on pause,
  and pausing or resuming loses no audio.
- **Video stays in sync.** The sink reports its real latency, so players delay
  video to match. Delay and sync adjust live.
- **Volume that reaches the speakers.** The desktop slider controls the speakers'
  own volume across their full range, and follows changes made on the speaker.
- **Shares with other devices.** Another device can take the speakers at any time;
  Tuxplay steps aside and moves your audio to a local output.
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

- **Top bar.** The icon shows the current state. Its menu has a checkbox per device
  and the hold toggle; **Settings...** opens the delay and sync controls. Needs a
  shell that shows AppIndicators, which Ubuntu does by default.
- **Web page.** `http://<this-machine>:8730` offers the same controls from a
  browser, a phone, or a desktop without a tray.
- **Command line.** See below.

You may also see sound panel entries named after individual speakers. Those are
PipeWire's built-in AirPlay 1 outputs: one speaker each, no stereo pairs. They are
unrelated to Tuxplay and can be ignored.

## Tuning

```sh
tuxplay status              # transport, delay, sync and hold
tuxplay delay 400           # the delay you hear, in ms (150-3500)
tuxplay sync earlier 20     # sound is late, video ahead
tuxplay sync later 20       # sound is early, audio ahead
tuxplay sync reset
tuxplay hold on             # or off
```

**delay** trades responsiveness for robustness: lower starts sooner, too low
gives dropouts on busy Wi-Fi. 350-400 ms suits most networks. It applies live;
the speakers drop out for a few seconds while the sender restarts, but nothing on
the desktop is interrupted.

**sync** shifts the sound against the picture and applies instantly. Adjust 20-30
ms at a time.

**hold** decides what happens when nothing is playing:

| hold | when idle | starting again |
|---|---|---|
| on | connection kept, speakers dark | immediate |
| off | speakers released for other devices | about 1-1.5 s |

Either way another device can take the speakers whenever it wants them. When that
happens your audio moves to the first local output; set `TUXPLAY_FALLBACK_SINK`
to choose a specific one.

## What gets installed

| | |
|---|---|
| **Tuxplay (AirPlay 2)** | the PipeWire sink you select in Sound settings |
| `owntone-mini` | system service: the AirPlay 2 sender |
| `tuxplay-share` | user service: takes the speakers when you play, holds or releases them when idle, steps aside for other devices |
| `tuxplay-volume` | user service: links the desktop volume slider to the speakers' volume |
| `tuxplay-ui` | user service: the web page on `:8730` and the state the top bar reads |
| `tuxplay-indicator` | user service: the top bar icon and settings window |
| `tuxplay` | the command line tool |

## How it works

```
app  ->  PipeWire sink "Tuxplay"  ->  fifo  ->  owntone-mini  ->  AirPlay 2  ->  speakers
```

A PipeWire pipe-tunnel sink writes audio into a fifo.
[owntone-mini](https://github.com/lo-tech-systems/owntone-mini), a trimmed OwnTone
fork with AirPlay 2 stereo group support, reads it and streams to the selected
receivers over the AirPlay 2 realtime transport, the one macOS uses for system
audio.

Two patches in `patches/` adapt the pieces:

- **owntone-mini** becomes an always-on audio device: stereo pairs start and stay
  in step, playback state is reported so the speakers' lights follow it, speaker
  selection persists across restarts, latency stays bounded after dropouts, and
  the speakers are released when another device asks for them.
- **PipeWire's pipe-tunnel module** declares the sink's latency to clients and can
  change it at runtime.

## Troubleshooting

- **Connected but silent.** Check that another device is not holding the speakers.
  A Mac that last played to them keeps them until its output is switched away.
- **An app is silent after PipeWire restarts.** Existing streams are left paused;
  restart playback or reload the browser tab.
- **After a PipeWire package upgrade.** The upgrade replaces the patched module.
  Run `./install.sh` again.
- **Logs.** `journalctl -u owntone-mini -f` and
  `journalctl --user -u tuxplay-share -f`.

## See also

- [docs/](docs/): protocol and PipeWire notes from building this.
- [Preroll](https://github.com/ksha23/preroll): lower AirPlay latency on macOS.

## License

The scripts, configuration and documentation in this repository are MIT, per
`LICENSE`.

The files in `patches/` modify owntone-mini (GPL-2.0) and PipeWire (MIT), and each
carries the licence of the project it applies to. Applying the owntone-mini patch
produces a GPL-2.0 binary.

AirPlay, Apple TV and HomePod are trademarks of Apple Inc. This project is not
affiliated with or endorsed by Apple; the names are used only to describe what it
works with.
