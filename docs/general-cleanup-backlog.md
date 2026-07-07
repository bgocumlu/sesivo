# General Cleanup Backlog

This is current cleanup work that is useful, but not required before starting
the JUCE GUI. It is not a compatibility backlog and should not preserve stale
protocol behavior.

## Deferred Follow-ups

- Finish installed invite-link packaging.
  Noted 2026-07-07. Core crypto cleanup is closed: secure audio uses libsodium
  IETF ChaCha20-Poly1305 AEAD, clients derive the media key from a client-only
  media secret plus the signed room instance, the server forwards opaque sealed
  media/control packets without decrypting or re-encrypting them, and room
  admins can copy/paste shortcut invite links while encrypted client-to-client
  room-key handoff stays hidden from the UI. The client now uses a real
  `JUCEApplication`,
  routes `sesivo://` launches into Join Invite, self-registers `sesivo://` for
  the current Windows user, declares the scheme in the macOS bundle plist, and
  includes a Linux `.desktop` scheme-handler template.
  Scope:
  - Move Windows `sesivo://` registration into the installer once Windows
    installer packaging exists.
  - Install/register the Linux `.desktop` file in the future Linux package.
  - Add the optional `https://sesivo.app/invite/...` web bridge.
  - Keep the server blind to media keys; do not move the media key into room
    create, room join-token, metrics, or logging payloads.
  Effort: small.

## Release And Distribution

- Add Windows installer packaging.
  Noted 2026-07-06. macOS packaging/signing/notarization is working; Windows
  still needs an installer path.
  Scope:
  - Use Inno Setup first unless MSI/enterprise deployment becomes a real need.
  - Package `sesivo.exe` with Start Menu shortcut, uninstall entry, optional
    desktop shortcut, and install directory selection.
  - Decide whether `sesivo-server.exe` ships as a separate artifact or an
    optional installer component. Do not silently install/run a server service.
  - Keep installer output naming aligned with macOS, e.g.
    `sesivo-0.1.0-setup.exe`.

- Add Windows Authenticode signing.
  Noted 2026-07-06. The Apple Developer ID `.p12` only signs macOS artifacts;
  Windows needs its own code-signing provider.
  Scope:
  - Choose a Windows signing path: OV/EV code-signing certificate or Microsoft
    Trusted Signing.
  - Sign both `sesivo.exe` and the final installer `.exe`.
  - Timestamp signatures.
  - Add verification commands to the release script.

- Add GitHub release publishing.
  Noted 2026-07-06. The old Electron `jam-app` release script creates a
  `v<version>` GitHub release when missing, or uploads artifacts with
  `gh release upload --clobber` when it already exists.
  Scope:
  - Add a native release publishing script using the same pattern.
  - Upload `build/package/sesivo-0.1.0.dmg` and the future Windows installer.
  - Keep version/tag source single-purpose and explicit; likely centralize the
    `0.1.0` value instead of repeating it across CMake/package scripts.
  - Consider checksum artifacts before public distribution.

- Decide update flow after GitHub releases exist.
  Noted 2026-07-06. Native JUCE has no Electron auto-updater equivalent.
  Scope:
  - Start with a simple manual update check against GitHub Releases.
  - Show latest version and open/download the installer or DMG.
  - Defer silent/background updates until the app has stable release channels.
  - Revisit Sparkle on macOS and WinSparkle or installer handoff on Windows if
    automatic updates become important.

## Client Runtime Extractions

- UDP transport/socket helper: socket setup, QoS logging, send/receive
  scheduling, and rebind support.
- Session/control-message handler: join/leave, ping, control messages, and
  participant add/remove handling.
- Opus send pipeline: audio sender thread, packet batching, redundancy, secure
  audio send.
- Audio callback/mix pipeline: callback mixing, participant playout, WAV,
  metronome, and recording mix handling.
