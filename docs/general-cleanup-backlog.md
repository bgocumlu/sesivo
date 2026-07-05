# General Cleanup Backlog

This is current cleanup work that is useful, but not required before starting
the JUCE GUI. It is not a compatibility backlog and should not preserve stale
protocol behavior.

## Deferred Follow-ups

- Swap the session crypto primitive to libsodium ChaCha20-Poly1305.
  Noted 2026-07-03. Current `session_crypto.h` uses a picosha2-based
  HMAC-SHA256 keystream plus HMAC tag. It is encrypt-then-MAC, uses
  constant-time tag comparison, and has nonce replay protection, so this is not
  urgent at current jam scale.
  Triggers:
  - Rooms grow beyond roughly 8 participants. The server decrypts and
    re-encrypts per recipient, and the picosha2-based keystream can become a
    relay throughput ceiling compared with ChaCha20-Poly1305.
  - The project wants audited crypto before wider hosting.
  Scope:
  - Replace the internals of `seal_audio_packet` and `open_audio_packet` in
    `session_crypto.h` with libsodium `crypto_aead_chacha20poly1305_ietf`.
  - Change key derivation if it helps the new design.
  - Change the wire format cleanly; bump the secure-audio capability/version so
    mismatched builds fail at JOIN. Do not add compatibility readers, writers,
    or fallbacks.
  - Keep `session_crypto_self_test` green and add focused crypto tests if
    needed. Do not add production-binary smoke flags.
  Effort: small, likely one header plus one FetchContent dependency.

## Client Runtime Extractions

- UDP transport/socket helper: socket setup, QoS logging, send/receive
  scheduling, and rebind support.
- Session/control-message handler: join/leave, ping, control messages, and
  participant add/remove handling.
- Opus send pipeline: audio sender thread, packet batching, redundancy, secure
  audio send.
- Audio callback/mix pipeline: callback mixing, participant playout, WAV,
  metronome, and recording mix handling.
