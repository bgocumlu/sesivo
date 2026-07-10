#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Packet identification magic numbers
constexpr uint32_t PING_MAGIC  = 0x50494E47;  // 'PING'
constexpr uint32_t CTRL_MAGIC  = 0x4354524C;  // 'CTRL'
constexpr uint32_t AUDIO_V3_MAGIC = 0x41553349;  // 'AU3I'
constexpr uint32_t AUDIO_REDUNDANT_MAGIC = 0x41555244;  // 'AURD'
constexpr uint32_t SECURE_AUDIO_MAGIC = 0x53454341;  // 'SECA' E2E-sealed audio
constexpr uint32_t SECURE_CONTROL_MAGIC = 0x53454343;  // 'SECC' E2E-sealed control
constexpr uint32_t E2E_KEY_ENVELOPE_MAGIC = 0x4532454B;  // 'E2EK' sealed key handoff

// Buffer sizes
constexpr size_t AUDIO_BUF_SIZE = 512;
constexpr int UDP_SOCKET_BUFFER_BYTES = 4 * 1024 * 1024;

// One room limit shared by server admission, client mixing, and UI. The client
// audio callback publishes at most this many participants; the server must
// never admit more than it, or extra members would be silently unheard.
constexpr size_t MAX_ROOM_PARTICIPANTS = 32;

// Jitter buffer configuration (client only - server just relays packets)
constexpr size_t MAX_OPUS_QUEUE_SIZE       = 128; // Hard safety cap for Opus receive queue
constexpr size_t TARGET_OPUS_QUEUE_SIZE    = 3;   // Target queue size for adaptive management
constexpr size_t MIN_JITTER_BUFFER_PACKETS = 3;   // Minimum packets before playback starts
constexpr size_t MIN_OPUS_JITTER_PACKETS = 0;     // Manual testing can disable Opus prebuffer
constexpr int    DEFAULT_OPUS_JITTER_MS = 20;     // Default steady-state Opus playout floor
constexpr int    DEFAULT_OPUS_AUTO_START_JITTER_MS = 40; // Initial adaptive internet cushion
constexpr size_t DEFAULT_OPUS_JITTER_PACKETS = 2; // 20 ms at the default 10 ms packet
constexpr size_t DEFAULT_OPUS_AUTO_START_JITTER_PACKETS = 4; // 40 ms at default packet
constexpr size_t DEFAULT_OPUS_QUEUE_LIMIT_PACKETS = 64; // Default Opus burst capacity
constexpr size_t MAX_OPUS_JITTER_PACKETS = 32;    // User-facing Opus jitter limit
constexpr size_t MIN_OPUS_QUEUE_LIMIT_PACKETS = 1;
constexpr size_t MAX_OPUS_QUEUE_LIMIT_PACKETS = 128; // User-facing Opus queue limit
constexpr size_t MAX_OPUS_CONSECUTIVE_GAP_PLC_PACKETS = 2; // Cap synthetic audio on large gaps
constexpr int    DEFAULT_JITTER_PACKET_AGE_MS = 120; // Default age limit at playout
constexpr int    MIN_JITTER_PACKET_AGE_MS = 0;        // Manual testing can disable age drops
constexpr int    MAX_JITTER_PACKET_AGE_MS = 250;      // User-facing age limit
constexpr size_t AUDIO_REDUNDANT_TARGET_BYTES = 1200; // Keep protected UDP datagrams below common MTUs
constexpr uint8_t MAX_AUDIO_REDUNDANT_PACKETS = 12; // Current packet + recent history within target bytes
constexpr int    OPUS_REDUNDANCY_DEPTH_AUTO = -1; // Choose history depth from packet rate
constexpr int    DEFAULT_OPUS_REDUNDANCY_DEPTH_PACKETS = OPUS_REDUNDANCY_DEPTH_AUTO;
constexpr int    MAX_OPUS_REDUNDANCY_DEPTH_PACKETS =
    static_cast<int>(MAX_AUDIO_REDUNDANT_PACKETS) - 1; // Previous packets per datagram

