#pragma once

#include "utils/error.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <string_view>

namespace loraman {

class Keychain
{
public:
    static constexpr size_t MAX_KEYS = 8;
    static constexpr size_t MAX_KEY_LEN = 256; // bumped from 32 — see note above

    struct Entry
    {
        std::array<char, 4> name{};
        std::array<uint8_t, MAX_KEY_LEN> key{};
        size_t key_len = 0;
        bool used = false;
    };

    Keychain() = default;

    // Add a key to the keychain. The key name must match the three-character
    // nickname used by the mesh configuration.
    // Returns ErrCode::KeyBufferFull if MAX_KEYS is reached, or
    // ErrCode::KeyInvalidName / ErrCode::KeyInvalidLength for bad input.
    Result<void> add_key(std::string_view name, const uint8_t *key, size_t key_len);

    size_t count() const { return count_; }
    const Entry *find(const char *name) const;

    // Encrypt `content` (content_len bytes) with the key named `key_name`,
    // starting at offset = uid % key_len.
    // Writes  xor_with_offset( [len:1][content:len][crc8:1] )  into `out`.
    // Returns the total bytes written (content_len + 2), or an error:
    //   ErrCode::KeyNotFound — no key with the given name
    //   ErrCode::CryptoBufferTooSmall — out buffer too small
    Result<size_t> encrypt(const uint8_t *content, size_t content_len,
                           const char *key_name, uint16_t uid,
                           uint8_t *out, size_t out_cap) const;

    // Trial-decrypt `enc` (enc_len bytes) by cycling all keys, each with
    // offset = uid % key_len. On success, writes the decrypted content to
    // `out`, the content length to *out_len, and the matching key's name
    // (null-terminated) to `sender_out` (must point to a char[4] buffer).
    // Returns ErrCode::CryptoMismatch if no key validates, or
    // ErrCode::CryptoBufferTooSmall if `out` is too small for the plaintext.
    Result<void> decrypt(const uint8_t *enc, size_t enc_len, uint16_t uid,
                         uint8_t *out, size_t out_cap, size_t *out_len,
                         char *sender_out) const;

private:
    std::array<Entry, MAX_KEYS> keys_{};
    size_t count_ = 0;
};

} // namespace loraman
