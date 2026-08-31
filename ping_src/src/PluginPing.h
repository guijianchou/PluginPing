// PluginPing - plugin host class (ITMPlugin)
#pragma once
#include "Common.h"
#include "../include/PluginInterface.h"
#include "PingStatusItem.h"
#include "PublicIpProvider.h"
#include "StatusDataStore.h"
#include "ConfigManager.h"
#include <thread>
#include <atomic>
#include <mutex>

namespace pluginping {

class CPluginPing : public ITMPlugin
{
private:
    CPluginPing();

public:
    ~CPluginPing();
    CPluginPing(const CPluginPing&) = delete;
    CPluginPing& operator=(const CPluginPing&) = delete;

    static CPluginPing& Instance();
    void Shutdown();

    // ===== ITMPlugin =====
    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;

private:
    void EnsureWorkersStarted();
    void StopWorkers();
    void DirectWorkerLoop();
    void ProxyWorkerLoop();
    void PublishWorkerFailure(PingEndpoint endpoint) noexcept;
    void PublishEndpoint(PingEndpoint endpoint, const EndpointStatus& status,
                         const PluginConfig& cfg);
    bool HasDisplayedItem() const;

    CPingStatusItem m_localItem;
    CPingStatusItem m_remoteItem;
    StatusDataStore m_store;
    ConfigManager m_config;

    std::thread m_directWorker;
    std::thread m_proxyWorker;
    std::mutex m_workerMutex;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_shutdown{ false };
    std::atomic<unsigned long long> m_directRevalidateRequest{ 0 };
    std::atomic<unsigned long long> m_directRevalidateCompleted{ 0 };
    // Protected by m_logMutex.
    unsigned long long m_lastDebugLogWrite = 0;
    std::mutex m_logMutex;
    HANDLE m_wake = nullptr;      // manual-reset wake/stop event
    HttpRequestCancellation m_directHttpCancellation;
    HttpRequestCancellation m_proxyHttpCancellation;
};

} // namespace pluginping

// Export looked up by TrafficMonitor through GetProcAddress.
extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance();
extern "C" __declspec(dllexport) void TMPluginShutdown();