// Endpoint capabilities negotiated in extended JOIN/JOIN_ACK packets.
constexpr uint32_t AUDIO_CAP_REDUNDANCY = 1U << 0;
constexpr uint32_t AUDIO_CAP_SECURE_AUDIO = 1U << 2;
constexpr uint32_t AUDIO_SUPPORTED_CAPABILITIES =
    AUDIO_CAP_REDUNDANCY | AUDIO_CAP_SECURE_AUDIO;

// Secure audio packet envelope uses a 96-bit AEAD nonce and a 16-byte
// ChaCha20-Poly1305 tag. The packet header is cleartext authenticated metadata.
constexpr size_t SECURE_PACKET_NONCE_BYTES = 12;
constexpr size_t SECURE_PACKET_TAG_BYTES = 16;
constexpr size_t MEDIA_SECRET_MAX_BYTES = 128;
constexpr size_t E2E_PUBLIC_KEY_BYTES = 32;
constexpr size_t E2E_SECRET_KEY_BYTES = 32;
constexpr size_t E2E_KEY_ENVELOPE_MAX_BYTES = 256;
constexpr size_t ROOM_CHAT_RETAINED_MESSAGES = 10;
constexpr size_t ROOM_CHAT_PLAINTEXT_MAX_BYTES = 512;
constexpr size_t ROOM_CHAT_CIPHERTEXT_MAX_BYTES = 1024;

// Type aliases
template <size_t N>
using Bytes = std::array<char, N>;

#pragma pack(push, 1)

struct MsgHdr {
    uint32_t magic;
};

struct SyncHdr : MsgHdr {
    uint32_t seq;
    int64_t  t1_client_send;
    int64_t  t2_server_recv;
    int64_t  t3_server_send;
};

struct CtrlHdr : MsgHdr {
    enum class Cmd : uint8_t {
        JOIN              = 1,
        LEAVE             = 2,
        ALIVE             = 3,
        PARTICIPANT_LEAVE = 4,  // Server broadcasts when participant leaves
        PARTICIPANT_INFO  = 6,  // Server broadcasts room-local participant metadata
        METRONOME_SYNC    = 7,  // Server relays room-local metronome state
        JOIN_ACK          = 8,  // Server confirms that this endpoint is registered
        JOIN_REQUIRED     = 9,  // Server asks an unknown endpoint to re-send JOIN
        AUDIO_PATH_STATS  = 10, // Server reports sender-to-server audio ingress health
        SERVER_STATUS_REQUEST = 11, // Browser asks for server/room summaries
        SERVER_STATUS_RESPONSE = 12,
        ROOM_CREATE_REQUEST = 13, // Create in-memory room and receive join/admin tickets
        ROOM_CREATE_RESPONSE = 14,
        ROOM_JOIN_TOKEN_REQUEST = 15, // Request short-lived room join ticket
        ROOM_JOIN_TOKEN_RESPONSE = 16,
        ROOM_ADMIN_REQUEST = 17, // Change password, kick participant, close room
        ROOM_ADMIN_RESPONSE = 18,
        ROOM_REMOVED = 19, // Server tells this client it was removed from the room
        ROOM_CHAT_SEND = 20, // Client submits one encrypted room chat message
        ROOM_CHAT_EVENT = 21, // Server broadcasts one accepted room chat message
        ROOM_CHAT_SEND_REJECTED = 22, // Server rejects a chat send
        ROOM_CHAT_HISTORY_REQUEST = 23, // Client asks for retained chat tail
        ROOM_CHAT_HISTORY_RESPONSE = 24, // Server returns one retained chat message or done
        JOIN_DENIED = 25,
    } type;
    uint32_t participant_id = 0;  // Used for PARTICIPANT_LEAVE to identify which participant left
};

struct JoinHdr : CtrlHdr {
    Bytes<64>  room_id;
    Bytes<64>  room_handle;
    Bytes<64>  profile_id;
    Bytes<64>  display_name;
    Bytes<512> join_token;
    uint32_t   capabilities = 0;
    Bytes<E2E_PUBLIC_KEY_BYTES> key_public;
};

