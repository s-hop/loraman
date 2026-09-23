#include "loraman/packet_queue.h"
#include "loraman/packet.h"

#include <cstring>

namespace loraman
{

    Result<void> PacketQueue::push(const Packet &pkt)
    {
        if (count_ == capacity_)
            return fail(ErrCode::QueueFull);

        if (pkt.len > sizeof(Packet::data))
            return fail(ErrCode::PacketOversize, int32_t(pkt.len));

        Packet &slot = buffer_[tail_];

        memcpy(slot.data, pkt.data, pkt.len);

        // Clear any remaining bytes from previous contents to avoid leaking
        // stale data when the new packet is shorter than the old one.
        if (pkt.len < sizeof(slot.data))
        {
            memset(slot.data + pkt.len, 0, sizeof(slot.data) - pkt.len);
        }

        slot.len = pkt.len;
        slot.rssi = pkt.rssi;
        slot.snr = pkt.snr;

        tail_ = (tail_ + 1) % capacity_;
        ++count_;

        return ok();
    }

    Result<void> PacketQueue::push(const uint8_t *data, size_t len)
    {
        if (len > sizeof(Packet::data))
            return fail(ErrCode::PacketOversize, int32_t(len));

        Packet pkt{};
        pkt.len = len;
        memcpy(pkt.data, data, len);

        return push(pkt);
    }

    Result<void> PacketQueue::pop(Packet &out)
    {
        if (count_ == 0)
            return fail(ErrCode::QueueEmpty);

        out = buffer_[head_];
        head_ = (head_ + 1) % capacity_;
        --count_;

        return ok();
    }

    bool PacketQueue::empty() const
    {
        return count_ == 0;
    }

    bool PacketQueue::available() const
    {
        return count_ < capacity_;
    }

    // bool PacketQueue::pending() const
    // {
    //     return count_ > 0;
    // }

} // namespace loraman