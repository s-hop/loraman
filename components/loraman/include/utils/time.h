#pragma once

#include <esp_timer.h>

#include <cstdint>

namespace loraman :: time
{
    inline uint32_t now_ms()
    {
        return static_cast<uint32_t> (esp_timer_get_time() / 1000u);
    }

    inline int32_t ticks_diff(uint32_t later, uint32_t earlier)
    {
        return later - earlier;
    }
}