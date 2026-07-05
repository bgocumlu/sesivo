# General Cleanup Backlog

This is current cleanup work that is useful, but not required before starting
the JUCE GUI. It is not a compatibility backlog and should not preserve stale
protocol behavior.

## Client Runtime Extractions

- UDP transport/socket helper: socket setup, QoS logging, send/receive
  scheduling, and rebind support.
- Session/control-message handler: join/leave, ping, control messages, and
  participant add/remove handling.
- Opus send pipeline: audio sender thread, packet batching, redundancy, secure
  audio send.
- Audio callback/mix pipeline: callback mixing, participant playout, WAV,
  metronome, and recording mix handling.
