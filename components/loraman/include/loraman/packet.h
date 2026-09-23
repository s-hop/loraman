#pragma once

#include <cstdint>
#include <cstddef>

namespace loraman {

struct Packet
{
    uint8_t data[64] = {0};
    size_t len = 0;
    float rssi = 0.0f;
    float snr = 0.0f;
};

} // namespace loraman