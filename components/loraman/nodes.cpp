#include "loraman/nodes.h"

#include <cstring>
#include <cstdint>

namespace loraman
{

    // Compare two 3-char nicks for equality. Caller is responsible for
    // ensuring non-null input — null is a bug at the call site, not here.
    bool Nodes::nick_eq(const char *a, const char *b)
    {
        return strncmp(a, b, NICK_LEN) == 0;
    }

    const Nodes::Entry *Nodes::find(const char *nick3) const
    {
        for (size_t i = 0; i < total_; ++i)
            if (table_[i].used && nick_eq(table_[i].nick.data(), nick3))
                return &table_[i];
        return nullptr;
    }

    Nodes::Entry *Nodes::find_mutable(const char *nick3)
    {
        for (size_t i = 0; i < total_; ++i)
            if (table_[i].used && nick_eq(table_[i].nick.data(), nick3))
                return &table_[i];
        return nullptr;
    }

    bool Nodes::seen(const char *nick3) const
    {
        return find(nick3) != nullptr;
    }

    Result<void> Nodes::add(const char *nick3, uint32_t now_ms, int8_t rssi)
    {
        if (!nick3)
            return fail(ErrCode::InvalidArgument);

        Entry *slot = nullptr;
        for (size_t i = 0; i < total_; ++i)
        {
            if (!table_[i].used)
            {
                slot = &table_[i];
                break;
            }
        }
        if (!slot)
        {
            if (total_ >= MAX_NODES)
                return fail(ErrCode::NodeTableFull);
            slot = &table_[total_];
            ++total_;
        }

        slot->nick.fill('\0');
        for (size_t i = 0; i < NICK_LEN && nick3[i]; ++i)
            slot->nick[i] = nick3[i];
        slot->last_seen_ms = now_ms;
        slot->last_rssi = rssi;
        slot->timedout = false;
        slot->used = true;
        ++active_;
        return ok();
    }

    void Nodes::update(const char *nick3, uint32_t now_ms, int8_t rssi)
    {
        Entry *n = find_mutable(nick3);
        if (!n)
        {
            // Node not in table — add it. If the table is full, silently drop
            // (the caller will see it as not tracked, which is acceptable).
            (void)add(nick3, now_ms, rssi);
            return;
        }
        if (n->timedout)
        {
            n->timedout = false;
            ++active_;
        }
        n->last_seen_ms = now_ms;
        n->last_rssi = rssi;
    }

    void Nodes::timeout(const char *nick3)
    {
        Entry *n = find_mutable(nick3);
        if (!n || n->timedout)
            return;
        n->timedout = true;
        if (active_ > 0)
            --active_;
    }

} // namespace loraman
