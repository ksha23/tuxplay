# Notes

Working notes from building this - the protocol behaviour that is not written
down anywhere obvious, what a Mac actually does when you measure it rather than
assume, and the things that wasted a day so they do not waste another.

- [airplay2-protocol.md](airplay2-protocol.md) - transports, the anchor and its
  `rate` field, `RECORD`, stereo pairs, the event channel, and a table of
  everything ruled out about the white indicator.
- [what-a-mac-does.md](what-a-mac-does.md) - measured, including the two
  measurement traps that produced confident wrong answers.
- [pipewire-notes.md](pipewire-notes.md) - sinks that will not run, latency that
  will not stick, and where it actually lives.
- [debugging.md](debugging.md) - useful checks, the failure mode where every
  sender-side signal is green and nothing plays, and the shell and timing traps
  that bit this project more than once.

Claims here are marked measured or inferred. Where something was inferred and
later disproved, the disproof is kept rather than the file being quietly
corrected - knowing which plausible explanations are dead is most of the value.
