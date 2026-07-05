# JUCE GUI Direction

The JUCE client should preserve the current jam controls, but it should not
copy the ImGui layout or startup behavior.

## Migration Plan

1. Split `src/client/client.cpp` first.
   Extract the app/session/audio/network logic away from ImGui drawing. This is
   done far enough for GUI replacement: startup is in `client.cpp`, runtime is
   behind `ClientRuntime`, and UI code talks through `ClientAppFacade`.
2. Move current ImGui app UI into its own file.
   This is done: `src/client/imgui_client_ui.cpp` depends on
   `ClientAppFacade`, not the concrete client implementation.
3. Add JUCE GUI files.
   Next step. Start with a JUCE app/window/component shell that reads the same
   facade state and dispatches the same commands.
4. Replace the `client` target UI entrypoint.
   Once JUCE can start, connect, show devices, start/stop audio, and show
   participants, switch the executable from the ImGui shell to the JUCE shell.
5. Remove ImGui/GLFW completely.
   No long-term dual UI.

## Runtime Cleanup Backlog

These are useful `client_runtime.cpp` extractions, but they are not blockers for
starting the JUCE GUI:

- UDP transport/socket helper: socket setup, QoS logging, send/receive
  scheduling, and rebind support.
- Session/control-message handler: join/leave, ping, control messages, and
  participant add/remove handling.
- Opus send pipeline: audio sender thread, packet batching, redundancy, secure
  audio send.
- Audio callback/mix pipeline: callback mixing, participant playout, WAV,
  metronome, and recording mix handling.

## Startup Responsiveness

- Show the main window as early as possible.
- Do not block first paint on audio device enumeration, saved-device matching,
  stream opening, room discovery, or server connection attempts.
- Show explicit loading states for:
  - Audio device list loading
  - Audio stream starting
  - Room/server discovery
  - Joining/reconnecting
- Prefer cached device names for the first frame, then refresh device details in
  the background.
- Run manual refreshes, auto-start audio, and backend capability scans as
  cancellable background jobs.
- Keep the UI thread limited to rendering snapshots and dispatching user
  commands.

## Current ImGui Behavior To Avoid Carrying Forward

- The client currently chooses default audio devices and applies saved device
  preferences before the GUI loop starts.
- The client currently tries to auto-start the audio stream before the GUI loop
  starts.
- The ImGui bottom device bar can enumerate devices on its first draw.

Those behaviors are acceptable for the current app while refactoring, but the
JUCE version should move them behind asynchronous state and visible progress.
