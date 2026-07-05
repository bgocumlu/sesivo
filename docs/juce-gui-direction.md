# JUCE GUI Direction

The JUCE client should preserve the jam controls from the removed ImGui UI, but
it should not copy the old layout or startup behavior.

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
  background jobs; add cancellation when a job can outlive the current window or
  selected device state.
- Keep the UI thread limited to rendering snapshots and dispatching user
  commands.

## Current State

- Audio device enumeration, saved-device matching, and startup audio open are
  launched by the JUCE mixer after the window is running.
- The device bar shows a loading state while that startup work runs off the UI
  thread.
- Server connection still starts from `ClientRuntime` construction; move it
  behind explicit joining/reconnecting state when room discovery is added.

## Removed ImGui Behavior To Avoid Carrying Forward

- The old client chose default audio devices and applied saved device
  preferences before the GUI loop started.
- The old client tried to auto-start the audio stream before the GUI loop
  started.
- The old bottom device bar could enumerate devices on its first draw.

The JUCE version should move those behind asynchronous state and visible
progress.
