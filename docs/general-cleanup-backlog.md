# General cleanup backlog

Status: reviewed against `main` on 2026-07-24.

This contains current cleanup and distribution work only. Completed historical
items are preserved in
[`docs/archive/plans/general-cleanup-backlog-2026-07.md`](archive/plans/general-cleanup-backlog-2026-07.md).

## Release and distribution

### Windows installer

Windows currently ships as a portable ZIP. Add an installer when installed
desktop distribution is required.

- Prefer Inno Setup unless MSI or enterprise deployment becomes a concrete
  requirement.
- Install `sesivo.exe`, Start Menu and uninstall entries, and an optional
  desktop shortcut.
- Register the `sesivo://` URL scheme through the installer instead of relying
  only on per-user runtime registration.
- Sign and timestamp both the application and installer.
- Do not silently install or run the server as a Windows service.

### Trusted Windows signing identity

`tools/package-windows.ps1` already supports Authenticode signing with a PFX,
certificate subject, or development self-signed certificate. Public Windows
distribution still needs an operationally managed trusted signing identity and
release credentials.

### Linux desktop client packaging

The repository contains `packaging/linux/sesivo.desktop`, including the
`sesivo://` scheme handler, but there is no current installed Linux desktop
client package.

- Install and register the desktop file when Linux client packaging is added.
- Keep the headless Linux server installer separate from desktop-client
  packaging.

### Optional web invite bridge

Consider an `https://sesivo.app/invite/...` bridge only if browser-to-app invite
handoff becomes necessary. The native `sesivo://` invite remains the current
application contract.

## Client runtime extractions

`src/client/client_runtime.cpp` still combines several independent ownership
and hot-path responsibilities.

The first extraction is governed by the
[local audio regression roadmap](local-audio-regression-roadmap.md): build
behavior-preserving receive/playout contracts while extracting the production
boundary they exercise.

Later candidates, only when protected by focused tests:

- UDP transport, socket setup, QoS, receive scheduling, and rebind handling.
- Session lifecycle and control-message dispatch.
- Capture accumulation, Opus encode, redundancy, and transmit queueing.
- Audio callback mixing, WAV playback, metronome, and recording composition.

Do not perform broad file-splitting cleanup without a product-level contract
that protects the moved behavior.

## Completed baseline

Do not reopen these as missing work:

- Native GitHub release publishing exists in `tools/gh-release.mjs`.
- Windows portable packaging and Authenticode support exist in
  `tools/package-windows.ps1`.
- macOS packaging, signing, and notarization tooling exists.
- The client performs a version check and opens the current downloads page.
- Windows runtime and macOS bundle URL-scheme registration exist.
