#pragma once

#include "packet.h"
#include "utils/error.h"

#include <cstddef>
#include <cstdint>

namespace loraman
{

    class PacketQueue
    {
        Packet *buffer_;
        size_t capacity_;
        size_t head_ = 0;
        size_t tail_ = 0;
        size_t count_ = 0;

    public:
        PacketQueue(Packet *buf, size_t capacity) : buffer_(buf), capacity_(capacity) {}

        Result<void> push(const Packet &pkt);
        Result<void> push(const uint8_t *data, size_t len);
        Result<void> pop(Packet &out);
        bool empty() const;
        bool available() const;
        // bool pending() const; Same as !empty() probably remove this
    };

} // namespace loraman