#pragma once
//
// LoRaMAN: the FreakWAN application ported from freakwan.py.
//
// All configuration is pulled from config::mesh_cfg / config::device_cfg at
// runtime — there is no separate LoRaMANConfig struct. The config structs are
// loaded at boot (see config_state.h) and are the single source of truth for
// TTL, duty-cycle limit, ACK/relay behaviour, etc.
//
// Device identity comes from config::mesh_cfg.nick — NOT from the keychain.
// The keychain is purely for crypto; the nick is the human-readable identity
// used for ACK correlation and logging.
//
#include "packet_queue.h"
#include "packet.h"
#include "message.h"
#include "keychain.h"
#include "nodes.h"
#include "duty_cycle.h"

#include <array>
#include <cstdint>
#include <cstddef>

namespace loraman
{

    class LoRaMAN
    {
    public:
        LoRaMAN(PacketQueue &rx_queue, PacketQueue &tx_queue,
                Keychain &keychain, Nodes &nodes, DutyCycle &duty);

        // --- Inbound: build a Message from user content and queue it -----------
        bool send_chat(const char *content);

        bool send_asynchronously(Message &m, uint32_t max_delay,
                                 uint8_t num_tx, bool relay);

        // --- Main cooperative loop -------------------------------------------
        void poll();

        // --- Accessors --------------------------------------------------------
        const Keychain &keychain() const { return keychain_; }
        const Nodes &nodes() const { return nodes_; }
        const DutyCycle &duty_cycle() const { return duty_; }
        size_t send_queue_len() const { return send_count_; }

        // RSSI ring (last 8 receptions) for the display sparkline.
        static constexpr size_t kRssiHistorySize = 8;
        size_t rssi_history_count() const { return rssi_count_; }
        int16_t rssi_history(size_t i) const;

    private:
        // --- wiring -----------------------------------------------------------
        PacketQueue &rx_queue_;
        PacketQueue &tx_queue_;
        Keychain &keychain_;
        Nodes &nodes_;
        DutyCycle &duty_;

        // --- app-level send queue (Messages with send_time + num_tx) ----------
        static constexpr size_t kSendQueueCapacity = 10;
        std::array<Message, kSendQueueCapacity> send_queue_{};
        size_t send_count_ = 0;

        // --- dedup + ACK correlation cache ------------------------------------
        static constexpr size_t kProcessedCacheCapacity = 48;
        static constexpr size_t kEvictionScanSize = 6;
        static constexpr uint32_t kProcessedCacheMaxAgeMs = 60000;
        struct Processed
        {
            bool used = false;
            uint16_t uid = 0;
            uint32_t ctime = 0;
            Message msg{};
        };
        std::array<Processed, kProcessedCacheCapacity> processed_{};
        size_t eviction_cursor_ = 0;

        // --- periodic task scheduling (time-checked, no asyncio) -------------
        uint32_t next_hello_ = 0;
        uint32_t next_automsg_ = 0;
        uint32_t next_flush_ = 0;
        uint32_t last_status_ = 0;
        uint32_t automsg_counter_ = 0;

        // --- RSSI history ----------------------------------------------------
        std::array<int16_t, kRssiHistorySize> rssi_history_{};
        size_t rssi_count_ = 0;

        // --- Duty Cycle limit flag -------------------------------------------
        bool duty_limited_ = false;

        // --- internal helpers ------------------------------------------------
        void drain_rx_queue();
        void handle_received_packet(const Packet &pkt);
        void send_ack_if_needed(const Message &m);
        void relay_if_needed(Message &m);
        bool mark_as_processed(const Message &m);
        Message *get_processed(uint16_t uid);
        void evict_processed_cache(uint32_t now);
        void pump_send_queue();
        void maybe_send_hello(uint32_t now);
        void maybe_send_automsg(uint32_t now);
        void maybe_flush_nodes(uint32_t now);
        void update_active_nodes(const char *nick3, int16_t rssi, uint32_t now);
        void update_rssi_history(int16_t rssi);
        bool push_encoded_to_tx(const Message &m);
        void show_status_log(uint32_t now);
    };

} // namespace loraman
