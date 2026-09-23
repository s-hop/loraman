#pragma once

#include "radio_init.h"
#include "duty_cycle.h"
#include "utils/time.h"

#include <esp_attr.h>   
#include <esp_log.h>
#include <RadioLib.h>

#include <atomic>
#include <concepts>
#include <utility>

namespace loraman
{

  // Concept constraining the RadioType template parameter. Any RadioLib radio
  // class (SX1280, LR1121, etc.) satisfies this concept.
  template <typename T>
  concept Radio = requires(T &radio, uint8_t *data, size_t len) {
    { radio.begin(std::declval<const ConfigLoRa_t &>()) } -> std::convertible_to<int>;
    { radio.startReceive() } -> std::convertible_to<int>;
    { radio.startTransmit(data, len) } -> std::convertible_to<int>;
    { radio.getIrqFlags() } -> std::convertible_to<uint16_t>;
    { radio.readData(data, len) } -> std::convertible_to<int>;
    { radio.getRSSI() } -> std::convertible_to<float>;
    { radio.getSNR() } -> std::convertible_to<float>;
    { radio.getPacketLength() } -> std::convertible_to<size_t>;
    { radio.finishTransmit() } -> std::convertible_to<int>;
    { radio.finishReceive() } -> std::convertible_to<int>;
  };

  // RadioInterface is the ONLY code that touches the radio hardware. It runs an
  // IRQ-driven state machine in poll():
  //
  //   idle  ──no IRQ + tx_queue_.pending()──►  startTransmit(data,len)  (radio TXing)
  //   TXing ──TX_DONE IRQ──►  finishTransmit + startReceive            (radio RXing)
  //   RXing ──RX_DONE IRQ──►  readData → push to rx_queue_ → startReceive
  //
  // The app layer (LoRaMAN) never calls startTransmit directly; it pushes encoded
  // frames onto tx_queue_ and lets this class pump them out when the radio is
  // idle.
  //
  // Duty-cycle timing is measured here, at the real startTransmit/TX_DONE
  // boundary, so it reflects actual airtime rather than queue-push time.
  template <Radio RadioType>
  class RadioInterface
  {
  private:
    RadioType &radio_;
    RadioLibHal &hal_;
    ConfigLoRa_t &lora_config_;
    PacketQueue &rx_queue_;
    PacketQueue &tx_queue_;
    DutyCycle &duty_;

    // TODO: Conditionally set these masks based on the radio type
    uint32_t tx_done_mask_ = RADIOLIB_LR11X0_IRQ_TX_DONE;
    uint32_t rx_done_mask_ = RADIOLIB_LR11X0_IRQ_RX_DONE;

    static constexpr const char *kTag = "[irad]";

    // IRQ latch: set by the DIO1 ISR, cleared (check-and-exchange) in poll().
    inline static std::atomic<bool> radio_irq{false};

    // True between startTransmit() and the TX_DONE IRQ. Used to avoid starting
    // a second transmission while one is in flight, and by the TX watchdog.
    bool tx_in_progress_ = false;
    uint32_t tx_start_ms_ = 0;
    Packet packet_in_progress_{};

    static void IRAM_ATTR on_radio_irq(void);

  public:
    RadioInterface(RadioType &radio, RadioLibHal &hal,
                   ConfigLoRa_t &lora_config, PacketQueue &rx_queue,
                   PacketQueue &tx_queue, DutyCycle &duty)
        : radio_(radio),
          hal_(hal),
          lora_config_(lora_config),
          rx_queue_(rx_queue),
          tx_queue_(tx_queue),
          duty_(duty) {}

    Result<void> begin();
    void poll();
  };

  template <Radio RadioType>
  void IRAM_ATTR RadioInterface<RadioType>::on_radio_irq(void)
  {
    radio_irq.store(true, std::memory_order_release);
  }

  template <Radio RadioType>
  Result<void> RadioInterface<RadioType>::begin()
  {
    int state = beginRadio(radio_, lora_config_);
    if (state != RADIOLIB_ERR_NONE)
    {
      return fail(ErrCode::RadioInitFailed, state);
    }

    radio_.setPacketReceivedAction(on_radio_irq);
    radio_.setPacketSentAction(on_radio_irq);

    state = radio_.setRxBoostedGainMode(true);
        if (state != RADIOLIB_ERR_NONE)
    {
      return fail(ErrCode::RadioInitFailed, state);
    }

    state = radio_.startReceive();
    if (state != RADIOLIB_ERR_NONE)
    {
      return fail(ErrCode::RadioInitFailed, state);
    }

    return ok();
  }

