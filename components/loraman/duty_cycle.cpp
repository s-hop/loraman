#include "loraman/duty_cycle.h"
#include "utils/time.h"

#include <algorithm>

namespace loraman
{
    DutyCycle::DutyCycle(size_t slots_num, uint32_t slots_dur_s)
        : slot_duration_ms_(slots_dur_s* 1000UL),
          slot_count_(slots_num > 60 ? 60 : slots_num),
          tx_start_ms_(0),
          tx_active_(false)
    {
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            slots_[i].txtime_ms = 0;
            slots_[i].epoch = UINT32_MAX;
        }
    }

    uint32_t DutyCycle::get_epoch() const
    {
        return time::now_ms() / slot_duration_ms_;
    }

    size_t DutyCycle::get_slot_index() const
    {
        return static_cast<size_t>(get_epoch() % slot_count_);
    }

    void DutyCycle::start_tx()
    {
        tx_start_ms_ = time::now_ms();
        tx_active_ = true;
    }

    uint32_t DutyCycle::get_current_tx_time() const
    {
        if (!tx_active_)
            return 0;
        return time::now_ms() - tx_start_ms_;
    }

    void DutyCycle::end_tx()
    {
        if (!tx_active_)
            return;

        const int64_t txtime = get_current_tx_time();
        const uint32_t epoch = get_epoch();
        Slot &s = slots_[get_slot_index()];

        if (s.epoch != epoch)
        {
            s.epoch = epoch;
            s.txtime_ms = 0;
        }
        s.txtime_ms += txtime;

        tx_active_ = false;
        tx_start_ms_ = 0;
    }

    float DutyCycle::get_duty_cycle() const
    {
        const uint32_t now_ms = time::now_ms();

        const uint32_t full_window_ms = slot_duration_ms_ * slot_count_;
        const uint32_t window_ms = std::min(now_ms, full_window_ms);
        if (window_ms <= 0)
            return 0.0f;

        const uint32_t epoch = get_epoch();
        uint32_t txtime = 0;

        for (size_t i = 0; i < slot_count_; ++i)
        {
            const Slot &s = slots_[i];
            if (s.epoch == UINT32_MAX)
                continue; // never touched

            const uint32_t age = epoch - s.epoch;
            if (age > uint32_t(slot_count_))
                continue; // too old

            txtime += s.txtime_ms;
        }

        if (txtime <= 0)
            return 0.0f;

        return (static_cast<float>(txtime) / static_cast<float>(window_ms)) * 100.0f;
    }
} // namespace loraman