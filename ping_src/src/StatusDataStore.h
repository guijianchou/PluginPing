// PluginPing - 线程安全的状态快照存储
#pragma once
#include "StatusData.h"
#include <memory>
#include <mutex>

namespace pluginping {

// worker 线程写入，DrawItem / GetItemWidthEx 读取。快照以不可变 shared_ptr 发布，
// 读侧只复制指针，不复制整份快照，也不会出现撕裂读。
class StatusDataStore
{
public:
    // 读侧热路径：只增加一次引用计数，不做字符串深拷贝。
    std::shared_ptr<const LocalRemoteSnapshot> GetSnapshotShared() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_snapshot;
    }

    // worker 需要可变副本时使用。
    LocalRemoteSnapshot GetSnapshot() const
    {
        return *GetSnapshotShared();
    }

    void UpdateSnapshot(const LocalRemoteSnapshot& snapshot)
    {
        auto next = std::make_shared<const LocalRemoteSnapshot>(snapshot);
        std::lock_guard<std::mutex> lk(m_mutex);
        m_snapshot = std::move(next);
    }

    void UpdateLocal(const EndpointStatus& status)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LocalRemoteSnapshot next = *m_snapshot;
        next.local = status;
        m_snapshot = std::make_shared<const LocalRemoteSnapshot>(std::move(next));
    }

    void UpdateRemote(const EndpointStatus& status)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LocalRemoteSnapshot next = *m_snapshot;
        next.remote = status;
        m_snapshot = std::make_shared<const LocalRemoteSnapshot>(std::move(next));
    }

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const LocalRemoteSnapshot> m_snapshot = std::make_shared<LocalRemoteSnapshot>();
};

} // namespace pluginping
