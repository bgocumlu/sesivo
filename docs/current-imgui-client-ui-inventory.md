# Current ImGui Client UI Inventory

This records what the current ImGui client exposes so a future JUCE UI can keep
the same control surface while replacing the visual design. This is not a design
spec and not a compatibility contract.

## Main Window

- Window title: `Jam Client`
- Top status/menu row:
  - Server host and port
  - Room id
  - RTT
  - Participant count
  - RX and TX byte counters
  - Connected/disconnected audio state
  - FPS
- Main mixer area:
  - Horizontal scrolling strip layout
  - One master/local strip
  - One participant strip per remote user
- Bottom device bar:
  - Audio API selector
  - Input device selector
  - Input channel selector
  - Output device selector
  - Buffer frame size selector
  - Opus packet frame size selector
  - Apply, start, stop, reset, and refresh actions
  - Audio backend error text

## Master Strip

- Local identity label: `YOU`
- Microphone mute/unmute button
- Self-monitor checkbox
- Input level meter
- Input gain fader
- Codec display
- Global Opus jitter controls:
  - Jitter floor in milliseconds
  - Effective jitter packet count
  - Auto jitter default checkbox
  - Queue limit in packets
  - Packet age limit in milliseconds
  - Redundancy depth selector
- Metronome controls and status:
  - BPM input
  - Start/stop
  - Tap tempo
  - Current beat
  - Sync sent/received counters
  - Clock sync status
- Recording controls and status:
  - Start/stop recording
  - Output folder/status
  - Queued and dropped recording blocks
- Path diagnostics:
  - RTT last/average/max
  - Ping loss and ingress loss
  - Estimated total latency
  - End-to-end latency
  - TX pacing/queue information
  - RX queue information
  - PLC and underrun counters
  - Opus frame, jitter, and queue information
- Audio backend diagnostics:
  - API
  - Input/output device latency
  - Sample rate
  - Buffer frames and buffer duration
  - Callback average/deadline/max timing
  - Late callback count
  - Backend latency unknown status
- WAV playback controls:
  - File path input
  - Load
  - Play/pause
  - Seek/progress
  - Volume
  - Local mute

## Participant Strip

- Participant name/color button
- Remote mute toggle
- Pan knob
- Level meter
- Volume fader
- Volume label in dB
- Quality/statistics section:
  - Quality label
  - Quality reason/action text
  - Queue size, average, max, and drift
  - Jitter target
  - Per-participant auto jitter checkbox
  - Per-participant jitter milliseconds input
  - Reset-to-default jitter action
  - Jitter packet count
  - Auto jitter increase/decrease counters
  - Queue limit
  - Frames per packet/callback
  - Decoded frame and packet counters
  - Packet age average/max
  - End-to-end latency average/max or waiting state
  - Receiver drift ppm
  - Sequence/drop diagnostics
  - Buffering, underrun, PLC, PCM hold, and PCM drift drop status

## JUCE Migration Notes

- Preserve these user capabilities first; redesign layout and styling later.
- Do not keep ImGui-only layout decisions as requirements.
- The next useful split is to make the client session/audio/network state
  available through a small UI-facing API, then build JUCE components against
  that API.
