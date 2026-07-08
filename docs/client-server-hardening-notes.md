# Client/Server Critical Hardening Notes

Status: critical abuse risks to track if the current ephemeral-room design is
kept.

Scope: this document is only about abusable user-originated network traffic to
the server. It does not cover room visibility, private rooms, accounts, durable
identity, or long-term moderation systems.

## Current Risk Level

- Small trusted use: low to moderate risk.
- Public internet use: moderate to high abuse risk.
- Strong secrecy claims: out of scope for now.

The main concern is not valuable stored data or network-path packet sniffing.
The main concern is attackers making the server noisy, expensive, unstable, or
bad for low-latency audio.

## Critical Items

1. Abuse / DoS from UDP control traffic

   Public clients can spam status, create-room, join-token, JOIN, admin, chat,
   metronome, and invalid packets. Existing rate limits help, but public use
   needs tighter per-command limits and quieter repeated-invalid-packet logging.

2. Resource exhaustion

   Add hard caps for total rooms, pending empty rooms, total clients,
   participants per room, unknown endpoints, and rate-limiter entries. Set the
   participant cap to 7 per room and enforce it before accepting a join/token
   request so audio relay work stays predictable. Without global caps, public
   source-port churn or room creation can grow server memory and work.

3. User input weirdness

   Fixed packet sizes prevent simple oversized-field overflow, but long or
   malformed names can still be truncated or displayed/logged badly. Room name,
   display name, room password, chat text, invite text, server address, and all
   fixed string fields should have explicit limits and should be rejected when
   overlong or malformed.

   Allow Unicode display text, but reject invalid UTF-8, null bytes, control
   characters, terminal escape sequences, and bidi override controls before
   storing, logging, forwarding, or displaying it.

4. UDP reflection and amplification

   Small unauthenticated requests can cause larger server responses, especially
   server status responses and room-control errors. Public servers should rate
   limit these paths, cap response sizes, and avoid producing repeated large
   responses for unauthenticated abusive traffic.

5. Low-latency audio protection

   Keep expensive checks out of the audio relay path and the client audio
   callback. Enforce room and participant caps before accepting a client into a
   room. Audio forwarding should stay limited to cheap packet validation,
   endpoint lookup, and relay to the room's current participants.

## Accepted Limitations

- Network-path packet sniffing and replay are out of scope for now. This app is
  not a banking app and should not be described as secure against someone who
  can observe client/server traffic.
- The client does not send the plaintext room password. It sends
  `SHA-256(password)` as `password_hash`, and that hash currently acts like the
  password on the room-control UDP path. Never log or echo it.
- Each room has one server-generated admin bearer token. The server stores only
  a hash and returns the plaintext token once in the create-room response. Never
  log, echo, or include the admin token in invites.

## Acceptable For Now

- Kick-only moderation is acceptable for the current design.
- Weak room passwords are acceptable for ephemeral casual rooms as long as they
  are length-bounded and not described as strong security.
- Full encrypted room-control transport or PAKE-style password auth can wait if
  the app is positioned as ephemeral casual rooms, not secure private rooms.
