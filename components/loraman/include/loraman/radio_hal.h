/*
 * Modified RadioLib ESP-IDF HAL example.
 *
 * This file is a customized, target-aware version of the RadioLib ESP-IDF HAL
 * example. It keeps the RadioLib API contract but replaces the original
 * ESP32-only raw register implementation with the standard ESP-IDF GPIO and SPI
 * driver APIs so the same HAL can be reused across ESP32-family targets.
 */

#ifndef ESP_HAL_H
#define ESP_HAL_H

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <RadioLib.h>

#include <cstring>

#if !(CONFIG_IDF_TARGET_ESP32C3)
  #error "Target not supported."
#endif

#define LOW     (0x0)
#define HIGH    (0x1)
#define INPUT   (0x01)
#define OUTPUT  (0x03)
#define RISING  (0x01)
#define FALLING (0x02)

class EspHal : public RadioLibHal {
  public:
    EspHal(int8_t sck, int8_t miso, int8_t mosi)
      : RadioLibHal(INPUT, OUTPUT, LOW, HIGH, RISING, FALLING),
        spi_sck_(sck), spi_miso_(miso), spi_mosi_(mosi), spi_handle_(nullptr) {
    }

    void init() override {
      spiBegin();
    }

    void term() override {
      spiEnd();
    }

    void pinMode(uint32_t pin, uint32_t mode) override {
      if (pin == RADIOLIB_NC) {
        return;
      }

      gpio_set_direction((gpio_num_t)pin,
                         (mode == OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    }

    void digitalWrite(uint32_t pin, uint32_t value) override {
      if (pin == RADIOLIB_NC) {
        return;
      }

      gpio_set_level((gpio_num_t)pin, value);
    }

    uint32_t digitalRead(uint32_t pin) override {
      if (pin == RADIOLIB_NC) {
        return 0;
      }

      return gpio_get_level((gpio_num_t)pin);
    }

    void attachInterrupt(uint32_t interrupt_num, void (*interrupt_callback)(void), uint32_t mode) override {
      if (interrupt_num == RADIOLIB_NC) {
        return;
      }

      ESP_LOGD(tag_, "attachInterrupt(pin=%lu, mode=%lu)", (unsigned long)interrupt_num, (unsigned long)mode);

      static bool service_installed = false;
      if (!service_installed) {
        esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
          service_installed = true;
        } else {
          ESP_LOGD(tag_, "gpio_install_isr_service: %s", esp_err_to_name(err));
          return;
        }
      }

      gpio_set_intr_type((gpio_num_t)interrupt_num,
                        (mode == RISING) ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE);

      // gpio_isr_t is void(*)(void*); RadioLib hands us void(*)(), so bounce
      // through a trampoline instead of casting the function pointer.
      esp_err_t err = gpio_isr_handler_add((gpio_num_t)interrupt_num,
                                          &EspHal::isrTrampoline,
                                          reinterpret_cast<void *>(interrupt_callback));
      if (err != ESP_OK) {
        ESP_LOGD(tag_, "gpio_isr_handler_add: %s", esp_err_to_name(err));
      }
    }

    void detachInterrupt(uint32_t interrupt_num) override {
      if (interrupt_num == RADIOLIB_NC) {
        return;
      }
      gpio_isr_handler_remove((gpio_num_t)interrupt_num);
      gpio_set_intr_type((gpio_num_t)interrupt_num, GPIO_INTR_DISABLE);
    }

    void delay(RadioLibTime_t ms) override {
      vTaskDelay(pdMS_TO_TICKS(ms));
    }

    void delayMicroseconds(RadioLibTime_t us) override {
      esp_rom_delay_us(us);
    }

    RadioLibTime_t millis() override {
      return static_cast<RadioLibTime_t>(esp_timer_get_time() / 1000ULL);
    }

    RadioLibTime_t micros() override {
      return static_cast<RadioLibTime_t>(esp_timer_get_time());
    }

    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override {
      if (pin == RADIOLIB_NC) {
        return 0;
      }

      uint64_t start = esp_timer_get_time();
      while (digitalRead(pin) == state) {
        if ((esp_timer_get_time() - start) > timeout) {
          return 0;
        }
      }

      return static_cast<long>(esp_timer_get_time() - start);
    }

    void spiBegin() override {
      if (spi_handle_ != nullptr) {
        return;
      }

      spi_bus_config_t buscfg;
      memset(&buscfg, 0, sizeof(buscfg));
      buscfg.mosi_io_num = spi_mosi_;
      buscfg.miso_io_num = spi_miso_;
      buscfg.sclk_io_num = spi_sck_;
      buscfg.quadwp_io_num = -1;
      buscfg.quadhd_io_num = -1;
      buscfg.max_transfer_sz = 4096;

      spi_device_interface_config_t devcfg;
      memset(&devcfg, 0, sizeof(devcfg));
      devcfg.command_bits = 0;
      devcfg.address_bits = 0;
      devcfg.dummy_bits = 0;
      devcfg.mode = 0;
      devcfg.duty_cycle_pos = 0;
      devcfg.cs_ena_pretrans = 0;
      devcfg.cs_ena_posttrans = 0;
      devcfg.clock_speed_hz = 2 * 1000 * 1000;
      devcfg.input_delay_ns = 0;
      devcfg.spics_io_num = -1;
      devcfg.flags = 0;
      devcfg.queue_size = 1;
      devcfg.pre_cb = nullptr;
      devcfg.post_cb = nullptr;

      ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
      ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle_));
    }

    void spiBeginTransaction() override {
    }

    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override {
      if (spi_handle_ == nullptr) {
        return;
      }

      spi_transaction_t transaction = {};
      transaction.length = len * 8;
      transaction.tx_buffer = out;
      transaction.rx_buffer = in;
      ESP_ERROR_CHECK(spi_device_polling_transmit(spi_handle_, &transaction));
    }

    void spiEndTransaction() override {
    }

    void spiEnd() override {
      if (spi_handle_ != nullptr) {
        spi_bus_remove_device(spi_handle_);
        spi_handle_ = nullptr;
        spi_bus_free(SPI2_HOST);
      }
    }

  private:
    int8_t spi_sck_;
    int8_t spi_miso_;
    int8_t spi_mosi_;
    spi_device_handle_t spi_handle_;

    const char *tag_ = "[radio_hal]";

    static void IRAM_ATTR isrTrampoline(void *arg) {
      auto cb = reinterpret_cast<void (*)()>(reinterpret_cast<uintptr_t>(arg));
      cb();
    }
};

#endif
