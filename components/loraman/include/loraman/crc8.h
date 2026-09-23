#pragma once
//
// CRC-8/CCITT (poly 0x07, init 0x00, no reflection, no final XOR).
//
// Used as the key-validation tag appended to every encrypted FreakWAN payload.
// Combined with the length-check rejector in Keychain::decrypt, this gives a
// 1/65536 false-positive rate per wrong key during trial-decryption — i.e.
// when the receiver cycles through keys to identify the sender, a wrong key
// is accepted only if its decrypted output happens to have:
//   (a) the expected length byte (1/256), AND
//   (b) a matching CRC-8 over [len | content] (1/256).
//
#include <cstdint>
#include <span>

namespace loraman
{
    inline uint8_t crc8(std::span<const uint8_t> data)
    {
        uint8_t crc = 0x00;
        for (uint8_t b : data)
        {
            crc ^= b;
            for (int bit = 0; bit < 8; ++bit)
            {
                if (crc & 0x80)
                    crc = (crc << 1) ^ 0x07;
                else
                    crc <<= 1;
            }
        }
        return crc;
    }

} // namespace loraman
