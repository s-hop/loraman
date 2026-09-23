#pragma once

#include <RadioLib.h>

namespace loraman {

template <class T>
inline int beginRadio(T &radio, const ConfigLoRa_t &cfg)
{
    return radio.begin(cfg);
}

} // namespace loraman
