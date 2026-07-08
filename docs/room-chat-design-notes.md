# Room Chat Design

Status: Final scoped design, not implemented.

## Goals

- Add per-room chat without affecting low-latency audio.
- Keep chat history in memory only while the room is alive.
- Keep the UI consistent with the existing custom room dialogs.
- Keep admin-only room controls enforced by the server, not just hidden in the UI.
- Keep server work small: no plaintext chat handling, no database, no per-client
  delivery state, no read receipts, no typing indicators, no edits, no deletes,
  and no reactions.

## Room Controls UI

The current room admin row becomes a general room controls row shown to every
joined user.

Visible controls:

- Everyone: `Copy invite`, `Chat`
- Room creator/admin: `Settings`, `Participants`, `Close room`, creator/admin status text
- Normal participants: no admin settings, no participant approval/kick controls, no close room

The row is no longer named `Room Admin`. Its visible label is `Room` for every
joined user.

The `Chat` button must show an orange unread badge/count when new messages
arrive and the chat dialog is closed. Opening the chat dialog clears unread
state for messages already shown. This behaves similarly to the
participants button notification, but it tracks unread chat messages instead of
waiting access requests.

Unread state is client-local only. The server must not store per-participant
unread counts or read state.

## Chat Dialog UI

Use a custom dialog/window like the current Participants and Settings dialogs,
not a generic alert window.

Required layout:

- Scrollable message list.
- Sender name, compact local `HH:MM` time from the server timestamp, and message
  body.
- Composer at the bottom.
- Send button.
- Empty state when there are no messages.
- Error/status line for send failures and reconnect gaps.

The server must stay lightweight by retaining only the last 10 encrypted room
messages. Each client keeps up to 100 local messages for the current room
session so active users can scroll back without increasing server memory or
history replay work. Room chat is a lightweight room-local communication
feature, not a permanent chat product.

When the chat dialog is open, new messages append to the list and the transcript
scrolls to the latest message.

## Hot Path Rule

Chat must stay completely off the realtime audio hot path.

Do not:

- Parse chat inside audio packet handling.
- Touch chat state from the audio callback.
- Share locks between chat state and audio mixing/forwarding.
- Let chat sends, receives, history sync, UI updates, or encryption block audio
  callback execution.
- Store chat messages in audio packet queues.

Do:

- Handle chat on the existing control/event plane with dedicated chat control
  messages.
- Run client chat encryption/decryption outside the audio callback.
- Store client chat state in normal UI/control state.
- Let chat latency be relaxed compared with audio. Tens or hundreds of
  milliseconds are acceptable for chat if audio remains unaffected.

## Wire Protocol

Chat uses dedicated `CtrlHdr::Cmd` control messages. It must not use audio
packets, audio queues, secure audio packets, or the secure media-key rotation
message path.

Required commands:

- `ROOM_CHAT_SEND = 20`: client sends one encrypted chat envelope for its current
  room.
- `ROOM_CHAT_EVENT = 21`: server broadcasts one accepted chat message to every
  participant in the room, including the sender.
- `ROOM_CHAT_SEND_REJECTED = 22`: server reports a rejected send to the sender
  only.
- `ROOM_CHAT_HISTORY_REQUEST = 23`: client requests missed messages after its
  last seen room chat sequence.
- `ROOM_CHAT_HISTORY_RESPONSE = 24`: server returns retained encrypted chat
  messages from the room ring buffer, one message per packet.

The server is authoritative for room chat sequence numbers and server timestamps.
Clients render sent messages only after receiving the corresponding
`ROOM_CHAT_EVENT`; this avoids optimistic-message reconciliation and gives every
participant the same ordering.

Accepted sends are confirmed by the sender receiving its own `ROOM_CHAT_EVENT`.
Rejected sends use `ROOM_CHAT_SEND_REJECTED` with the client message nonce and a
small status code. Accepted sends do not use a separate ACK.

