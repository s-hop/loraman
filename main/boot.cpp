#include "boot.h"

#include "config/config_store.h"
#include "loraman/duty_cycle.h"
#include "loraman/loraman.h"
#include "loraman/nodes.h"
#include "loraman/packet_queue.h"
#include "loraman/radio_hal.h"
#include "loraman/radio_interface.h"
#include "utils/error.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <nvs_handle.hpp>

#include <RadioLib.h>

#include <cstring>

using namespace loraman;

static constexpr const char *kTag = "[boot]";
static constexpr size_t kRxQueueCapacity = 10;
static constexpr size_t kTxQueueCapacity = 10;
static constexpr uint8_t kDutyCycleSlots = 60;
static constexpr uint8_t kDutyCycleSlotDurationSeconds = 60;
static constexpr float kFrequencyBandwidthScale = 10;

[[noreturn]] static void init_fail_fatal(const Result<void> &r, const char *phase)
{
    const auto &e = r.error();
    ESP_LOGE(kTag, "%s failed: %s:%d code=0x%04x detail=0x%04x (%s)",
        phase, e.loc.file_name(), e.loc.line(),
        unsigned(e.code), e.detail, esp_err_to_name(e.detail));
    esp_system_abort(phase);
}

static Result<void> load_keychain(Keychain &keychain)
{
    esp_err_t status = ESP_OK;
    std::unique_ptr<nvs::NVSHandle> nvs_keychain =
        nvs::open_nvs_handle("ns_keychain", NVS_READONLY, &status);
    if (status != ESP_OK)
        return fail(ErrCode::NvsOpenFailed, status);

    ESP_LOGD(kTag, "NVS keychain opened successfully");

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", "ns_keychain", NVS_TYPE_STR, &it);
    while (err == ESP_OK)
    {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);

        char key_value[Keychain::MAX_KEY_LEN + 1]{};
        size_t required_size = sizeof(key_value);

        err = nvs_keychain->get_string(info.key, key_value, required_size);
        if (err == ESP_ERR_NVS_INVALID_LENGTH)
            return fail(ErrCode::KeyInvalidLength, required_size);
        if (err != ESP_OK)
            return fail(ErrCode::NvsReadFailed, err);

        if (auto r = keychain.add_key(
                info.key,
                reinterpret_cast<const uint8_t *>(key_value),
                strlen(key_value));
            !r.has_value())
        {
            const auto &error = r.error();
            ESP_LOGW(kTag, "Skipping key '%s': code=0x%04x detail=%04x",
                     info.key, unsigned(error.code), error.detail);
        }

        nvs_entry_next(&it);
        if (it == nullptr)
            break;
    }
    nvs_release_iterator(it);
    return ok();
}

static Result<void> load_mesh_config(const Keychain &keychain)
{
    esp_err_t status = ESP_OK;
    std::unique_ptr<nvs::NVSHandle> nvs_mesh =
        nvs::open_nvs_handle("ns_mesh", NVS_READONLY, &status);
    if (status != ESP_OK)
        return fail(ErrCode::NvsOpenFailed, status);

    ESP_LOGD(kTag, "NVS mesh configuration opened successfully");

    auto nvs_get_or_fail = [&](const char *key, auto &out) -> Result<void> {
        status = nvs_mesh->get_item(key, out);
        if (status != ESP_OK)
            return fail(ErrCode::NvsReadFailed, status);
        return ok();
    };

    auto nvs_get_string_or_fail = [&](const char *key, char *out,
                                      size_t out_size) -> Result<void> {
        status = nvs_mesh->get_string(key, out, out_size);
        if (status != ESP_OK)
            return fail(ErrCode::NvsReadFailed, status);
        return ok();
    };

    if (auto r = nvs_get_string_or_fail("nick", config::mesh_cfg.nick.data(),
                                        config::mesh_cfg.nick.size());
        !r.has_value())
        return r;
    if (!keychain.find(config::mesh_cfg.nick.data()))
        return fail(ErrCode::KeyNotFound);

    if (auto r = nvs_get_or_fail("dc_limit", config::mesh_cfg.duty_cycle_limit);
        !r.has_value())
        return r;
    uint8_t auto_msg{};
    if (auto r = nvs_get_or_fail("auto", auto_msg); !r.has_value())
        return r;
    config::mesh_cfg.automsg = (auto_msg != 0);
    if (auto r = nvs_get_or_fail("auto_delay_min", config::mesh_cfg.automsg_delay_min);
        !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("auto_delay_max", config::mesh_cfg.automsg_delay_max);
        !r.has_value())
        return r;
    uint8_t hello_msg{};
    if (auto r = nvs_get_or_fail("hello?", hello_msg); !r.has_value())
        return r;
    config::mesh_cfg.hello = (hello_msg != 0);
    if (auto r = nvs_get_or_fail("hello_delay_min", config::mesh_cfg.hello_delay_min);
        !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("hello_delay_max", config::mesh_cfg.hello_delay_max);
        !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("flush_threshold", config::mesh_cfg.node_flush_threshold);
        !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("flush_check", config::mesh_cfg.node_flush_interval);
        !r.has_value())
        return r;
    uint8_t acks{};
    if (auto r = nvs_get_or_fail("acks", acks); !r.has_value())
        return r;
    config::mesh_cfg.acks = (acks != 0);
    if (auto r = nvs_get_or_fail("ack_delay_max", config::mesh_cfg.ack_delay_max);
        !r.has_value())
        return r;
    uint8_t relays{};
    if (auto r = nvs_get_or_fail("relays", relays); !r.has_value())
        return r;
    config::mesh_cfg.relays = (relays != 0);
    if (auto r = nvs_get_or_fail("relay_delay_max", config::mesh_cfg.relay_delay_max);
        !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("relay_tx_count", config::mesh_cfg.relay_num_tx);
        !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("relay_rssi_max", config::mesh_cfg.relay_rssi_limit);
        !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("ttl", config::mesh_cfg.ttl); !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("tx_led", config::mesh_cfg.tx_status_led);
        !r.has_value())
        return r;

    return ok();
}