  // TODO: Finish implementing IRQ paths
  template <Radio RadioType>
  void RadioInterface<RadioType>::poll()
  {
    // --- IRQ-driven path: a TX_DONE or RX_DONE fired ----------------------
    if (radio_irq.exchange(false, std::memory_order_acq_rel))
    {
      uint32_t irq_flags = radio_.getIrqFlags();
      ESP_LOGD(kTag, "-- IRQ flags: 0x%04x", static_cast<unsigned int>(irq_flags));

      if (tx_in_progress_) {
        radio_.finishTransmit();
        duty_.end_tx();
        tx_in_progress_ = false;
        packet_in_progress_ = {};
        tx_start_ms_ = 0;
        radio_.startReceive();
        return;
      }

      else if (irq_flags & rx_done_mask_)
      {
        size_t len = radio_.getPacketLength();
        ESP_LOGI(kTag, "<< %zuB", len);

        Packet pkt{};
        if (len > sizeof(pkt.data))
        {
          len = sizeof(pkt.data);
        }

        int state = radio_.readData(pkt.data, len);

        if (state == RADIOLIB_ERR_NONE)
        {
          pkt.len = len;
          pkt.rssi = radio_.getRSSI();
          pkt.snr = radio_.getSNR();

          if (auto r = rx_queue_.push(pkt); !r.has_value())
          {
            // Queue full; drop and log.
            ESP_LOGW(kTag, "!! RX queue full, dropping packet (code=%u)",
                     unsigned(r.error().code));
          }
        }
        else
        {
          if (state == RADIOLIB_ERR_CRC_MISMATCH)
          {
            ESP_LOGW(kTag, "!! RX CRC error!");
          }
          else
          {
            ESP_LOGE(kTag, "!! RX failed, code %d", state);
          }
        }
        radio_.finishReceive();
        radio_.startReceive();
      }
      else
      {
        ESP_LOGW(kTag, "!! IRQ fired but no TX_DONE or RX_DONE flags set");
      }

      return; // IRQ handling is the priority; TX draining waits for next poll()
    }

    // --- TX watchdog -------------------------------------------------------
    if (tx_in_progress_)
    {
      // If the radio has been "transmitting" far longer than any frame could
      // take, it's stuck — reinitialize it.
      RadioLibTime_t max_time_on_air_ms = radio_.getTimeOnAir(packet_in_progress_.len) * 3UL / 1000UL;
      if (time::now_ms() - tx_start_ms_ > max_time_on_air_ms)
      {
        ESP_LOGE(kTag, "!! TX watchdog reset (stuck for %d ms)", max_time_on_air_ms);
        duty_.end_tx();

        beginRadio(radio_, lora_config_);
        radio_.setPacketReceivedAction(on_radio_irq);
        radio_.setPacketSentAction(on_radio_irq);
        radio_.startReceive();
        tx_in_progress_ = false;
        tx_start_ms_ = 0;
        packet_in_progress_ = {};
        
      }
      return;
    }

    // --- Idle path: nothing happened, so maybe start a pending TX ---------
    if (tx_queue_.empty())
      return;

    // Pop one frame and start transmitting it. Binary-safe: we pass the
    // explicit length, so a 0x00 inside the frame is preserved.
    Packet pkt;
    if (auto r = tx_queue_.pop(pkt); !r.has_value())
      return; // Queue empty — normal condition, not a failure

    packet_in_progress_ = pkt;
    ESP_LOGI(kTag, ">> %zuB", pkt.len);
    int state = radio_.startTransmit(pkt.data, pkt.len);
    if (state != RADIOLIB_ERR_NONE)
    {
      ESP_LOGE(kTag, "!! startTransmit failed, code %d", state);
      // Leave the packet dropped; the radio is still in receive mode.
      return;
    }

    tx_in_progress_ = true;
    tx_start_ms_ = time::now_ms();
    duty_.start_tx();
  }

} // namespace loraman