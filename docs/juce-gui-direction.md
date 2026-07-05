# JUCE GUI Direction

The JUCE client should preserve the current jam controls, but it should not
copy the ImGui layout or startup behavior.

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
