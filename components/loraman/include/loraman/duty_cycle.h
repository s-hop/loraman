#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace loraman
{
    class DutyCycle
    {
    public:
        struct Slot
        {
            uint32_t txtime_ms = 0;       // total TX time recorded in this slot
            uint32_t epoch = UINT32_MAX; // slot epoch tag (never-used sentinel)
        };

        DutyCycle(size_t slots_num, uint32_t slots_dur_s);

        void start_tx();
        uint32_t get_current_tx_time() const; // ms since start_tx; 0 if idle
        void end_tx();
        float get_duty_cycle() const;        // percent over the sliding window

    private:
        uint32_t get_epoch() const;
        size_t get_slot_index() const;

        static constexpr size_t MAX_SLOTS = 60;

        uint32_t slot_duration_ms_;
        size_t slot_count_;
        std::array<Slot, MAX_SLOTS> slots_;
        uint32_t tx_start_ms_;
        bool tx_active_;
    };
} // namespace loraman