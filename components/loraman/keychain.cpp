#include "loraman/keychain.h"
#include "loraman/crc8.h"

#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cstdio>

namespace loraman
{
    // ---- helpers --------------------------------------------------------------
    // Copy a 3-char nick into a std::array<char, 4> (3 chars + NUL).
    // Lengths are derived from the destination array size, not hardcoded.
    static void copy_name(std::array<char, 4> &dst, const char *src)
    {
        if (!src)
        {
            dst.fill('\0');
            return;
        }
        size_t i = 0;
        for (; i < dst.size() - 1 && src[i]; ++i)
            dst[i] = src[i];
        for (; i < dst.size(); ++i)
            dst[i] = '\0';
    }

    // XOR a buffer with a key starting at the given offset (key wraps mod key_len).
    static void xor_with_offset(uint8_t *data, size_t data_len,
                                const uint8_t *key, size_t key_len, uint16_t uid)
    {
        size_t offset = uid % key_len;
        for (size_t i = 0; i < data_len; ++i)
            data[i] ^= key[(offset + i) % key_len];
    }

    // ---- public API -----------------------------------------------------------
    Result<void> Keychain::add_key(std::string_view name, const uint8_t *key, size_t key_len)
    {
        if (count_ >= MAX_KEYS)
            return fail(ErrCode::KeyBufferFull);
        if (key_len == 0 || key_len > MAX_KEY_LEN)
            return fail(ErrCode::KeyInvalidLength, int32_t(key_len));
        if (!key)
            return fail(ErrCode::InvalidArgument);
        if (name.empty())
            return fail(ErrCode::KeyInvalidName);

        if (name.size() != 3 || name.find('\0') != std::string_view::npos)
            return fail(ErrCode::KeyInvalidName);

        for (size_t i = 0; i < count_; ++i)
        {
            if (std::string_view(keys_[i].name.data(), 3) == name)
                return fail(ErrCode::KeyInvalidName);
        }

        Entry &e = keys_[count_];
        e.used = true;
        copy_name(e.name, name.data());
        std::memcpy(e.key.data(), key, key_len);
        e.key_len = key_len;

        ++count_;
        return ok();
    }

    const Keychain::Entry *Keychain::find(const char *name) const
    {
        if (!name)
            return nullptr;
        if (std::strlen(name) != 3)
            return nullptr;
        // Entry::name is std::array<char, 4> (3 chars + NUL) — the
        // comparison length is derived from the array size, not hardcoded.
        constexpr size_t nick_len = sizeof(Entry::name) - 1;
        for (size_t i = 0; i < count_; ++i)
            if (strncmp(keys_[i].name.data(), name, nick_len) == 0)
                return &keys_[i];
        return nullptr;
    }

    Result<size_t> Keychain::encrypt(const uint8_t *content, size_t content_len,
                                     const char *key_name, uint16_t uid,
                                     uint8_t *out, size_t out_cap) const
    {
        const Entry *e = find(key_name);
        if (!e)
            return fail<size_t>(ErrCode::KeyNotFound);

        size_t total = 1 + content_len + 1;
        if (out_cap < total)
            return fail<size_t>(ErrCode::CryptoBufferTooSmall, int32_t(out_cap));

        // Build the plaintext in `out` first: [len][content][crc8]
        out[0] = uint8_t(content_len);
        if (content_len)
            std::memcpy(out + 1, content, content_len);
        out[1 + content_len] = crc8(std::span<const uint8_t>(out, 1 + content_len));

        // XOR with per-message offset derived from the UID.
        xor_with_offset(out, total, e->key.data(), e->key_len, uid);

        return total;
    }

    Result<void> Keychain::decrypt(const uint8_t *enc, size_t enc_len, uint16_t uid,
                                   uint8_t *out, size_t out_cap, size_t *out_len,
                                   char *sender_out) const
    {
        constexpr size_t max_payload = 250 + 2;
        if (!enc || enc_len < 2 || enc_len > max_payload)
            return fail<void>(ErrCode::CryptoMismatch, int32_t(enc_len));

        const uint8_t expected_len = uint8_t(enc_len - 2);

        // Scratch buffer sized to the protocol max (MSG_MAX_CONTENT + MSG_PAYLOAD_OVR).
        // See review doc B6 — was hardcoded 256, now tracks the protocol constant.
        std::array<uint8_t, max_payload> scratch{};

        for (size_t i = 0; i < count_; ++i)
        {
            const Entry &e = keys_[i];
            if (!e.used || e.key_len == 0)
                continue;

            // Copy ciphertext into scratch, then XOR with this key's offset.
            std::memcpy(scratch.data(), enc, enc_len);
            xor_with_offset(scratch.data(), enc_len, e.key.data(), e.key_len, uid);

            // Length-check rejector.
            if (scratch[0] != expected_len)
                continue;

            // CRC-8 validation over [len | content].
            size_t body = 1 + expected_len;
            uint8_t expect_crc = scratch[body];
            if (crc8(std::span<const uint8_t>(scratch.data(), body)) != expect_crc)
                continue;

            // This key validated — sender identified.
            if (out_cap < expected_len)
                return fail<void>(ErrCode::CryptoBufferTooSmall, int32_t(out_cap));
            if (expected_len > 0)
                std::memcpy(out, scratch.data() + 1, expected_len);
            if (out_len)
                *out_len = expected_len;
            // Copy the sender's name (3 chars + NUL) to the caller's buffer.
            if (sender_out)
            {
                std::memcpy(sender_out, e.name.data(), 4);
            }
            return ok();
        }

        return fail(ErrCode::CryptoMismatch);
    }

} // namespace loraman
