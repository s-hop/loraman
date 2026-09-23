#pragma once

#include <esp_random.h>

#include <cstdint>

namespace loraman::random
{
    inline uint32_t random()
    {
        return esp_random();
    }

    inline uint16_t random_u16()
    {
        return static_cast<uint16_t> (esp_random());
    }

    inline uint32_t random(uint32_t max)
    {
        if (max == 0)
            return 0;
        return esp_random() % max;
    }

    inline uint32_t random(uint32_t min, uint32_t max)
    {
        if (min >= max)
            return min;
        uint32_t range = max - min + 1;
        uint32_t rand_val = esp_random();
        return min + (rand_val % range);
    }
}