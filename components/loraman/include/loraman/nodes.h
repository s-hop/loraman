#pragma once

#include "utils/error.h"

#include <array>
#include <cstdint>
#include <cstddef>

namespace loraman {

class Nodes
{
public:
    static constexpr size_t MAX_NODES = 32;
    static constexpr size_t NICK_LEN = 3;

    struct Entry
    {
        std::array<char, NICK_LEN + 1> nick{};
        uint32_t last_seen_ms = 0;
        int8_t last_rssi = 0;
        bool timedout = false;
        bool used = false;
    };

    Nodes() = default;

    // Add a new node to the table. Returns ErrCode::NodeTableFull if the
    // table is at MAX_NODES capacity.
    Result<void> add(const char *nick3, uint32_t now_ms, int8_t rssi);

    // Update an existing node's last-seen time and RSSI. If the node is not
    // in the table, calls add() internally. Returns void — "not found" is
    // not a failure for update (it triggers an implicit add).
    void update(const char *nick3, uint32_t now_ms, int8_t rssi);

    // Mark a node as timed out. Silently no-ops if the node is not found
    // or is already timed out. Returns void — not-found is not a failure
    // for timeout (idempotent operation).
    void timeout(const char *nick3);

    bool seen(const char *nick3) const;

    const Entry *find(const char *nick3) const;
    Entry *find_mutable(const char *nick3);

    size_t active_count() const { return active_; }
    size_t total_count() const { return total_; }

    const Entry *at(size_t i) const { return i < MAX_NODES ? &table_[i] : nullptr; }
    Entry *at_mutable(size_t i) { return i < MAX_NODES ? &table_[i] : nullptr; }

private:
    std::array<Entry, MAX_NODES> table_{};
    size_t total_ = 0;
    size_t active_ = 0;

    static bool nick_eq(const char *a, const char *b);
};

} // namespace loraman
