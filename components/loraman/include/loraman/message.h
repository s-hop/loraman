#pragma once
// Wire format (little-endian):
//
//   byte 0      : type(3 bits) | flags(5 bits)
//                 type  = MSG_T_DATA / MSG_T_ACK / MSG_T_HELLO
//                 flags = RELAYED, PLEASE_RELAY (3 bits spare)
//   bytes 1..2  : uid (uint16 LE) — dedup + ACK correlation
//   byte  3     : ttl (uint8)
//
//   Then an encrypted payload of variable length:
//     xor( [len:1][content:len][crc8:1] )  with the sender's key
//
//   The receiver trial-decrypts with each key; the key that produces a valid
//   length byte + CRC identifies the sender (key-as-identity). Zero on-wire
//   bytes spent on a key_id prefix.
//
#include <array>
#include <cstdint>
#include <cstddef>

namespace loraman {

class Keychain;

// ---- Protocol constants ---------------------------------------------------
constexpr uint8_t MSG_TYPE_MASK = 0x07;  // 0000 0111
constexpr uint8_t MSG_FLAGS_MASK = 0xF8; // 1111 1000

constexpr uint8_t MSG_T_DATA = 1;
constexpr uint8_t MSG_T_ACK = 2;
constexpr uint8_t MSG_T_HELLO = 4;

// Flags carried in byte 0 (high 5 bits).
constexpr uint8_t MSG_FLAG_RELAYED = 0x08;      // bit 3
constexpr uint8_t MSG_FLAG_PLEASE_RELAY = 0x10; // bit 4
// bits 5-7 spare.
// MSG_FLAG_ENCR removed — always encrypted in this design.
// MSG_FLAG_BADCRC (0x80) is virtual: never on the wire, set on the in-memory
// object to annotate a frame received with a bad LoRa CRC.
constexpr uint8_t MSG_FLAG_BADCRC = 0x80;

// Content capacity. The 1-byte length field supports up to 255, but the
// transport Packet buffer (256 bytes) minus 6 bytes overhead caps us at 250.
constexpr size_t MSG_HEADER_LEN = 4;  // type+flags, uid, ttl
constexpr size_t MSG_PAYLOAD_OVR = 2; // len byte + crc byte
constexpr size_t MSG_MAX_CONTENT = 250;

// Nickname / key-name length (3 chars, derived from the decrypting key).
constexpr size_t MSG_NICK_LEN = 3;

struct Message
{
    // --- Wire / decoded fields --------------------------------------------
    uint8_t type = MSG_T_DATA;
    uint8_t flags = 0;
    uint16_t uid = 0;
    uint8_t ttl = 0;

    // Content payload (variable-length). For DATA this is the chat/command
    // text or binary telemetry. For ACK it's [rssi_i8:1]. For HELLO [seen_u8:1].
    std::array<uint8_t, MSG_MAX_CONTENT> content{};
    size_t content_len = 0;

    // --- RX metadata (not on the wire) ------------------------------------
    int16_t rssi = 0; // transport-layer RSSI (from radio)
    float snr = 0.0f;
    char key_name[4] = {0}; // sender identity (decrypted key name)

    // --- Send-queue / dedup bookkeeping -----------------------------------
    uint32_t ctime = 0;
    uint32_t send_time = 0;
    uint8_t num_tx = 1;
    bool send_canceled = false;

    // ACK correlation: nicks that have acked a DATA we originated.
    static constexpr size_t MAX_ACKERS = 8;
    std::array<std::array<char, 4>, MAX_ACKERS> ackers{};
    uint8_t ack_count = 0;

    // Sender nick for outgoing messages (used for ACK correlation: the
    // processed-cache entry must carry our nick so an incoming ACK matches
    // "about->nick == device_name"). Not transmitted on DATA — the receiver
    // derives it from the decryption key.
    char nick[4] = {0};

    // --- API --------------------------------------------------------------
    void clear();
    void gen_uid();
    bool has_acker(const char *nick3) const;
    bool add_acker(const char *nick3);

    // Encode into `out` (capacity `cap`). Returns total bytes written, or 0
    // on failure. The keychain must contain the key named by `nick` (which
    // for outgoing messages is the device key).
    size_t encode(const Keychain &kc, uint8_t *out, size_t cap) const;

    // Decode a received frame. Returns true on success. The keychain is
    // used to trial-decrypt the payload; on success, key_name is set to the
    // sender's key name. Returns false if no key validates (unknown sender
    // or corrupted frame).
    bool decode(const uint8_t *data, size_t len, const Keychain &kc);

    // Short human-readable summary for the log line.
    void to_log_string(char *out, size_t cap) const;
};

// Build a Message from an encoded frame. Returns true and fills `m`.
bool message_from_encoded(const uint8_t *encoded, size_t len,
                          const Keychain &kc, Message &m);

} // namespace loraman
