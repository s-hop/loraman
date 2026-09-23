#include "loraman/loraman.h"
#include "utils/time.h"
#include "utils/random.h"
#include "config/config_store.h"

#include <esp_log.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace loraman
{

    static constexpr uint32_t kSendMaxDelayMs = 2000;
    static constexpr uint32_t kTxAgainMinDelayMs = 3000;
    static constexpr uint32_t kTxAgainMaxDelayMs = 8000;
    static constexpr const char *kTag = "[LMAN]";

    // Convenience accessor: the device nick from config, as a const char*.
    static inline const char *device_nick()
    {
        return config::mesh_cfg.nick.data();
    }

    static inline void copy_device_nick(char dst[4])
    {
        const char *src = device_nick();
        for (size_t i = 0; i < MSG_NICK_LEN; ++i)
            dst[i] = src[i];
        dst[MSG_NICK_LEN] = '\0';
    }

    // ---------------------------------------------------------------------------
        LoRaMAN::LoRaMAN(PacketQueue &rx_queue, PacketQueue &tx_queue,
                     Keychain &keychain, Nodes &nodes, DutyCycle &duty)
                : rx_queue_(rx_queue),
                    tx_queue_(tx_queue),
          keychain_(keychain),
          nodes_(nodes),
          duty_(duty)
    {
        for (size_t i = 0; i < kRssiHistorySize; ++i)
            rssi_history_[i] = -100;
        next_hello_ = time::now_ms() + uint32_t(config::mesh_cfg.hello_delay_min) * 1000u;
        next_automsg_ = time::now_ms() + uint32_t(config::mesh_cfg.automsg_delay_min) * 1000u;
        next_flush_ = time::now_ms() + uint32_t(config::mesh_cfg.node_flush_interval) * 1000u;
    }

    // ---------------------------------------------------------------------------
    // Outbound
    // ---------------------------------------------------------------------------
    bool LoRaMAN::send_chat(const char *content)
    {
        if (duty_limited_)
            return false;

        Message m;
        m.clear();
        m.type = MSG_T_DATA;
        m.gen_uid();
        m.ttl = config::mesh_cfg.ttl;
        m.flags = 0;
        // Device identity from config (for ACK correlation).
        copy_device_nick(m.nick);
        // Copy content.
        if (content)
        {
            size_t n = 0;
            for (; n < MSG_MAX_CONTENT && content[n]; ++n)
                m.content[n] = uint8_t(content[n]);
            m.content_len = n;
        }
        
        return send_asynchronously(m, kSendMaxDelayMs, 1, /*relay=*/true);
    }

    bool LoRaMAN::send_asynchronously(Message &m, uint32_t max_delay,
                                      uint8_t num_tx, bool relay)
    {
        if (send_count_ >= kSendQueueCapacity)
            return false;

        // Safety net: if the duty-cycle limiter is active, refuse to queue.
        // The maybe_send_*, send_ack_if_needed, relay_if_needed, and
        // send_chat functions all check duty_limited_ before preparing a
        // message, so this check should never fire in normal operation.
        // It's here to catch any future code path that calls
        // send_asynchronously without pre-checking.
        if (duty_limited_)
            return false;

        uint32_t now = time::now_ms();
        m.send_time = now + random::random(0, max_delay);
        m.num_tx = num_tx;
        m.ctime = now;
        m.send_canceled = false;
        if (relay)
            m.flags |= MSG_FLAG_PLEASE_RELAY;

        send_queue_[send_count_] = m;
        ++send_count_;

        // We originated this message — register it for ACK correlation + dedup.
        mark_as_processed(m);
        return true;
    }

    // ---------------------------------------------------------------------------
    // Main cooperative loop
    // ---------------------------------------------------------------------------

    void LoRaMAN::poll()
    {
        uint32_t now = time::now_ms();

        drain_rx_queue();
        pump_send_queue();
        evict_processed_cache(now);

        maybe_send_hello(now);
        maybe_send_automsg(now);
        maybe_flush_nodes(now);

        // Status line every ~60s.
        if (time::ticks_diff(now, last_status_) >= 60000)
        {
            last_status_ = now;
            show_status_log(now);
        }
    }

    // ---------------------------------------------------------------------------
    // Inbound
    // ---------------------------------------------------------------------------

    void LoRaMAN::drain_rx_queue()
    {
        Packet pkt;
        while (rx_queue_.pop(pkt).has_value())
            handle_received_packet(pkt);
    }

    void LoRaMAN::handle_received_packet(const Packet &pkt)
    {
        Message m;
        if (!message_from_encoded(pkt.data, pkt.len, keychain_, m))
        {
            ESP_LOGE(kTag, "!! can't decode packet (%zuB) — unknown sender or corrupt", pkt.len);
            return;
        }

        m.rssi = int16_t(pkt.rssi);
        m.snr = pkt.snr;
        update_rssi_history(pkt.rssi);

        // --- DATA ------------------------------------------------------------
        if (m.type == MSG_T_DATA)
        {
            if (mark_as_processed(m))
            {
                
                ESP_LOGI(kTag, "-- ignoring duplicate msg %04x", m.uid);
                return;
            }

            if (!(m.flags & MSG_FLAG_RELAYED))
                update_active_nodes(m.nick, m.rssi, time::now_ms());

            char log[64];
            m.to_log_string(log, sizeof(log));
            ESP_LOGI(kTag, "<< %s %s %s",
                log,
                (m.flags & MSG_FLAG_RELAYED) ? " [R]" : "",
                (m.flags & MSG_FLAG_BADCRC) ? " [BADCRC]" : ""
            );

            send_ack_if_needed(m);
            relay_if_needed(m);
            return;
        }

        // --- ACK -------------------------------------------------------------
        if (m.type == MSG_T_ACK)
        {
            Message *about = get_processed(m.uid);
            if (about && strncmp(about->nick, device_nick(), MSG_NICK_LEN) == 0)
            {
                about->add_acker(m.nick);
                int8_t ack_rssi = (m.content_len >= 1) ? int8_t(m.content[0]) : 0;
                update_active_nodes(m.nick, m.rssi, time::now_ms());
                ESP_LOGI(kTag, "<< %04x ACK from %s (rx_rssi=%d)",
                    about->uid, m.nick, int(ack_rssi));

                if (nodes_.active_count() > 0 &&
                    about->ack_count >= nodes_.active_count())
                {
                    about->send_canceled = true;
                    ESP_LOGI(kTag, "-- %04x ACKs from all %zu nodes; suppressing resends",
                        about->uid, nodes_.active_count());
                }
            }
            return;
        }

        // --- HELLO -----------------------------------------------------------
        if (m.type == MSG_T_HELLO)
        {
            uint8_t sender_seen = (m.content_len >= 1) ? m.content[0] : 0;
            ESP_LOGI(kTag, "<< %04x HELLO from %s (seen=%u)", m.uid, m.nick, sender_seen);
            update_active_nodes(m.nick, m.rssi, time::now_ms());
            return;
        }

        ESP_LOGW(kTag, "!! unhandled message type %u", unsigned(m.type));
    }

    // ---------------------------------------------------------------------------
    // ACK generation
    // ---------------------------------------------------------------------------

    void LoRaMAN::send_ack_if_needed(const Message &m)
    {
        if (duty_limited_)
            return;
        if (!config::mesh_cfg.acks)
            return;
        if (m.type != MSG_T_DATA)
            return;
        if (m.flags & MSG_FLAG_RELAYED)
            return;
        if (strncmp(m.nick, device_nick(), MSG_NICK_LEN) == 0)
            return; // don't ACK our own

        Message ack;
        ack.clear();
        ack.type = MSG_T_ACK;
        ack.uid = m.uid;
        ack.ctime = time::now_ms();
        // ACK content: 1-byte rssi of the acked DATA (int8).
        int16_t r = m.rssi;
        if (r > 127)
            r = 127;
        if (r < -128)
            r = -128;
        ack.content[0] = uint8_t(int8_t(r));
        ack.content_len = 1;
        copy_device_nick(ack.nick);

        send_asynchronously(ack, config::mesh_cfg.ack_delay_max, 1, /*relay=*/false);
        ESP_LOGI(kTag, "-- %04x queue ACK from %s", m.uid, m.nick);
    }

    // ---------------------------------------------------------------------------
    // Relay logic
    // ---------------------------------------------------------------------------

    void LoRaMAN::relay_if_needed(Message &m)
    {
        if (duty_limited_)
            return;
        if (!config::mesh_cfg.relays)
            return;
        if (m.type != MSG_T_DATA)
            return;
        if (!(m.flags & MSG_FLAG_PLEASE_RELAY))
            return;

        if (m.rssi > config::mesh_cfg.relay_rssi_limit)
            return;
        if (m.ttl <= 1)
            return;

        m.ttl -= 1;
        m.flags |= MSG_FLAG_RELAYED;
        m.flags &= ~MSG_FLAG_PLEASE_RELAY;

        send_asynchronously(m, config::mesh_cfg.relay_delay_max,
                            config::mesh_cfg.relay_num_tx, /*relay=*/false);
        ESP_LOGI(kTag, "-- %04x queue RELAY from %s (ttl=%u)",
               m.uid, m.nick, unsigned(m.ttl));
    }

    // ---------------------------------------------------------------------------
    // Dedup / ACK-correlation cache
    // ---------------------------------------------------------------------------

    bool LoRaMAN::mark_as_processed(const Message &m)
    {
        if (m.type != MSG_T_DATA)
            return false;

        if (get_processed(m.uid) != nullptr)
            return true;

        size_t idx = 0;
        uint32_t oldest_age = 0;
        for (size_t i = 0; i < kProcessedCacheCapacity; ++i)
        {
            if (!processed_[i].used)
            {
                idx = i;
                break;
            }
            uint32_t age = uint32_t(time::ticks_diff(time::now_ms(),
                                                         processed_[i].ctime));
            if (age > oldest_age)
            {
                oldest_age = age;
                idx = i;
            }
        }

        processed_[idx].used = true;
        processed_[idx].uid = m.uid;
        processed_[idx].ctime = time::now_ms();
        processed_[idx].msg = m;
        return false;
    }

    Message *LoRaMAN::get_processed(uint16_t uid)
    {
        for (size_t i = 0; i < kProcessedCacheCapacity; ++i)
            if (processed_[i].used && processed_[i].uid == uid)
                return &processed_[i].msg;
        return nullptr;
    }

    void LoRaMAN::evict_processed_cache(uint32_t now)
    {
        for (size_t n = 0; n < kEvictionScanSize; ++n)
        {
            if (eviction_cursor_ >= kProcessedCacheCapacity)
                eviction_cursor_ = 0;
            Processed &p = processed_[eviction_cursor_++];
            if (!p.used)
                continue;
            uint32_t age = uint32_t(time::ticks_diff(now, p.ctime));
            if (age > kProcessedCacheMaxAgeMs)
            {
                ESP_LOGI(kTag, "-- evicting cache entry %04x (age %ums)", p.uid, age);
                p.used = false;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Outbound: pump the send queue into the transport TX queue
    // ---------------------------------------------------------------------------

    void LoRaMAN::pump_send_queue()
    {
        if (duty_.get_duty_cycle() >= float(config::mesh_cfg.duty_cycle_limit))
        {
            if (!duty_limited_)
            {
                duty_limited_ = true;

                size_t purged_send = send_count_;
                send_count_ = 0;

                size_t purged_tx = 0;
                Packet dropped;
                while (tx_queue_.pop(dropped).has_value())
                    ++purged_tx;

                ESP_LOGW(kTag, "!! duty cycle %.2f%% >= limit %u%%; purged %zu send + %zu tx",
                       double(duty_.get_duty_cycle()),
                       unsigned(config::mesh_cfg.duty_cycle_limit),
                       purged_send, purged_tx);
            }
            return;
        }

        if (duty_limited_)
        {
            duty_limited_ = false;
            ESP_LOGI(kTag, "-- duty cycle %.2f%% below limit; resuming TX",
                   duty_.get_duty_cycle());
        }

        uint32_t now = time::now_ms();
        size_t i = 0;
        while (i < send_count_)
        {
            Message &m = send_queue_[i];

            if (m.send_canceled)
            {
                send_queue_[i] = send_queue_[send_count_ - 1];
                --send_count_;
                continue;
            }

            if (time::ticks_diff(now, m.send_time) < 0)
            {
                ++i;
                continue;
            }

            if (!tx_queue_.available())
            {
                ++i;
                continue;
            }

            if (!push_encoded_to_tx(m))
            {
                m.send_canceled = true;
                ++i;
                continue;
            }

            if (m.num_tx > 1)
            {
                m.num_tx -= 1;
                m.send_time = now + random::random(kTxAgainMinDelayMs,
                                                     kTxAgainMaxDelayMs);
            }
            else
            {
                m.send_canceled = true;
            }
            ++i;
        }
    }

    bool LoRaMAN::push_encoded_to_tx(const Message &m)
    {
        std::array<uint8_t, MSG_HEADER_LEN + MSG_MAX_CONTENT + MSG_PAYLOAD_OVR> buf{};
        size_t len = m.encode(keychain_, buf.data(), buf.size());
        if (len == 0)
            return false;

        if (auto r = tx_queue_.push(buf.data(), len); !r.has_value())
            return false;

        char log[64];
        m.to_log_string(log, sizeof(log));
        ESP_LOGI(kTag, ">> %s", log);
        return true;
    }

    // ---------------------------------------------------------------------------
    // Periodic tasks
    // ---------------------------------------------------------------------------

    void LoRaMAN::maybe_send_hello(uint32_t now)
    {
        if (duty_limited_)
            return;
        if (!config::mesh_cfg.hello)
            return;
        if (time::ticks_diff(now, next_hello_) < 0)
            return;

        Message hello;
        hello.clear();
        hello.type = MSG_T_HELLO;
        hello.gen_uid();
        // HELLO content is 1 byte: the sender's known-node count.
        hello.content[0] = uint8_t(nodes_.active_count());
        hello.content_len = 1;
        copy_device_nick(hello.nick);
        hello.ctime = now;

        send_asynchronously(hello, 3000, 1, false);
        ESP_LOGI(kTag, "-- %04x queue HELLO (seen=%u)", hello.uid, hello.content[0]);

        // hello_delay_min/max are in seconds → convert to ms.
        uint32_t lo = uint32_t(config::mesh_cfg.hello_delay_min) * 1000u;
        uint32_t hi = uint32_t(config::mesh_cfg.hello_delay_max) * 1000u;
        next_hello_ = now + random::random(lo, hi);
    }

    void LoRaMAN::maybe_send_automsg(uint32_t now)
    {
        if (duty_limited_)
            return;
        if (!config::mesh_cfg.automsg)
            return;
        if (time::ticks_diff(now, next_automsg_) < 0)
            return;

        Message m;
        m.clear();
        m.type = MSG_T_DATA;
        m.gen_uid();
        m.ttl = config::mesh_cfg.ttl;
        int n = snprintf(reinterpret_cast<char *>(m.content.data()), MSG_MAX_CONTENT,
                         "%04u", unsigned(automsg_counter_));
        m.content_len = (n > 0) ? size_t(n) : 0;
        copy_device_nick(m.nick);

        send_asynchronously(m, 3000, 1, /*relay=*/true);
        ESP_LOGI(kTag, "-- %04x queue AUTO", m.uid);
        ++automsg_counter_;

        // automsg_delay_min/max are in seconds → convert to ms.
        uint32_t lo = uint32_t(config::mesh_cfg.automsg_delay_min) * 1000u;
        uint32_t hi = uint32_t(config::mesh_cfg.automsg_delay_max) * 1000u;
        next_automsg_ = now + random::random(lo, hi);
    }

    void LoRaMAN::maybe_flush_nodes(uint32_t now)
    {
        if (time::ticks_diff(now, next_flush_) < 0)
            return;
        // node_flush_interval is in seconds → convert to ms.
        next_flush_ = now + uint32_t(config::mesh_cfg.node_flush_interval) * 1000u;

        // node_flush_threshold is in seconds → convert to ms.
        uint32_t threshold_ms = uint32_t(config::mesh_cfg.node_flush_threshold) * 1000u;

        for (size_t i = 0; i < Nodes::MAX_NODES; ++i)
        {
            Nodes::Entry *n = nodes_.at_mutable(i);
            if (!n || !n->used || n->timedout)
                continue;
            if (n->last_seen_ms == 0)
                continue;
            uint32_t age = uint32_t(time::ticks_diff(now, n->last_seen_ms));
            if (age > threshold_ms)
            {
                ESP_LOGW(kTag, "-- flushing timed-out node %s (age %ums)",
                      n->nick.data(), unsigned(age));
                nodes_.timeout(n->nick.data());
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    void LoRaMAN::update_active_nodes(const char *nick3, int16_t rssi, uint32_t now)
    {
        if (!nick3 || nick3[0] == '\0')
            return;
        if (nodes_.seen(nick3))
        {
            nodes_.update(nick3, now, rssi);
        }
        else
        {
            ESP_LOGI(kTag, "-- new node sensed: %s", nick3);
            if (auto r = nodes_.add(nick3, now, rssi); !r.has_value())
            {
                ESP_LOGW(kTag, "!! node table full, cannot track %s (code=%u)",
                       nick3, unsigned(r.error().code));
            }
        }
    }

    void LoRaMAN::update_rssi_history(int16_t rssi)
    {
        if (rssi_count_ < kRssiHistorySize)
        {
            rssi_history_[rssi_count_++] = rssi;
        }
        else
        {
            for (size_t i = 1; i < kRssiHistorySize; ++i)
                rssi_history_[i - 1] = rssi_history_[i];
            rssi_history_[kRssiHistorySize - 1] = rssi;
        }
    }

    int16_t LoRaMAN::rssi_history(size_t i) const
    {
        if (i >= rssi_count_)
            return -100;
        return rssi_history_[i];
    }

    void LoRaMAN::show_status_log(uint32_t now)
    {
        (void)now;
            ESP_LOGI(kTag, "~~ %s Q:%u DC:%.2f%% nodes:%u",
               device_nick(),
               unsigned(send_count_),
               double(duty_.get_duty_cycle()),
               unsigned(nodes_.active_count()));
    }

} // namespace loraman