struct JoinAckHdr : CtrlHdr {
    uint32_t capabilities = 0;
};

struct JoinDeniedHdr : CtrlHdr {
    uint8_t reason = 0;  // 1 = room full
};

struct ParticipantInfoHdr : CtrlHdr {
    Bytes<64> profile_id;
    Bytes<64> display_name;
};

struct ParticipantInfoCapsHdr : ParticipantInfoHdr {
    uint32_t capabilities = 0;
    Bytes<E2E_PUBLIC_KEY_BYTES> key_public;
};

struct E2EKeyEnvelopeHdr : MsgHdr {
    uint32_t sender_id = 0;
    uint32_t target_id = 0;
    uint16_t encrypted_bytes = 0;
    uint16_t reserved = 0;
    Bytes<E2E_KEY_ENVELOPE_MAX_BYTES> encrypted;
};

struct E2EKeyEnvelopePayload {
    uint16_t media_secret_bytes = 0;
    uint16_t reserved = 0;
    uint32_t access_epoch = 0;
    Bytes<MEDIA_SECRET_MAX_BYTES> media_secret;
};

constexpr uint8_t METRONOME_FLAG_RUNNING = 1 << 0;

struct MetronomeSyncHdr : CtrlHdr {
    uint32_t bpm_milli      = 120000;
    uint32_t beat_number    = 0;
    uint8_t  flags          = 0;
    int64_t  sender_time_ns = 0;
    int64_t  effective_server_time_ns = 0;
    uint32_t sequence = 0;
};

enum class AudioCodec : uint8_t {
    Opus = 1,
};

struct AudioHdrV3 : MsgHdr {
    uint32_t              sender_id;      // Server-owned sender identifier
    uint32_t              sequence;       // Sender-local packet sequence
    uint32_t              sample_rate;    // Packet sample rate
    uint16_t              frame_count;    // Frames per packet
    uint16_t              payload_bytes;  // Audio payload bytes
    uint8_t               channels;       // Channel count in payload
    AudioCodec            codec;          // Payload codec
    int64_t               capture_server_time_ns; // Capture time in server-clock domain
    Bytes<AUDIO_BUF_SIZE> buf;
};

struct SecureAudioHdr : MsgHdr {
    uint32_t sender_id = 0;       // Sender-owned JOIN_ACK participant id
    uint32_t sequence = 0;        // Sender-local packet sequence
    uint32_t sample_rate = 0;     // Plaintext audio packet sample rate
    uint16_t frame_count = 0;     // Plaintext Opus frame count
    uint16_t plaintext_bytes = 0; // Sealed AudioHdrV3/AURD bytes
    uint16_t encrypted_bytes = 0; // Ciphertext plus AEAD tag bytes
    uint16_t reserved = 0;
    uint8_t  channels = 0;       // Plaintext channel count
    AudioCodec codec = AudioCodec::Opus;
    int64_t  capture_server_time_ns = 0;
    Bytes<SECURE_PACKET_NONCE_BYTES> nonce;
};

constexpr size_t SECURE_PACKET_HEADER_BYTES = sizeof(SecureAudioHdr);

constexpr uint8_t SECURE_CONTROL_ROTATE_MEDIA_KEY = 1;

struct SecureControlHdr : MsgHdr {
    uint32_t sender_id = 0;       // Sender-owned JOIN_ACK participant id
    uint32_t sequence = 0;        // Sender-local secure control sequence
    uint16_t plaintext_bytes = 0; // Sealed control payload bytes
    uint16_t encrypted_bytes = 0; // Ciphertext plus AEAD tag bytes
    uint16_t reserved = 0;
    Bytes<SECURE_PACKET_NONCE_BYTES> nonce;
};

constexpr size_t SECURE_CONTROL_HEADER_BYTES = sizeof(SecureControlHdr);