static Result<void> load_radio_config(ConfigLoRa_t &radiolib_radio_config)
{
    esp_err_t status = ESP_OK;
    std::unique_ptr<nvs::NVSHandle> nvs_radio =
        nvs::open_nvs_handle("ns_radio", NVS_READONLY, &status);
    if (status != ESP_OK)
        return fail(ErrCode::NvsOpenFailed, status);

    ESP_LOGD(kTag, "NVS radio configuration opened successfully");

    struct NvsRadioConfig {
        uint16_t frequency{};
        uint16_t bandwidth{};
        uint8_t spreading_factor{};
        uint8_t coding_rate{};
        int8_t power{};
    } nvs_radio_config{};

    auto nvs_get_or_fail = [&](const char *key, auto &out) -> Result<void> {
        status = nvs_radio->get_item(key, out);
        if (status != ESP_OK)
            return fail(ErrCode::NvsReadFailed, status);
        return ok();
    };

    if (auto r = nvs_get_or_fail("fr", nvs_radio_config.frequency); !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("bw", nvs_radio_config.bandwidth); !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("sf", nvs_radio_config.spreading_factor); !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("cr", nvs_radio_config.coding_rate); !r.has_value())
        return r;
    if (auto r = nvs_get_or_fail("pw", nvs_radio_config.power); !r.has_value())
        return r;

    radiolib_radio_config = ConfigLoRa_t{
        .frequency       = nvs_radio_config.frequency       / kFrequencyBandwidthScale,
        .bandwidth       = nvs_radio_config.bandwidth       / kFrequencyBandwidthScale,
        .spreadingFactor = nvs_radio_config.spreading_factor,
        .codingRate      = nvs_radio_config.coding_rate,
        .power           = nvs_radio_config.power,
    };

    return ok();
}

static Result<void> init_radio(RadioInterface<LR1121> &radio_interface)
{
    return radio_interface.begin();
}

void loraman::boot()
{
    ESP_LOGD(kTag, "Starting device initialisation...");

    ESP_LOGD(kTag, "Initialising NVS...");
    esp_err_t status = nvs_flash_init();
    if (status == ESP_ERR_NVS_NO_FREE_PAGES || status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "NVS partition was truncated, erasing and reinitialising...");

        status = nvs_flash_erase();
        if (status != ESP_OK)
            init_fail_fatal(fail(ErrCode::NvsInitFailed, status), "nvs_flash_erase()");

        status = nvs_flash_init();
        if (status != ESP_OK)
            init_fail_fatal(fail(ErrCode::NvsInitFailed, status), "nvs_flash_init()");
    }

    if (status != ESP_OK)
        init_fail_fatal(fail(ErrCode::NvsInitFailed, status), "nvs_flash_init()");

    ESP_LOGD(kTag, "NVS initialised successfully");

    static Keychain keychain;
    if (auto r = load_keychain(keychain); !r.has_value())
        init_fail_fatal(r, "load_keychain()");

    if (auto r = load_mesh_config(keychain); !r.has_value())
        init_fail_fatal(r, "load_mesh_config()");

    // TODO: Create pin layout headers for each supported board and use them here instead of hardcoding pin numbers.
    static EspHal radio_hal(6, 5, 4);
    static Module radio_mod(&radio_hal, 7, 1, 2, 3);
    static LR1121 radio(&radio_mod);

    ESP_LOGD(kTag, "Initialising radio module...");
    ConfigLoRa_t radiolib_radio_config{};
    if (auto r = load_radio_config(radiolib_radio_config); !r.has_value())
        init_fail_fatal(r, "load_radio_config()");

    static Packet rxStore[kRxQueueCapacity];
    static Packet txStore[kTxQueueCapacity];
    static PacketQueue rx_queue(rxStore, std::size(rxStore));
    static PacketQueue tx_queue(txStore, std::size(txStore));

    static DutyCycle duty_cycle(kDutyCycleSlots, kDutyCycleSlotDurationSeconds);

    static Nodes nodes;
    static LoRaMAN mesh(rx_queue, tx_queue, keychain, nodes, duty_cycle);

    static RadioInterface radio_interface(radio, radio_hal, radiolib_radio_config, rx_queue, tx_queue, duty_cycle);

    if (auto r = init_radio(radio_interface); !r.has_value())
        init_fail_fatal(r, "init_radio()");

    ESP_LOGD(kTag, "Radio module initialised successfully");
    ESP_LOGD(kTag, "Device initialisation complete. Entering main loop...");

    for (;;)
    {
        radio_interface.poll();
        mesh.poll();
        vTaskDelay(1);
    }
}
