# Client/server hardening notes

Status: reviewed against `main` on 2026-07-24.

This tracks current server-abuse and control-plane hardening work. It is not a
protocol compatibility document.

## Current protections

- Authenticated audio has protocol-aware rate limiting that preserves valid
  low-latency traffic while bounding floods.
- Status, room-control, general control, chat-send, secure-control, and unknown
  endpoint traffic use separate rate limits.
- Rate-limiter state and unknown-endpoint tracking are bounded to 4096
  endpoints and expire inactive entries.
- Each room is capped at the current supported envelope of 32 participants.
- Audio and room chat use authenticated encryption for signed sessions. Chat
  has separate key derivation, fixed plaintext/ciphertext limits, a bounded
  server history, and send-rate limiting.
- Signed join tokens, secure packet nonces, and replay tracking protect the
  authenticated session paths they cover.
- Room admin authority uses a server-generated bearer token whose hash is
  stored by the server.
- Fixed packet structures and bounded relay pools constrain per-packet memory
  use on the media path.

## Open priorities

### 1. Global resource ceilings

Per-room, endpoint, and rate-limiter limits exist, but the server still needs
reviewed hard ceilings for:

- Total connected clients.
- Total rooms.
- Pending or empty rooms.
- Aggregate retained room state.

Enforce limits before expensive room or client setup. Rejections must remain
cheap and rate-limited.

### 2. Text and fixed-field validation

Review every user-controlled room name, display name, profile id, invite field,
chat field, server address, and fixed string before storing, logging,
forwarding, or displaying it.

Current byte-size bounds are not a complete text-validation policy. Define and
test the current contract for:

- Invalid UTF-8.
- Embedded nulls.
- Control and terminal-escape characters.
- Bidirectional override controls.
- Truncation versus rejection.

### 3. Reflection, amplification, and noisy failures

Re-audit unauthenticated and invalid request paths for response amplification.
Status and room-control responses should remain bounded and separately
rate-limited. Repeated invalid traffic must not produce unbounded logging or
expensive formatting.

### 4. Control-plane security claims

Do not make one broad secrecy claim for every UDP control path. Secure media
and chat have stronger guarantees than room discovery and room-control
metadata.

The room password hash acts as a bearer value on the current room-control path.
The room admin token is also bearer authority. Neither value may be logged,
echoed to unrelated clients, placed in invites, or exposed through metrics.

If stronger private-room control security becomes a product requirement,
design it as a clean current protocol rather than adding a legacy fallback.

### 5. Protect the realtime path

New validation, abuse accounting, chat work, metrics, and logging must stay out
of the client audio callback and the server relay hot path. Capacity rejection
belongs at admission boundaries. Media forwarding should remain bounded packet
validation, endpoint lookup, and fan-out to the current room snapshot.

## Current accepted limits

- The supported room capacity is 32 participants, matching the current
  low-latency operating envelope.
- Room state and chat history are ephemeral and memory-only.
- There is no ICE, STUN, TURN, or TCP media fallback.
- Full account, moderation, durable identity, and permanent chat systems are
  outside the current product scope.