History responses are one retained message per `ROOM_CHAT_HISTORY_RESPONSE`
packet, sent in increasing room chat sequence order. The server sends a final
done packet with no ciphertext after the retained messages. If the requested
sequence is older than the retained ring buffer, the final done packet includes
a truncated flag. If no retained messages match, the server sends only the done
packet.

All chat request messages carry a `request_id`. `ROOM_CHAT_SEND` also carries
the client message nonce so a rejection can be matched to the local composer
state. `ROOM_CHAT_HISTORY_RESPONSE` echoes the history request id.

Rejected sends use existing `ROOM_STATUS_*` status values:

- `ROOM_STATUS_BAD_REQUEST`: malformed room id, room instance id, sender id,
  nonce, ciphertext length, or empty ciphertext.
- `ROOM_STATUS_FORBIDDEN`: endpoint is not joined, sender id does not match the
  endpoint, room does not match the endpoint, or access epoch is stale.
- `ROOM_STATUS_CONFLICT`: duplicate client message nonce is already present in
  the retained room buffer for that sender.
- `ROOM_STATUS_SERVER_ERROR`: server could not queue or store the event.

History response flags:

- `ROOM_CHAT_HISTORY_DONE = 1 << 0`: this packet is the final response for the
  request and contains no ciphertext.
- `ROOM_CHAT_HISTORY_TRUNCATED = 1 << 1`: older requested messages have already
  fallen out of the retained 10-message buffer.

## Message Shape

The client sends encrypted message content plus server-visible routing metadata.
The server must never encrypt, decrypt, or inspect plaintext chat contents.

Server-visible metadata:

- Room id.
- Room instance id.
- Sender participant id.
- Access epoch.
- Client message nonce.
- Ciphertext bytes.

Server-assigned metadata:

- Monotonically increasing room chat sequence number.
- Server receive timestamp as Unix epoch milliseconds from
  `std::chrono::system_clock`.

The encrypted envelope authenticates the server-visible routing metadata as AEAD
associated data:

- Room id.
- Room instance id.
- Sender participant id.
- Access epoch.
- Client message nonce.

The room chat sequence number and server timestamp are not AEAD associated data,
because the server assigns them after receiving the encrypted client envelope.
Clients treat them as delivery/order metadata, not as part of the
end-to-end-authenticated plaintext.

Use the same libsodium AEAD family already used by secure media/control packets:
`crypto_aead_chacha20poly1305_ietf`. Each chat message uses a fresh 12-byte
random client message nonce generated before encryption. The nonce is stored in
the envelope and authenticated through AEAD associated data.

## Server Behavior

The server must maintain a per-room in-memory ring buffer of the last 10 chat
messages. When the room closes or expires, the chat history is deleted with the
room.

The ring buffer and next chat sequence number must live with room state in the
room registry so normal room close and room expiry paths delete chat history
without a second cleanup system.

Server send flow:

1. Client sends a chat message request for the current room.
2. Server verifies the endpoint is joined to that room.
3. Server verifies the message is for the endpoint's current room id and room
   instance id.
4. Server verifies the sender participant id matches the endpoint's server-owned
   participant id.
5. Server verifies the access epoch matches the room's current access epoch.
6. Server applies chat message size and rate limits.
7. If validation fails, server sends `ROOM_CHAT_SEND_REJECTED` to the sender and
   stops.
8. Server assigns the next room chat sequence number and server timestamp.
9. Server stores the encrypted message envelope in the room ring buffer.
10. Server broadcasts a `ROOM_CHAT_EVENT` to all current room participants,
   including the sender.

Server history flow:

1. Client sends `ROOM_CHAT_HISTORY_REQUEST` with the last seen room chat
   sequence.
2. Server verifies the endpoint is currently joined to the room.
3. Server sends one `ROOM_CHAT_HISTORY_RESPONSE` packet per retained message
   with a sequence number greater than the requested sequence and matching the
   current room access epoch.
