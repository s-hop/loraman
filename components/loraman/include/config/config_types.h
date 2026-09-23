#pragma once

#include <array>
#include <cstdint>

namespace loraman::config {

struct RadioConfig
{
    float frequency{0.0f};
    float bandwidth{0.0f};
    uint8_t spread_factor{0};
    uint8_t coding_rate{0};
    int8_t power{0};
};

struct BatteryConfig
{
    float v_min{0.0f};
    float v_nom{0.0f};
    float adc_mult{0.0f};
    uint8_t adc_pin{0};
};

struct WifiConfig
{
    std::array<char, 32> ssid{};
    std::array<char, 32> password{};
};

struct LedConfig
{
    bool inverted{false};
    uint8_t gpio_pin{0};
};

struct DeviceConfig
{
    BatteryConfig battery{};
    WifiConfig wifi{};
    LedConfig led{};
};

struct MeshConfig
{
    std::array<char, 4> nick{};
    uint8_t duty_cycle_limit{1};
    bool automsg{false};
    uint32_t automsg_delay_min{20};
    uint32_t automsg_delay_max{40};
    bool hello{false};
    uint32_t hello_delay_min{30};
    uint32_t hello_delay_max{60};
    uint32_t node_flush_threshold{60};
    uint32_t node_flush_interval{20};
    bool acks{false};
    uint8_t ack_delay_max{3};
    bool relays{false};
    uint8_t relay_delay_max{3};
    uint8_t relay_num_tx{1};
    int8_t relay_rssi_limit{-30};
    uint8_t ttl{8};
    bool tx_status_led{true};
};

} // namespace loraman::config