struct MediaKeyRotationPayload {
    uint8_t  command = SECURE_CONTROL_ROTATE_MEDIA_KEY;
    uint8_t  reserved8 = 0;
    uint16_t media_secret_bytes = 0;
    uint32_t access_epoch = 0;
    Bytes<MEDIA_SECRET_MAX_BYTES> media_secret;
};

struct AudioRedundantHdr : MsgHdr {
    uint8_t packet_count = 0;
    uint8_t reserved[3] = {};
};

struct AudioPathStatsHdr : CtrlHdr {
    uint32_t interval_received = 0;
    uint32_t interval_sequence_gaps = 0;
    uint32_t total_received = 0;
    uint32_t total_sequence_gaps = 0;
    uint32_t interval_unrecovered_sequence_gaps = 0;
    uint32_t total_unrecovered_sequence_gaps = 0;
    uint16_t observed_frame_count = 0;
    uint16_t reserved = 0;
};

constexpr uint8_t ROOM_FLAG_LOCKED = 1 << 0;
constexpr uint8_t ROOM_FLAG_CREATED = 1 << 1;
constexpr uint8_t ROOM_ACCESS_OPEN = 1;
constexpr uint8_t ROOM_ACCESS_PASSWORD = 2;
constexpr uint8_t ROOM_ACCESS_APPROVE = 3;
constexpr uint8_t ROOM_STATUS_OK = 0;
constexpr uint8_t ROOM_STATUS_BAD_REQUEST = 1;
constexpr uint8_t ROOM_STATUS_NOT_FOUND = 2;
constexpr uint8_t ROOM_STATUS_FORBIDDEN = 3;
constexpr uint8_t ROOM_STATUS_CONFLICT = 4;
constexpr uint8_t ROOM_STATUS_SERVER_ERROR = 5;
constexpr uint8_t ROOM_STATUS_PENDING = 6;
constexpr uint8_t ROOM_ADMIN_CHANGE_PASSWORD = 1;
constexpr uint8_t ROOM_ADMIN_KICK_PARTICIPANT = 2;
constexpr uint8_t ROOM_ADMIN_CLOSE_ROOM = 3;
constexpr uint8_t ROOM_ADMIN_ROTATE_MEDIA_KEY = 4;
constexpr uint8_t ROOM_ADMIN_CHANGE_ACCESS = 5;
constexpr uint8_t ROOM_CHAT_HISTORY_DONE = 1 << 0;
constexpr uint8_t ROOM_CHAT_HISTORY_TRUNCATED = 1 << 1;
constexpr size_t MAX_ROOM_STATUS_SUMMARIES = 8;

struct RoomSummaryWire {
    Bytes<64> room_id;
    Bytes<64> room_name;
    Bytes<64> room_instance_id;
    uint32_t access_epoch = 0;
    uint16_t participant_count = 0;
    uint8_t  flags = 0;
    uint8_t  access_mode = ROOM_ACCESS_OPEN;
};

struct ServerStatusRequestHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint16_t room_offset = 0;
    uint8_t  room_limit = static_cast<uint8_t>(MAX_ROOM_STATUS_SUMMARIES);
    uint8_t  reserved = 0;
};

struct ServerStatusResponseHdr : CtrlHdr {
    uint32_t request_id = 0;
    Bytes<64> server_id;
    uint16_t total_rooms = 0;
    uint16_t active_participants = 0;
    uint16_t room_offset = 0;
    uint8_t  room_count = 0;
    uint8_t  truncated = 0;
    uint8_t  token_auth_available = 0;
    uint8_t  reserved = 0;
    RoomSummaryWire rooms[MAX_ROOM_STATUS_SUMMARIES];
};

struct RoomCreateRequestHdr : CtrlHdr {
    uint32_t request_id = 0;
    Bytes<64> room_id;
    Bytes<64> room_name;
    Bytes<64> profile_id;
    Bytes<64> display_name;
    Bytes<128> password_hash;
    uint8_t  access_mode = ROOM_ACCESS_OPEN;
    uint8_t  reserved[3] = {};
};