4. Server sends a final done response with no ciphertext.
5. If the requested sequence is older than the retained ring buffer, the final
   done response carries the truncated flag. The client shows the local status
   line "Earlier chat messages are no longer available."

Delivery is best-effort broadcast plus history catch-up. Room chat does not use
per-client acknowledgements, server-side retry queues, or per-client cursors.

## E2E Chat Encryption

Chat message text must be encrypted end-to-end. The server must not see
plaintext chat messages.

Encryption rules:

- Derive a separate chat key from the current room media secret using the
  domain prefix `sesivo-e2e-chat-v1|`, followed by the room id, `|`, and the
  room instance id. Do not reuse the already-derived media packet key directly.
- Include room id, room instance id, sender participant id, access epoch, and
  client message nonce as authenticated associated data.
- Store and relay encrypted message envelopes.
- Decrypt only on clients that have the correct room key material.

Key rotation rules must match room access security:

- Messages sent under an old room key can remain in the room buffer as
  ciphertext until displaced by newer messages, but history responses do not
  return old-epoch messages.
- New participants without an old room key cannot decrypt old-epoch ciphertext.
- After a kick or room key rotation, new messages use the new derived chat key.
- This is acceptable because history is temporary and room-local.
- History responses include only messages from the current access epoch. The
  server must not return older-epoch messages.

## Limits And Rate Control

Room chat uses small fixed limits:

- Server retains the last 10 accepted messages per room.
- Client keeps the last 100 locally rendered messages for the current room
  session.
- Reject empty messages.
- Reject messages over 512 UTF-8 plaintext bytes before encryption on the
  client.
- Reject encrypted envelopes over 1024 bytes on the server.
- Allow up to 5 chat sends per second per endpoint with a small burst of 10.
- Allow history requests only from joined endpoints and rate-limit them on the
  existing control rate-limit path.

The client must enforce the plaintext limit before encryption. The server must
enforce the encrypted envelope limit because it cannot see plaintext.

## Client Behavior

The client owns local chat UI state:

- Last seen room chat sequence.
- Last rendered room chat sequence.
- Pending sent message nonces.
- Local unread count.
- Chat dialog open/closed state.
- Scroll position.
- Send failure/status text.

On join, the client starts with empty chat state. During the current room
session it keeps the last 100 locally rendered chat messages. It requests
history only when the chat dialog opens, after reconnect, or after detecting a
sequence gap. It requests messages newer than its last seen sequence.

When the client sends a message, it records the client message nonce as pending.
Receiving `ROOM_CHAT_EVENT` with that nonce clears the pending state and renders
the message. Receiving `ROOM_CHAT_SEND_REJECTED` clears the pending state and
shows the rejection in the local status line. If neither packet arrives within
two seconds, the client requests history once; if the nonce is still absent
after that history response completes, the client shows "Message not confirmed"
and leaves the composer text available for retry.

The client detects a sequence gap when a received `ROOM_CHAT_EVENT` sequence is
greater than `last_seen_chat_sequence + 1`. It updates `last_seen_chat_sequence`
to the highest accepted sequence and requests history for the missing range.

Clients show only messages received through `ROOM_CHAT_EVENT` or
`ROOM_CHAT_HISTORY_RESPONSE` for the current room instance and current access
epoch. Messages from previous joins, previous room instances, or previous access
epochs are discarded from local chat state.

## Admin Security

The current server already treats room admin authority as a server-side bearer
token check.

Current pattern:

- Room creation returns an admin token to the creator.
- The server stores only a hash of that admin token.
- Admin requests include the admin token.
- Server calls `room_registry_.authorize_admin(...)`.
- Empty or wrong tokens are rejected as `invalid room admin token`.
- Change access, kick, close room, and media key rotation go through this
  admin-token path.

The UI hiding admin buttons is only UX. The server-side token check is the real
authority boundary. Chat implementation must keep admin tokens out of invites,
normal chat messages, logs, and participant-visible state.
