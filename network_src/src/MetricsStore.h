// pluginNetwork - thread-safe metrics snapshot store
#pragma once
#include "MetricsData.h"
#include <memory>
#include <mutex>

namespace pluginnetwork {

// The TCP and UDP workers each publish only their own channel; DrawItem and
// GetItemWidthEx read the whole snapshot. Publishing an immutable shared_ptr
// means the read path copies one pointer instead of a struct full of strings,
// and can never observe a half-written channel.
class MetricsStore
{
public:
    std::shared_ptr<const MetricsSnapshot> GetShared() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_snapshot;
    }

    void UpdateChannel(ChannelKind kind, const ChannelSnapshot& channel)
    {
        auto next = std::make_shared<MetricsSnapshot>();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            *next = *m_snapshot;
            if (kind == ChannelKind::Tcp)
                next->tcp = channel;
            else
                next->udp = channel;
            m_snapshot = std::move(next);
        }
    }

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const MetricsSnapshot> m_snapshot = std::make_shared<MetricsSnapshot>();
};

} // namespace pluginnetwork