struct RoomCreateResponseHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint8_t  status = ROOM_STATUS_OK;
    uint8_t  flags = 0;
    uint8_t  access_mode = ROOM_ACCESS_OPEN;
    uint8_t  reserved = 0;
    uint32_t access_epoch = 0;
    Bytes<64> room_id;
    Bytes<64> room_name;
    Bytes<64> room_instance_id;
    Bytes<128> admin_token;
    Bytes<512> join_token;
    Bytes<128> reason;
};

struct RoomJoinTokenRequestHdr : CtrlHdr {
    uint32_t request_id = 0;
    Bytes<64> room_id;
    Bytes<64> profile_id;
    Bytes<64> display_name;
    Bytes<128> password_hash;
};

struct RoomJoinTokenResponseHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint8_t  status = ROOM_STATUS_OK;
    uint8_t  flags = 0;
    uint8_t  access_mode = ROOM_ACCESS_OPEN;
    uint8_t  reserved = 0;
    uint32_t access_epoch = 0;
    Bytes<64> room_id;
    Bytes<64> room_name;
    Bytes<64> room_instance_id;
    Bytes<512> join_token;
    Bytes<128> reason;
};

struct RoomAdminRequestHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint8_t  command = 0;
    uint8_t  access_mode = ROOM_ACCESS_OPEN;
    uint16_t reserved16 = 0;
    uint32_t target_participant_id = 0;
    Bytes<64> room_id;
    Bytes<128> admin_token;
    Bytes<128> password_hash;
};

struct RoomAdminResponseHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint8_t  status = ROOM_STATUS_OK;
    uint8_t  flags = 0;
    uint8_t  access_mode = ROOM_ACCESS_OPEN;
    uint8_t  reserved = 0;
    uint32_t access_epoch = 0;
    uint32_t target_participant_id = 0;
    Bytes<64> room_id;
    Bytes<128> reason;
};

struct RoomChatSendHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint32_t access_epoch = 0;
    uint16_t ciphertext_bytes = 0;
    uint16_t reserved = 0;
    Bytes<64> room_id;
    Bytes<64> room_instance_id;
    Bytes<SECURE_PACKET_NONCE_BYTES> nonce;
    Bytes<ROOM_CHAT_CIPHERTEXT_MAX_BYTES> ciphertext;
};

struct RoomChatEventHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint32_t access_epoch = 0;
    uint64_t chat_sequence = 0;
    int64_t server_time_ms = 0;
    uint16_t ciphertext_bytes = 0;
    uint16_t reserved = 0;
    Bytes<64> room_id;
    Bytes<64> room_instance_id;
    Bytes<SECURE_PACKET_NONCE_BYTES> nonce;
    Bytes<ROOM_CHAT_CIPHERTEXT_MAX_BYTES> ciphertext;
};

struct RoomChatSendRejectedHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint8_t status = ROOM_STATUS_BAD_REQUEST;
    uint8_t reserved8 = 0;
    uint16_t reserved16 = 0;
    Bytes<SECURE_PACKET_NONCE_BYTES> nonce;
    Bytes<128> reason;
};

struct RoomChatHistoryRequestHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint32_t access_epoch = 0;
    uint64_t after_sequence = 0;
    Bytes<64> room_id;
    Bytes<64> room_instance_id;
};

struct RoomChatHistoryResponseHdr : CtrlHdr {
    uint32_t request_id = 0;
    uint8_t status = ROOM_STATUS_OK;
    uint8_t flags = 0;
    uint16_t reserved16 = 0;
    uint32_t access_epoch = 0;
    uint64_t chat_sequence = 0;
    int64_t server_time_ms = 0;
    uint16_t ciphertext_bytes = 0;
    uint16_t reserved = 0;
    Bytes<64> room_id;
    Bytes<64> room_instance_id;
    Bytes<SECURE_PACKET_NONCE_BYTES> nonce;
    Bytes<ROOM_CHAT_CIPHERTEXT_MAX_BYTES> ciphertext;
};

#pragma pack(pop)
