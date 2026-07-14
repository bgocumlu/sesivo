#include "audio_packet.h"
#include "session_crypto.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    (void)audio_packet::parse_audio_header(data, size);
    std::string reason;
    (void)audio_packet::validate_audio_packet_bytes(data, size, &reason);
    (void)audio_packet::validate_redundant_audio_packet_bytes(data, size, &reason);

    session_crypto::SecureAudioMetadata audio_metadata;
    uint16_t encrypted_bytes = 0;
    (void)session_crypto::parse_secure_audio_header(
        data, size, audio_metadata, encrypted_bytes, &reason);
    session_crypto::SecureControlMetadata control_metadata;
    (void)session_crypto::parse_secure_control_header(data, size, control_metadata,
                                                       encrypted_bytes, &reason);
    return 0;
}
