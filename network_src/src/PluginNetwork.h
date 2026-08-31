// pluginNetwork - plugin host class (ITMPlugin)
#pragma once
#include "Common.h"
#include "../../ping_src/include/PluginInterface.h"
#include "ConfigManager.h"
#include "MetricsStore.h"
#include "NetworkStatusItem.h"
#include "SlidingWindow.h"
#include "StunProbe.h"
#include "TcpProbe.h"
#include <atomic>
#include <iphlpapi.h>
#include <mutex>
#include <string>
#include <thread>

namespace pluginnetwork {

// Per-channel worker state. Owned by exactly one worker thread; only the
// derived ChannelSnapshot crosses into the shared store.
struct ChannelRuntime
{
    SlidingWindow window;
    int targetIndex = 0;                 // 0 = primary, then the fallbacks
    int targetFailStreak = 0;            // consecutive failures on the current target
    int egressChanges = 0;
    std::wstring lastEgress;
    std::wstring lastEgressTarget;
    std::wstring lastDiagnostic;
    std::wstring targetSetKey;
    unsigned long long lastRoundTick = 0;
    unsigned long long nextPrimaryRetryTick = 0;
    int primaryRecoverySuccessStreak = 0; // consecutive Echo successes before restore
    unsigned long long nextFallbackProbeTick = 0;
    bool enabled = false;
    bool idle = false;
    bool fallbackAttempted = false;
    bool fallbackVerified = false;
};

class CPluginNetwork : public ITMPlugin
{
private:
    CPluginNetwork();

public:
    ~CPluginNetwork();
    CPluginNetwork(const CPluginNetwork&) = delete;
    CPluginNetwork& operator=(const CPluginNetwork&) = delete;

    static CPluginNetwork& Instance();
    void Shutdown();

    // ===== ITMPlugin =====
    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    OptionReturn ShowOptionsDialog(void* hParent) override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;

private:
    void EnsureWorkersStarted();
    void StopWorkers();
    void TcpWorkerLoop();
    void UdpWorkerLoop();

    void PublishChannel(ChannelKind kind, const ChannelRuntime& runtime,
                        const PluginConfig& cfg, const std::wstring& activeTarget,
                        bool degraded = false, bool fallbackPending = false);
    void AppendDebugLog(const std::wstring& line);
    void HandleWorkerFailure(ChannelKind kind);
    bool HasDisplayedItem() const;

    // Returns true when nothing has displayed this channel for IdleAfterMs.
    bool ChannelIsIdle(const CNetworkStatusItem& item, const PluginConfig& cfg) const;
    DWORD WaitForNextRound(const PluginConfig& cfg, bool idle,
                           unsigned long long roundStartTick,
                           const CNetworkStatusItem& item,
                           unsigned long long observedEpoch,
                           HANDLE routeChangedEvent);

    static VOID CALLBACK OnIpInterfaceChanged(PVOID context,
                                              PMIB_IPINTERFACE_ROW row,
                                              MIB_NOTIFICATION_TYPE type);
    static VOID CALLBACK OnRouteChanged(PVOID context,
                                        PMIB_IPFORWARD_ROW2 row,
                                        MIB_NOTIFICATION_TYPE type);
    static VOID CALLBACK OnUnicastAddressChanged(PVOID context,
                                                 PMIB_UNICASTIPADDRESS_ROW row,
                                                 MIB_NOTIFICATION_TYPE type);
    void SignalRouteChange();
    void StartRouteNotifications();
    void CancelRouteNotifications();
    void CloseRouteEvents();

    CNetworkStatusItem m_tcpItem;
    CNetworkStatusItem m_udpItem;
    MetricsStore m_store;
    ConfigManager m_config;
    TcpProbe m_tcpProbe;
    UdpProbe m_udpProbe;
    UdpProbe m_primaryEchoProbe;  // isolated validation while STUN fallback stays active

    std::mutex m_logMutex;

    std::thread m_tcpWorker;
    std::thread m_udpWorker;
    std::mutex m_workerMutex;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_shutdown{ false };
    unsigned long long m_lastDebugLogWrite = 0; // protected by m_logMutex
    HANDLE m_wake = nullptr;    // manual-reset wake/stop event shared by both workers
    HANDLE m_tcpRouteChanged = nullptr;
    HANDLE m_udpRouteChanged = nullptr;
    HANDLE m_interfaceNotification = nullptr;
    HANDLE m_routeNotification = nullptr;
    HANDLE m_addressNotification = nullptr;
};

} // namespace pluginnetwork

// Exports looked up by TrafficMonitor through GetProcAddress.
extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance();
extern "C" __declspec(dllexport) void TMPluginShutdown();
