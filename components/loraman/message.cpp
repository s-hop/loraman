#include "loraman/message.h"
#include "loraman/keychain.h"
#include "utils/time.h"
#include "utils/random.h"

#include <cstdio>
#include <cstring>

namespace loraman
{

    // ---- small helpers --------------------------------------------------------
    static void pack_u16_le(uint8_t *p, uint16_t v)
    {
        p[0] = uint8_t(v & 0xFF);
        p[1] = uint8_t((v >> 8) & 0xFF);
    }

    static uint16_t unpack_u16_le(const uint8_t *p)
    {
        return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
    }

    // Copy a 3-char nick into a char[4] (3 chars + NUL). Length derived
    // from MSG_NICK_LEN rather than hardcoded. Matches the std::array<char,4>
    // convention used by config::mesh_cfg.nick.
    static void copy_nick3(char dst[4], const char *src)
    {
        for (size_t i = 0; i < MSG_NICK_LEN; ++i)
            dst[i] = (src && src[i]) ? src[i] : '\0';
        dst[MSG_NICK_LEN] = '\0';
    }

    // ---- Message API ----------------------------------------------------------
    void Message::clear()
    {
        type = MSG_T_DATA;
        flags = 0;
        uid = 0;
        ttl = 0;
        content_len = 0;
        content.fill(0);
        rssi = 0;
        snr = 0.0f;
        std::memset(key_name, 0, sizeof(key_name));
        std::memset(nick, 0, sizeof(nick));
        ctime = 0;
        send_time = 0;
        num_tx = 1;
        send_canceled = false;
        ack_count = 0;
        for (auto &a : ackers)
            a.fill('\0');
    }

    void Message::gen_uid()
    {
        uid = random::random_u16();
    }

    bool Message::has_acker(const char *nick3) const
    {
        for (uint8_t i = 0; i < ack_count; ++i)
            if (std::strncmp(ackers[i].data(), nick3, MSG_NICK_LEN) == 0)
                return true;
        return false;
    }

    bool Message::add_acker(const char *nick3)
    {
        if (has_acker(nick3))
            return true;
        if (ack_count >= MAX_ACKERS)
            return false;
        copy_nick3(ackers[ack_count].data(), nick3);
        ++ack_count;
        return true;
    }

    size_t Message::encode(const Keychain &kc, uint8_t *out, size_t cap) const
    {
        if (!out || cap < MSG_HEADER_LEN)
            return 0;
        if (content_len > MSG_MAX_CONTENT)
            return 0;

        //For outgoing messages the sender identity is the device key;
        const char *key_name = nick;

        // Encrypt the content: produces xor_with_offset([len][content][crc8]).
        // The UID is used as a per-message offset into the key, so two messages
        // with different UIDs use different key segments (no two-time-pad).
        std::array<uint8_t, MSG_MAX_CONTENT + MSG_PAYLOAD_OVR> payload{};
        auto enc_result = kc.encrypt(content.data(), content_len,
                                     key_name, uid,
                                     payload.data(), payload.size());
        if (!enc_result.has_value())
            return 0; // key missing or buffer too small

        size_t payload_len = enc_result.value();
        size_t total = MSG_HEADER_LEN + payload_len;
        if (cap < total)
            return 0;

        // Header
        out[0] = (type & MSG_TYPE_MASK) | (flags & MSG_FLAGS_MASK);
        pack_u16_le(out + 1, uid);
        out[3] = ttl;

        // Encrypted payload
        std::memcpy(out + MSG_HEADER_LEN, payload.data(), payload_len);
        return total;
    }

    bool Message::decode(const uint8_t *data, size_t len, const Keychain &kc)
    {
        if (len < MSG_HEADER_LEN + MSG_PAYLOAD_OVR)
            return false; // header + min payload

        const uint8_t combined = data[0];
        const uint8_t mtype = combined & MSG_TYPE_MASK;
        const uint8_t wflags = combined & MSG_FLAGS_MASK;

        const uint16_t dec_uid = unpack_u16_le(data + 1);
        const uint8_t dec_ttl = data[3];

        // Trial-decrypt the payload. The keychain cycles all keys, each with
        // offset = uid % key_len. The key that validates identifies the sender.
        const uint8_t *enc_payload = data + MSG_HEADER_LEN;
        size_t enc_payload_len = len - MSG_HEADER_LEN;

        std::array<uint8_t, MSG_MAX_CONTENT> plain{};
        size_t plain_len = 0;
        char sender[4] = {0};
        auto dec_result = kc.decrypt(enc_payload, enc_payload_len, dec_uid,
                                     plain.data(), plain.size(), &plain_len, sender);

        if (!dec_result.has_value())
        {
            // No key validated — unknown sender or corrupted frame.
            return false;
        }

        // Populate the decoded message.
        type = mtype;
        flags = wflags;
        uid = dec_uid;
        ttl = dec_ttl;

        // Sender identity from the decrypting key.
        copy_nick3(key_name, sender);
        // nick defaults to the sender identity (matches Python convention).
        std::memcpy(nick, key_name, 4);

        // Content
        content_len = (plain_len <= MSG_MAX_CONTENT) ? plain_len : MSG_MAX_CONTENT;
        if (content_len > 0)
            std::memcpy(content.data(), plain.data(), content_len);

        return true;
    }

    void Message::to_log_string(char *out, size_t cap) const
    {
        if (!out || cap == 0)
            return;
        const char *tstr =
            (type == MSG_T_DATA)  ? "data " : 
            (type == MSG_T_ACK)   ? "ack  " : 
            (type == MSG_T_HELLO) ? "hello" : "?????";

        // Render the high 5 flag bits as a 5-char binary string.
        const uint8_t fbits = (flags >> 3) & 0x1F;
        char fb[6];
        for (int i = 0; i < 5; ++i)
            fb[i] = ((fbits >> (4 - i)) & 1) ? '1' : '0';
        fb[5] = '\0';

        snprintf(out, cap, "%04x %s %s -%02udBm %.2f %u %zuB",
                 uid, tstr, fb, unsigned(-rssi), snr, ttl, content_len);
    }

    bool message_from_encoded(const uint8_t *encoded, size_t len,
                              const Keychain &kc, Message &m)
    {
        m.clear();
        m.ctime = time::now_ms();
        return m.decode(encoded, len, kc);
    }

} // namespace loraman
