// pluginNetwork - TrafficMonitor host adapter and probe workers
#include "Common.h"
#include "PluginNetwork.h"
#include "ChannelStatePolicy.h"
#include "../../shared_src/RouteEpoch.h"
#include <algorithm>
#include <climits>
#include <cwchar>
#include <string>
#include <vector>

namespace pluginnetwork {

namespace {

constexpr unsigned long long kDebugLogMinIntervalMs = 1000;
constexpr unsigned long long kDebugLogMaxBytes = 5ull * 1024 * 1024;
constexpr unsigned long long kTcpFallbackDiagnosticIntervalMs = 60000;
constexpr unsigned long long kUdpPrimaryRetryIntervalMs = 15000;
constexpr int kUdpPrimaryRecoverySuccesses = 3;
constexpr DWORD kWorkerRetryBackoffMs = 5000;

void PinModuleForWorkerLifetime()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&PinModuleForWorkerLifetime), &self);
}

void AddUniqueTarget(std::vector<std::wstring>& targets, const std::wstring& target)
{
    if (target.empty())
        return;
    const auto same = [&target](const std::wstring& existing) {
        return _wcsicmp(existing.c_str(), target.c_str()) == 0;
    };
    if (std::find_if(targets.begin(), targets.end(), same) == targets.end())
        targets.push_back(target);
}

std::vector<std::wstring> TcpTargets(const PluginConfig& cfg)
{
    std::vector<std::wstring> targets;
    AddUniqueTarget(targets, cfg.tcpTargetUrl);
    AddUniqueTarget(targets, cfg.tcpFallbackUrl);
    return targets;
}

std::vector<std::wstring> UdpTargets(const PluginConfig& cfg)
{
    std::vector<std::wstring> targets;
    AddUniqueTarget(targets, cfg.udpEchoServer);
    AddUniqueTarget(targets, cfg.udpFallbackStunServer);
    return targets;
}

std::wstring TargetSetKey(const std::vector<std::wstring>& targets)
{
    std::wstring key;
    for (const std::wstring& target : targets)
    {
        if (!key.empty())
            key.push_back(L'\n');
        key += target;
    }
    return key;
}

// Everything a measurement is only comparable within. TCP follows the HTTP
// route and proxy identity; UDP follows its SOCKS/direct route and only shares
// the HTTP identity when the route planner says both protocols use the same
// local proxy selection. The local network generation applies to both.
std::wstring ChannelTargetKey(const std::vector<std::wstring>& targets,
                              const ProxyRoute& proxyRoute, ChannelKind kind)
{
    const pluginping_shared::RouteEpoch& epoch =
        pluginping_shared::RouteEpoch::Instance();
    std::wstring key = TargetSetKey(targets) + L"\nroute=" +
        (kind == ChannelKind::Tcp ? proxyRoute.TcpKey() : proxyRoute.UdpKey()) +
        L"\nnetwork=" + std::to_wstring(epoch.NetworkEpoch());
    if (kind == ChannelKind::Tcp || proxyRoute.udpSharesHttpIdentity)
        key += L"\nproxy=" + std::to_wstring(epoch.ProxyEpoch());
    return key;
}

bool ProbeGenerationChanged(const std::vector<std::wstring>& targets,
                            const ProxyRoute& proxyRoute, ChannelKind kind,
                            const std::wstring& expectedKey)
{
    pluginping_shared::RouteEpoch::Instance().ObserveLocalRoute();
    return ChannelTargetKey(targets, proxyRoute, kind) != expectedKey;
}

const wchar_t* StateText(ChannelState state)
{
    switch (state)
    {
    case ChannelState::Disabled:     return L"off";
    case ChannelState::Warming:      return L"warming";
    case ChannelState::Ok:           return L"ok";
    case ChannelState::Degraded:     return L"degraded";
    case ChannelState::Disconnected: return L"down";
    case ChannelState::Unsupported:  return L"unsupported";
    default:                         return L"unknown";
    }
}

std::wstring ValueOrDash(int value, const wchar_t* suffix)
{
    if (value < 0)
        return std::wstring(L"--") + suffix;
    return std::to_wstring(value) + suffix;
}

void AppendChannelDiagnostic(std::wstring& text, const wchar_t* name,
                             const ChannelSnapshot& channel, bool includeJitter)
{
    text += name;
    text += L": ";
    text += StateText(channel.state);
    text += L"\r\n  target: ";
    text += channel.activeTarget.empty() ? L"--" : channel.activeTarget;
    text += L"\r\n  latency: ";
    text += ValueOrDash(channel.avgRttMs, L" ms");
    if (includeJitter)
    {
        text += L", jitter: ";
        text += ValueOrDash(channel.jitterMs, L" ms");
    }
    text += L"\r\n  stability: ";
    text += ValueOrDash(channel.stabilityPct, L"%");
    text += L" (" + std::to_wstring(channel.successCount) + L"/" +
            std::to_wstring(channel.sampleCount) + L", window " +
            std::to_wstring(channel.windowCapacity) + L")";
    text += L"\r\n  failure streak: " + std::to_wstring(channel.consecutiveFailures) +
            L", session max: " + std::to_wstring(channel.maxConsecutiveFailures);
    if (!channel.diagnostic.empty())
    {
        text += L"\r\n  last: ";
        text += channel.diagnostic;
    }
}

std::wstring MakeDiagnosticText(const MetricsSnapshot& snapshot)
{
    std::wstring text;
    AppendChannelDiagnostic(text, L"TCP", snapshot.tcp, false);
    text += L"\r\n\r\n";
    AppendChannelDiagnostic(text, L"UDP", snapshot.udp, true);
    text += L"\r\n  UDP egress: ";
    text += snapshot.udp.egressIp.empty() ? L"--" : snapshot.udp.egressIp;
    text += L", changes: " + std::to_wstring(snapshot.udp.egressChanges);
    // Recorded on every entry so the log shows which node a measurement belongs
    // to; the epoch advancing between two entries is the rebuild trigger.
    const pluginping_shared::RouteEpoch& epoch = pluginping_shared::RouteEpoch::Instance();
    const std::wstring identity = epoch.Identity();
    text += L"\r\n\r\nproxy node: ";
    text += identity.empty() ? L"unconfirmed" : identity;
    text += L" (epoch " + std::to_wstring(epoch.Epoch()) + L")";
    text += L"\r\nproxy epoch: " + std::to_wstring(epoch.ProxyEpoch());
    text += L"\r\nnetwork epoch: " + std::to_wstring(epoch.NetworkEpoch());
    text += L"\r\nnetwork route: ";
    const std::wstring networkIdentity = epoch.NetworkIdentity();
    text += networkIdentity.empty() ? L"unobserved" : networkIdentity;
    return text;
}

void AppendChannelTooltip(std::wstring& text, const wchar_t* name,
                          const ChannelSnapshot& channel, bool includeJitter)
{
    text += name;
    text += L": ";
    text += StateText(channel.state);
    text += L" | ";
    text += ValueOrDash(channel.avgRttMs, L" ms");
    if (includeJitter)
    {
        text += L" | JIT ";
        text += ValueOrDash(channel.jitterMs, L" ms");
    }
    }

std::wstring MakeTooltip(const MetricsSnapshot& snapshot, bool showTcp, bool showUdp)
{
    // TrafficMonitor concatenates every loaded plug-in's text and pushes the
    // result through two MFC tooltip controls on every monitor tick. Keep this
    // contribution deliberately small; endpoints and diagnostics belong in
    // the optional debug log, not in an unbounded host UI string.
    std::wstring text;
    text.reserve(128);
    if (showTcp)
        AppendChannelTooltip(text, L"TCP", snapshot.tcp, false);
    if (showUdp)
    {
        if (!text.empty())
            text += L"\r\n";
        AppendChannelTooltip(text, L"UDP", snapshot.udp, true);
        if (!snapshot.udp.egressIp.empty())
        {
            text += L" | IP ";
            text += snapshot.udp.egressIp;
        }
    }
    return text;
}

std::wstring DebugTimestamp()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
               time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

std::string Utf8(const std::wstring& value)
{
    if (value.empty())
        return std::string();
    const int length = static_cast<int>((std::min)(value.size(),
                                                    static_cast<size_t>(INT_MAX)));
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), length,
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return std::string();
    std::string bytes(static_cast<size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), length, bytes.data(), needed,
                            nullptr, nullptr) != needed)
        return std::string();
    return bytes;
}

void RotateDebugLogIfLarge(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return;
    const unsigned long long size =
        (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size >= kDebugLogMaxBytes)
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
}

void ResetRuntime(ChannelRuntime& runtime, int capacity, bool enabled, bool idle,
                  const std::wstring& targetSetKey)
{
    runtime = ChannelRuntime{};
    runtime.window.Resize(capacity);
    runtime.enabled = enabled;
    runtime.idle = idle;
    runtime.targetSetKey = targetSetKey;
}

} // namespace

CPluginNetwork::CPluginNetwork()
    : m_tcpItem(ChannelKind::Tcp),
      m_udpItem(ChannelKind::Udp)
{
    m_tcpItem.Attach(&m_store, &m_config);
    m_udpItem.Attach(&m_store, &m_config);
#if defined(PLUGINPING_MERGED)
    // The host-visible rows already draw DIRECT/PROXY. Embedded probe sections
    // contribute only their status and measurements.
    m_tcpItem.SetEmbeddedMode(true);
    m_udpItem.SetEmbeddedMode(true);
#endif

    try
    {
        const auto cfg = m_config.GetShared();
        ChannelSnapshot tcp;
        tcp.state = cfg->tcpEnabled ? ChannelState::Warming : ChannelState::Disabled;
        tcp.windowCapacity = cfg->WindowCapacity(cfg->refreshIntervalMs, 1);
        tcp.activeTarget = cfg->tcpTargetUrl;
        tcp.diagnostic = cfg->tcpEnabled ? L"pending initial probe" : L"disabled by configuration";

        ChannelSnapshot udp;
        udp.state = cfg->udpEnabled ? ChannelState::Warming : ChannelState::Disabled;
        udp.windowCapacity = cfg->WindowCapacity(cfg->refreshIntervalMs,
                                                 cfg->burstPackets);
        udp.activeTarget = cfg->udpEchoServer;
        udp.diagnostic = cfg->udpEnabled ? L"pending initial probe" : L"disabled by configuration";

        m_store.UpdateChannel(ChannelKind::Tcp, tcp);
        m_store.UpdateChannel(ChannelKind::Udp, udp);
    }
    catch (...)
    {
        // The workers publish fresh snapshots after a successful start.
    }
}

CPluginNetwork::~CPluginNetwork()
{
    StopWorkers();
}

VOID CALLBACK CPluginNetwork::OnIpInterfaceChanged(PVOID context,
                                                    PMIB_IPINTERFACE_ROW row,
                                                    MIB_NOTIFICATION_TYPE type)
{
    (void)row;
    (void)type;
    if (auto* self = static_cast<CPluginNetwork*>(context))
        self->SignalRouteChange();
}

VOID CALLBACK CPluginNetwork::OnRouteChanged(PVOID context,
                                             PMIB_IPFORWARD_ROW2 row,
                                             MIB_NOTIFICATION_TYPE type)
{
    (void)row;
    (void)type;
    if (auto* self = static_cast<CPluginNetwork*>(context))
        self->SignalRouteChange();
}

VOID CALLBACK CPluginNetwork::OnUnicastAddressChanged(PVOID context,
                                                      PMIB_UNICASTIPADDRESS_ROW row,
                                                      MIB_NOTIFICATION_TYPE type)
{
    (void)row;
    (void)type;
    if (auto* self = static_cast<CPluginNetwork*>(context))
        self->SignalRouteChange();
}

void CPluginNetwork::SignalRouteChange()
{
    pluginping_shared::RouteEpoch::Instance().InvalidateLocalRoute();
    if (m_tcpRouteChanged)
        SetEvent(m_tcpRouteChanged);
    if (m_udpRouteChanged)
        SetEvent(m_udpRouteChanged);
}

void CPluginNetwork::StartRouteNotifications()
{
    m_tcpRouteChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_udpRouteChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_tcpRouteChanged || !m_udpRouteChanged)
    {
        CloseRouteEvents();
        return;
    }

    HANDLE interfaceNotification = nullptr;
    HANDLE routeNotification = nullptr;
    HANDLE addressNotification = nullptr;
    const DWORD interfaceResult = NotifyIpInterfaceChange(
        AF_UNSPEC, &CPluginNetwork::OnIpInterfaceChanged,
        this, FALSE, &interfaceNotification);
    const DWORD routeResult = NotifyRouteChange2(
        AF_UNSPEC, &CPluginNetwork::OnRouteChanged,
        this, FALSE, &routeNotification);
    const DWORD addressResult = NotifyUnicastIpAddressChange(
        AF_UNSPEC, &CPluginNetwork::OnUnicastAddressChanged,
        this, FALSE, &addressNotification);
    if (interfaceResult == NO_ERROR && routeResult == NO_ERROR &&
        addressResult == NO_ERROR)
    {
        m_interfaceNotification = interfaceNotification;
        m_routeNotification = routeNotification;
        m_addressNotification = addressNotification;
    }
    else
    {
        if (interfaceNotification)
            CancelMibChangeNotify2(interfaceNotification);
        if (routeNotification)
            CancelMibChangeNotify2(routeNotification);
        if (addressNotification)
            CancelMibChangeNotify2(addressNotification);
        CloseRouteEvents();
    }
}

void CPluginNetwork::CancelRouteNotifications()
{
    if (m_interfaceNotification)
    {
        CancelMibChangeNotify2(m_interfaceNotification);
        m_interfaceNotification = nullptr;
    }
    if (m_routeNotification)
    {
        CancelMibChangeNotify2(m_routeNotification);
        m_routeNotification = nullptr;
    }
    if (m_addressNotification)
    {
        CancelMibChangeNotify2(m_addressNotification);
        m_addressNotification = nullptr;
    }
}

void CPluginNetwork::CloseRouteEvents()
{
    if (m_tcpRouteChanged)
    {
        CloseHandle(m_tcpRouteChanged);
        m_tcpRouteChanged = nullptr;
    }
    if (m_udpRouteChanged)
    {
        CloseHandle(m_udpRouteChanged);
        m_udpRouteChanged = nullptr;
    }
}

CPluginNetwork& CPluginNetwork::Instance()
{
    // Process-lifetime object by design. TMPluginShutdown performs the orderly
    // stop; running ~CPluginNetwork from DLL static teardown could otherwise
    // join workers while the Windows loader lock is held.
    static CPluginNetwork* instance = new CPluginNetwork();
    return *instance;
}

void CPluginNetwork::Shutdown()
{
    m_shutdown.store(true, std::memory_order_release);
    StopWorkers();
}

IPluginItem* CPluginNetwork::GetItem(int index)
{
    if (index == 0)
        return &m_tcpItem;
    if (index == 1)
        return &m_udpItem;
    return nullptr;
}

void CPluginNetwork::DataRequired()
{
    try
    {
        // The host calls DataRequired for every loaded plug-in, including
        // items the user has not selected. A draw/measure callback is the only
        // reliable visibility signal exposed by API 7.
        if (!HasDisplayedItem())
            return;
        EnsureWorkersStarted();
    }
    catch (...)
    {
        // The next host tick can retry a failed worker start.
    }
}

const wchar_t* CPluginNetwork::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:        return L"pluginNetwork";
    case TMI_DESCRIPTION: return L"Monitors overseas TCP proxy and UDP STUN line quality.";
    case TMI_AUTHOR:      return L"pluginNetwork contributors";
    case TMI_COPYRIGHT:   return L"Copyright (C) 2026 pluginNetwork contributors";
    case TMI_VERSION:     return kPluginNetworkVersion;
    case TMI_URL:         return L"";
    default:              return L"";
    }
}

const wchar_t* CPluginNetwork::GetTooltipInfo()
{
    try
    {
        // The host also concatenates tooltip text from hidden plug-ins. Do not
        // change its UI or allocate a large string until one of our rows has
        // actually participated in layout or drawing.
        if (!HasDisplayedItem())
            return L"";
        thread_local std::wstring copy;
        const auto snapshot = m_store.GetShared();
        copy = MakeTooltip(*snapshot, m_tcpItem.LastDrawTick() != 0,
                           m_udpItem.LastDrawTick() != 0);
        return copy.c_str();
    }
    catch (...)
    {
        return L"";
    }
}

ITMPlugin::OptionReturn CPluginNetwork::ShowOptionsDialog(void* hParent)
{
    try
    {
        m_config.Reload();
        const std::wstring path = m_config.ConfigPath();
        if (path.empty())
            return OR_OPTION_NOT_PROVIDED;
        const HINSTANCE result = ShellExecuteW(static_cast<HWND>(hParent), L"open",
                                                path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32 ? OR_OPTION_UNCHANGED
                                                      : OR_OPTION_NOT_PROVIDED;
    }
    catch (...)
    {
        return OR_OPTION_NOT_PROVIDED;
    }
}

void CPluginNetwork::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    try
    {
        if (index == EI_CONFIG_DIR && data && data[0] != L'\0')
            m_config.SetConfigDir(data);
    }
    catch (...)
    {
        // The DLL-directory fallback remains available.
    }
}

void CPluginNetwork::PublishChannel(ChannelKind kind, const ChannelRuntime& runtime,
                                    const PluginConfig& cfg, const std::wstring& activeTarget,
                                    bool degraded,
                                    bool fallbackPending)
{
    ChannelSnapshot channel;
    channel.avgRttMs = runtime.window.AverageRecentRttMs(cfg.recentAvgCount);
    channel.jitterMs = runtime.window.JitterMs(cfg.recentAvgCount);
    channel.stabilityPct = runtime.window.StabilityPercent();
    channel.sampleCount = runtime.window.Count();
    channel.windowCapacity = runtime.window.Capacity();
    channel.successCount = runtime.window.SuccessCount();
    channel.consecutiveFailures = runtime.window.ConsecutiveFailures();
    channel.maxConsecutiveFailures = runtime.window.MaxConsecutiveFailures();
    channel.egressChanges = runtime.egressChanges;
    channel.egressIp = runtime.lastEgress;
    channel.activeTarget = activeTarget;
    channel.diagnostic = runtime.lastDiagnostic;

    const auto current = m_store.GetShared();
    ChannelStateInput stateInput;
    stateInput.enabled = runtime.enabled;
    stateInput.udp = kind == ChannelKind::Udp;
    stateInput.tcpHealthy = current->tcp.state == ChannelState::Ok;
    stateInput.fallbackPending = fallbackPending;
    stateInput.degraded = degraded;
    stateInput.successCount = channel.successCount;
    stateInput.sampleCount = channel.sampleCount;
    stateInput.windowCapacity = channel.windowCapacity;
    stateInput.consecutiveFailures = channel.consecutiveFailures;
    stateInput.maxFailCount = cfg.maxFailCount;
    stateInput.minSamples = cfg.minSamples;
    channel.state = EvaluateChannelState(stateInput);

    m_store.UpdateChannel(kind, channel);

    if (cfg.debugLog)
    {
        const auto snapshot = m_store.GetShared();
        AppendDebugLog(MakeDiagnosticText(*snapshot));
    }
}

void CPluginNetwork::AppendDebugLog(const std::wstring& line)
{
    std::lock_guard<std::mutex> lock(m_logMutex);
    const unsigned long long now = GetTickCount64();
    if (m_lastDebugLogWrite != 0 &&
        now - m_lastDebugLogWrite < kDebugLogMinIntervalMs)
        return;
    m_lastDebugLogWrite = now;
    const std::wstring path = m_config.DefaultSiblingFile(L"PluginPing_network_debug.log");
    if (path.empty())
        return;
    RotateDebugLogIfLarge(path);

    std::wstring entry = DebugTimestamp() + L"\r\n" + line + L"\r\n\r\n";
    const std::string bytes = Utf8(entry);
    if (bytes.empty())
        return;

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>((std::min)(bytes.size(),
                                                               static_cast<size_t>(MAXDWORD))),
              &written, nullptr);
    CloseHandle(file);
}

bool CPluginNetwork::HasDisplayedItem() const
{
    return m_tcpItem.LastDrawTick() != 0 || m_udpItem.LastDrawTick() != 0;
}

void CPluginNetwork::EnsureWorkersStarted()
{
    if (m_shutdown.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(m_workerMutex);
    if (m_shutdown.load(std::memory_order_acquire) ||
        m_running.load(std::memory_order_acquire))
        return;

    m_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_wake)
        return;
    StartRouteNotifications();

    m_running.store(true, std::memory_order_release);
    PinModuleForWorkerLifetime();

    try
    {
        m_tcpWorker = std::thread(&CPluginNetwork::TcpWorkerLoop, this);
        m_udpWorker = std::thread(&CPluginNetwork::UdpWorkerLoop, this);
    }
    catch (...)
    {
        m_running.store(false, std::memory_order_release);
        SetEvent(m_wake);
        CancelRouteNotifications();
        m_tcpProbe.Cancel();
        if (m_tcpWorker.joinable())
            m_tcpWorker.join();
        if (m_udpWorker.joinable())
            m_udpWorker.join();
        CloseRouteEvents();
        CloseHandle(m_wake);
        m_wake = nullptr;
        return;
    }

}

void CPluginNetwork::StopWorkers()
{
    std::lock_guard<std::mutex> lock(m_workerMutex);
    m_running.store(false, std::memory_order_release);
    if (m_wake)
        SetEvent(m_wake);
    CancelRouteNotifications();

    // Break synchronous WinHTTP/select calls before joining. These methods are
    // idempotent and are the only probe operations issued by the host thread.
    m_tcpProbe.Cancel();
    m_udpProbe.Cancel();
    m_primaryEchoProbe.Cancel();

    if (m_tcpWorker.joinable())
        m_tcpWorker.join();
    if (m_udpWorker.joinable())
        m_udpWorker.join();

    // Safe only after both joins: with the workers gone nothing else can touch
    // the probes, so the WinHTTP session and the STUN socket can be released
    // rather than lingering until the singleton's destructor at process exit.
    m_tcpProbe.Close();
    m_udpProbe.Close();
    m_primaryEchoProbe.Close();

    CloseRouteEvents();
    if (m_wake)
    {
        CloseHandle(m_wake);
        m_wake = nullptr;
    }
}

bool CPluginNetwork::ChannelIsIdle(const CNetworkStatusItem& item,
                                   const PluginConfig& cfg) const
{
    if (cfg.idleAfterMs == 0)
        return false;
    const unsigned long long lastDraw = item.LastDrawTick();
    return lastDraw != 0 && GetTickCount64() - lastDraw >=
                                static_cast<unsigned long long>(cfg.idleAfterMs);
}

DWORD CPluginNetwork::WaitForNextRound(const PluginConfig& cfg, bool idle,
                                       unsigned long long roundStartTick,
                                       const CNetworkStatusItem& item,
                                       unsigned long long observedEpoch,
                                       HANDLE routeChangedEvent)
{
    pluginping_shared::RouteEpoch& routeEpoch =
        pluginping_shared::RouteEpoch::Instance();
    // The IP worker normally observes the route, but pluginNetwork can also
    // run on its own. Seed here, then use notifications when all registrations
    // succeeded or an interruptible one-second polling fallback otherwise.
    routeEpoch.ObserveLocalRoute();
    if (routeEpoch.Epoch() != observedEpoch)
        return WAIT_TIMEOUT;
    const int interval = idle ? cfg.idleRefreshIntervalMs : cfg.refreshIntervalMs;
    const unsigned long long due = roundStartTick +
                                   static_cast<unsigned long long>((std::max)(interval, 1));
    if (routeChangedEvent && (!idle || item.ActivityEvent()))
    {
        const unsigned long long now = GetTickCount64();
        if (now >= due)
            return WAIT_TIMEOUT;
        HANDLE handles[3] = {};
        DWORD handleCount = 0;
        handles[handleCount++] = m_wake;
        if (idle && item.ActivityEvent())
            handles[handleCount++] = item.ActivityEvent();
        handles[handleCount++] = routeChangedEvent;
        const DWORD wait = static_cast<DWORD>((std::min)(
            due - now, static_cast<unsigned long long>(MAXDWORD - 1)));
        const DWORD result = WaitForMultipleObjects(handleCount, handles, FALSE, wait);
        return result == WAIT_OBJECT_0 ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }

    for (;;)
    {
        const unsigned long long now = GetTickCount64();
        if (now >= due)
            return WAIT_TIMEOUT;

        const DWORD wait = static_cast<DWORD>((std::min)(
            due - now, static_cast<unsigned long long>(1000)));
        if (idle && item.ActivityEvent())
        {
            const HANDLE handles[] = { m_wake, item.ActivityEvent() };
            const DWORD result = WaitForMultipleObjects(
                ARRAYSIZE(handles), handles, FALSE, wait);
            if (result == WAIT_OBJECT_0)
                return WAIT_OBJECT_0;
            if (result == WAIT_OBJECT_0 + 1 || result == WAIT_FAILED)
                return WAIT_TIMEOUT;
        }
        else if (WaitForSingleObject(m_wake, wait) == WAIT_OBJECT_0)
        {
            return WAIT_OBJECT_0;
        }

        routeEpoch.ObserveLocalRoute();
        if (routeEpoch.Epoch() != observedEpoch)
            return WAIT_TIMEOUT;

    }
}

void CPluginNetwork::TcpWorkerLoop()
{
    while (m_running.load(std::memory_order_acquire))
    {
        try
        {
            ChannelRuntime runtime;

    while (m_running.load(std::memory_order_acquire))
    {
        const auto config = m_config.Reload();
        const PluginConfig& cfg = *config;
        pluginping_shared::RouteEpoch& routeEpoch =
            pluginping_shared::RouteEpoch::Instance();
        routeEpoch.ObserveLocalRoute();
        const unsigned long long observedEpoch = routeEpoch.Epoch();
        if (m_tcpItem.LastDrawTick() == 0)
        {
            if (WaitForNextRound(cfg, false, GetTickCount64(), m_tcpItem,
                                 observedEpoch, m_tcpRouteChanged) == WAIT_OBJECT_0)
                break;
            continue;
        }
        const ProxyRoute proxyRoute = ResolveProxyRoute(cfg);
        const std::vector<std::wstring> targets = TcpTargets(cfg);
        const std::wstring targetKey =
            ChannelTargetKey(targets, proxyRoute, ChannelKind::Tcp);
        const bool enabled = cfg.tcpEnabled && !targets.empty();
        const bool idle = ChannelIsIdle(m_tcpItem, cfg);
        const int cadence = idle ? cfg.idleRefreshIntervalMs : cfg.refreshIntervalMs;
        const int capacity = cfg.WindowCapacity(cadence, 1);

        // Captured before the reset clears it: a key that merely appeared is a
        // first round, while a key that changed is a route the pooled WinHTTP
        // connection and the retained samples no longer describe.
        const bool routeChanged = !runtime.targetSetKey.empty() &&
                                  runtime.targetSetKey != targetKey;
        if (runtime.targetSetKey.empty() || runtime.enabled != enabled ||
            runtime.targetSetKey != targetKey)
        {
            ResetRuntime(runtime, capacity, enabled, idle, targetKey);
            m_tcpProbe.ResetTransport();
            if (routeChanged && enabled)
            {
                runtime.lastDiagnostic = L"route changed; rebuilding TCP transport";
                PublishChannel(ChannelKind::Tcp, runtime, cfg, targets.front());
            }
        }
        else if (runtime.idle != idle)
        {
            runtime.window = SlidingWindow();
            runtime.window.Resize(capacity);
            runtime.targetIndex = 0;
            runtime.targetFailStreak = 0;
            runtime.lastRoundTick = 0;
            runtime.nextFallbackProbeTick = 0;
            runtime.idle = idle;
        }
        else
        {
            runtime.window.Resize(capacity);
        }

        const unsigned long long roundStart = GetTickCount64();
        const unsigned long long gapLimit = static_cast<unsigned long long>(
            (std::max)(cfg.windowMs * 2, cadence * 3));
        if (runtime.lastRoundTick != 0 && roundStart - runtime.lastRoundTick > gapLimit)
        {
            runtime.window.Reset();
            runtime.targetIndex = 0;
            runtime.targetFailStreak = 0;
            runtime.nextFallbackProbeTick = 0;
            runtime.lastDiagnostic = L"measurement window reset after timer gap";
        }
        runtime.lastRoundTick = roundStart;

        if (!enabled)
        {
            runtime.lastDiagnostic = L"disabled by configuration";
            PublishChannel(ChannelKind::Tcp, runtime, cfg,
                           targets.empty() ? L"" : targets.front());
            if (WaitForNextRound(cfg, idle, roundStart, m_tcpItem,
                                 observedEpoch, m_tcpRouteChanged) == WAIT_OBJECT_0)
                break;
            continue;
        }

        // A fallback probe is diagnostic only. It never enters the sliding
        // window and the published active target remains Google, so a directly
        // reachable Cloudflare endpoint cannot restore TCP health.
        if (runtime.targetIndex != 0)
        {
            const std::wstring fallback = targets[static_cast<size_t>(runtime.targetIndex)];
            m_tcpProbe.Configure(fallback, cfg, proxyRoute);
            const ProbeResult diagnostic = m_tcpProbe.Run();
            if (diagnostic.cancelled || !m_running.load(std::memory_order_acquire))
                break;
            if (ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Tcp, targetKey))
            {
                m_tcpProbe.ResetTransport();
                continue;
            }

            runtime.lastDiagnostic = L"tcp fallback diagnostic: " + diagnostic.diagnostic +
                                     L"; Google remains authoritative";
            runtime.targetIndex = 0;
            runtime.targetFailStreak = 0;
            runtime.nextFallbackProbeTick = roundStart + kTcpFallbackDiagnosticIntervalMs;

            if (ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Tcp, targetKey))
            {
                m_tcpProbe.ResetTransport();
                continue;
            }

            PublishChannel(ChannelKind::Tcp, runtime, cfg, targets.front());
            if (WaitForNextRound(cfg, idle, roundStart, m_tcpItem,
                                 observedEpoch, m_tcpRouteChanged) == WAIT_OBJECT_0)
                break;
            continue;
        }

        const std::wstring& activeTarget = targets.front();
        m_tcpProbe.Configure(activeTarget, cfg, proxyRoute);
        const ProbeResult result = m_tcpProbe.Run();
        if (result.cancelled || !m_running.load(std::memory_order_acquire))
            break;
        if (ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Tcp, targetKey))
        {
            m_tcpProbe.ResetTransport();
            continue;
        }

        runtime.window.Push(result.ok, result.rttMs);
        runtime.lastDiagnostic = result.diagnostic;
        if (routeChanged)
            runtime.lastDiagnostic += L"; transport rebuilt after route change";
        if (result.ok)
        {
            runtime.targetFailStreak = 0;
            runtime.nextFallbackProbeTick = 0;
        }
        else
        {
            ++runtime.targetFailStreak;
            if (runtime.targetFailStreak >= cfg.maxFailCount && targets.size() > 1 &&
                (runtime.nextFallbackProbeTick == 0 ||
                 roundStart >= runtime.nextFallbackProbeTick))
            {
                runtime.targetIndex = 1;
                runtime.targetFailStreak = 0;
                runtime.lastDiagnostic += L"; Cloudflare diagnostic scheduled";
            }
        }

        if (ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Tcp, targetKey))
        {
            m_tcpProbe.ResetTransport();
            continue;
        }
        PublishChannel(ChannelKind::Tcp, runtime, cfg, activeTarget);
        if (WaitForNextRound(cfg, idle, roundStart, m_tcpItem,
                             observedEpoch, m_tcpRouteChanged) == WAIT_OBJECT_0)
            break;
            }
            return;
        }
        catch (...)
        {
            m_tcpProbe.ResetTransport();
            HandleWorkerFailure(ChannelKind::Tcp);
            if (!m_running.load(std::memory_order_acquire))
                return;
            if (WaitForSingleObject(m_wake, kWorkerRetryBackoffMs) == WAIT_OBJECT_0)
                return;
        }
    }
}

void CPluginNetwork::UdpWorkerLoop()
{
    while (m_running.load(std::memory_order_acquire))
    {
        try
        {
            ChannelRuntime runtime;

    while (m_running.load(std::memory_order_acquire))
    {
        const auto config = m_config.Reload();
        const PluginConfig& cfg = *config;
        pluginping_shared::RouteEpoch& routeEpoch =
            pluginping_shared::RouteEpoch::Instance();
        routeEpoch.ObserveLocalRoute();
        const unsigned long long observedEpoch = routeEpoch.Epoch();
        if (m_udpItem.LastDrawTick() == 0)
        {
            if (WaitForNextRound(cfg, false, GetTickCount64(), m_udpItem,
                                 observedEpoch, m_udpRouteChanged) == WAIT_OBJECT_0)
                break;
            continue;
        }
        const ProxyRoute proxyRoute = ResolveProxyRoute(cfg);
        const std::vector<std::wstring> targets = UdpTargets(cfg);
        const std::wstring targetKey =
            ChannelTargetKey(targets, proxyRoute, ChannelKind::Udp);
        const bool enabled = cfg.udpEnabled && !targets.empty();
        const bool idle = ChannelIsIdle(m_udpItem, cfg) && runtime.targetIndex == 0;
        const int cadence = idle ? cfg.idleRefreshIntervalMs : cfg.refreshIntervalMs;
        const int capacity = cfg.WindowCapacity(cadence, cfg.burstPackets);

        // See the TCP worker: a changed key invalidates the SOCKS5 association
        // and the cached target address as well as the measurement window.
        const bool routeChanged = !runtime.targetSetKey.empty() &&
                                  runtime.targetSetKey != targetKey;
        if (runtime.targetSetKey.empty() || runtime.enabled != enabled ||
            runtime.targetSetKey != targetKey)
        {
            ResetRuntime(runtime, capacity, enabled, idle, targetKey);
            m_udpProbe.ResetTransport();
            m_primaryEchoProbe.ResetTransport();
            if (routeChanged && enabled)
            {
                runtime.lastDiagnostic = L"route changed; rebuilding UDP transport";
                PublishChannel(ChannelKind::Udp, runtime, cfg, targets.front());
            }
        }
        else if (runtime.idle != idle)
        {
            runtime.window = SlidingWindow();
            runtime.window.Resize(capacity);
            runtime.targetFailStreak = 0;
            runtime.lastRoundTick = 0;
            runtime.idle = idle;
        }
        else
        {
            runtime.window.Resize(capacity);
        }

        const unsigned long long roundStart = GetTickCount64();
        const unsigned long long gapLimit = static_cast<unsigned long long>(
            (std::max)(cfg.windowMs * 2, cadence * 3));
        if (runtime.lastRoundTick != 0 && roundStart - runtime.lastRoundTick > gapLimit)
        {
            runtime.window.Reset();
            runtime.targetFailStreak = 0;
            runtime.lastDiagnostic = L"measurement window reset after timer gap";
        }
        runtime.lastRoundTick = roundStart;

        if (!enabled)
        {
            runtime.lastDiagnostic = L"disabled by configuration";
            PublishChannel(ChannelKind::Udp, runtime, cfg,
                           targets.empty() ? L"" : targets.front());
            if (WaitForNextRound(cfg, idle, roundStart, m_udpItem,
                                 observedEpoch, m_udpRouteChanged) == WAIT_OBJECT_0)
                break;
            continue;
        }

        if (runtime.targetIndex >= static_cast<int>(targets.size()))
        {
            if (runtime.targetIndex != 0)
                runtime.window.ClearRttHistory();
            runtime.targetIndex = 0;
            runtime.fallbackAttempted = false;
            runtime.fallbackVerified = false;
        }
        // Fallback mode bypasses idle backoff, so this deadline is evaluated at
        // the normal cadence and the echo primary is rechecked every 15 seconds.
        if (runtime.targetIndex != 0 && runtime.nextPrimaryRetryTick != 0 &&
            roundStart >= runtime.nextPrimaryRetryTick)
        {
            // Validate the primary without abandoning the working fallback.
            // This out-of-band result must not enter the fallback quality
            // window: it describes Echo recovery, not the active STUN route.
            const std::wstring fallbackTarget =
                targets[static_cast<size_t>(runtime.targetIndex)];
            const std::wstring& primaryTarget = targets.front();
            m_primaryEchoProbe.Configure(primaryTarget, 3478, cfg, proxyRoute,
                                         UdpProbeProtocol::Echo);
            const ProbeResult primaryResult = m_primaryEchoProbe.Run();
            // A validation probe is deliberately short-lived. In SOCKS5 mode
            // retaining this second UDP association while the primary is idle
            // can leave a stale control connection that delays later recovery.
            m_primaryEchoProbe.ResetTransport();
            if (primaryResult.cancelled || !m_running.load(std::memory_order_acquire))
                return;
            if (ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Udp, targetKey))
                continue;

            if (primaryResult.ok)
            {
                ++runtime.primaryRecoverySuccessStreak;
                if (runtime.primaryRecoverySuccessStreak >= kUdpPrimaryRecoverySuccesses)
                {
                    runtime.window.ClearRttHistory();
                    runtime.window.Push(true, primaryResult.rttMs);
                    runtime.targetIndex = 0;
                    runtime.fallbackVerified = false;
                    runtime.targetFailStreak = 0;
                    runtime.nextPrimaryRetryTick = 0;
                    runtime.primaryRecoverySuccessStreak = 0;
                    runtime.lastEgress.clear();
                    runtime.lastEgressTarget.clear();
                    runtime.lastDiagnostic = primaryResult.diagnostic + L"; primary restored after " +
                                             std::to_wstring(kUdpPrimaryRecoverySuccesses) + L" consecutive successes";
                    if (ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Udp, targetKey))
                    {
                        m_udpProbe.ResetTransport();
                        continue;
                    }
                    PublishChannel(ChannelKind::Udp, runtime, cfg, primaryTarget);
                }
                else
                {
                    runtime.nextPrimaryRetryTick = roundStart + kUdpPrimaryRetryIntervalMs;
                    runtime.lastDiagnostic = primaryResult.diagnostic + L"; primary recovery " +
                                             std::to_wstring(runtime.primaryRecoverySuccessStreak) + L"/" +
                                             std::to_wstring(kUdpPrimaryRecoverySuccesses) + L"; keeping " +
                                             fallbackTarget;
                    if (ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Udp, targetKey))
                    {
                        m_udpProbe.ResetTransport();
                        continue;
                    }
                    PublishChannel(ChannelKind::Udp, runtime, cfg, fallbackTarget,
                                   runtime.fallbackVerified, !runtime.fallbackAttempted);
                }
            }
            else
            {
                runtime.primaryRecoverySuccessStreak = 0;
                runtime.nextPrimaryRetryTick = roundStart + kUdpPrimaryRetryIntervalMs;
                runtime.lastDiagnostic = L"udp primary retry failed: " +
                                         primaryResult.diagnostic + L"; keeping " +
                                         fallbackTarget;
                if (ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Udp, targetKey))
                {
                    m_udpProbe.ResetTransport();
                    continue;
                }
                PublishChannel(ChannelKind::Udp, runtime, cfg, fallbackTarget,
                               runtime.fallbackVerified, !runtime.fallbackAttempted);
            }

            if (WaitForNextRound(cfg, idle, roundStart, m_udpItem,
                                 observedEpoch, m_udpRouteChanged) == WAIT_OBJECT_0)
                break;
            continue;
        }

        const std::wstring activeTarget = targets[static_cast<size_t>(runtime.targetIndex)];
        const UdpProbeProtocol protocol = runtime.targetIndex == 0
            ? UdpProbeProtocol::Echo
            : UdpProbeProtocol::Stun;
        m_udpProbe.Configure(activeTarget, 3478, cfg, proxyRoute, protocol);
        if (protocol == UdpProbeProtocol::Stun)
            runtime.fallbackAttempted = true;

        bool staleRound = false;
        bool egressChanged = false;
        for (int packet = 0; packet < cfg.burstPackets; ++packet)
        {
            const ProbeResult result = m_udpProbe.Run();
            if (result.cancelled || !m_running.load(std::memory_order_acquire))
                return;
            if (ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Udp, targetKey))
            {
                staleRound = true;
                break;
            }

            if (result.ok && protocol == UdpProbeProtocol::Stun &&
                !result.egressIp.empty() && !runtime.lastEgress.empty() &&
                runtime.lastEgress != result.egressIp &&
                runtime.lastEgressTarget == activeTarget)
            {
                ++runtime.egressChanges;
                egressChanged = true;
                runtime.window.Reset();
                runtime.targetFailStreak = 0;
                // The result that exposed the new mapping was measured on the
                // old association. Keep it out of the new route's window and
                // remember the new egress so the rebuilt association does not
                // report the same transition a second time.
                runtime.lastEgress = result.egressIp;
                runtime.lastEgressTarget = activeTarget;
                runtime.lastDiagnostic = result.diagnostic +
                    L"; UDP egress changed, transport rebuilt";
                break;
            }

            runtime.window.Push(result.ok, result.rttMs);
            runtime.lastDiagnostic = result.diagnostic;
            if (result.ok)
            {
                runtime.targetFailStreak = 0;
                if (protocol == UdpProbeProtocol::Stun)
                    runtime.fallbackVerified = true;
                if (protocol == UdpProbeProtocol::Echo)
                {
                    runtime.lastEgress.clear();
                    runtime.lastEgressTarget.clear();
                }
                else if (!result.egressIp.empty())
                {
                    runtime.lastEgress = result.egressIp;
                    runtime.lastEgressTarget = activeTarget;
                }
            }
            else
            {
                ++runtime.targetFailStreak;
            }
        }
        const bool generationChanged = staleRound ||
            ProbeGenerationChanged(targets, proxyRoute, ChannelKind::Udp, targetKey);
        if (generationChanged || egressChanged)
        {
            m_udpProbe.ResetTransport();
            m_primaryEchoProbe.ResetTransport();
            if (egressChanged && !generationChanged)
                PublishChannel(ChannelKind::Udp, runtime, cfg, activeTarget);
            continue;
        }
        if (routeChanged)
            runtime.lastDiagnostic += L"; transport rebuilt after route change";

        if (runtime.targetFailStreak >= 1 && targets.size() > 1 &&
            runtime.targetIndex == 0)
        {
            runtime.targetIndex = 1;
            runtime.targetFailStreak = 0;
            runtime.fallbackAttempted = false;
            runtime.fallbackVerified = false;
            runtime.primaryRecoverySuccessStreak = 0;
            runtime.window.ClearRttHistory();
            runtime.lastDiagnostic += L"; switching target";
            runtime.nextPrimaryRetryTick = roundStart + kUdpPrimaryRetryIntervalMs;
        }

        const std::wstring& selectedTarget =
            targets[static_cast<size_t>(runtime.targetIndex)];
        const bool onFallback = runtime.targetIndex != 0;
        PublishChannel(ChannelKind::Udp, runtime, cfg, selectedTarget,
                       onFallback && runtime.fallbackVerified,
                       onFallback && !runtime.fallbackAttempted);
        if (WaitForNextRound(cfg, idle && runtime.targetIndex == 0, roundStart,
                             m_udpItem, observedEpoch, m_udpRouteChanged) == WAIT_OBJECT_0)
            break;
            }
            return;
        }
        catch (...)
        {
            m_udpProbe.ResetTransport();
            m_primaryEchoProbe.ResetTransport();
            HandleWorkerFailure(ChannelKind::Udp);
            if (!m_running.load(std::memory_order_acquire))
                return;
            if (WaitForSingleObject(m_wake, kWorkerRetryBackoffMs) == WAIT_OBJECT_0)
                return;
        }
    }
}

void CPluginNetwork::HandleWorkerFailure(ChannelKind kind)
{
    // The failing channel publishes DOWN while its owning thread performs the
    // retry loop. The healthy worker is independent and remains live;
    // no joinable thread is replaced from a host callback.
    try
    {
        const auto current = m_store.GetShared();
        ChannelSnapshot failed = kind == ChannelKind::Tcp ? current->tcp : current->udp;
        failed.state = ChannelState::Disconnected;
        failed.diagnostic = L"worker retrying after an unexpected exception";
        m_store.UpdateChannel(kind, failed);
    }
    catch (...)
    {
        // Best effort; the original failure may be memory exhaustion.
    }
}

ITMPlugin* GetPluginInstanceInternal()
{
    try { return &CPluginNetwork::Instance(); }
    catch (...) { return nullptr; }
}

void ShutdownPluginInternal()
{
    try { CPluginNetwork::Instance().Shutdown(); }
    catch (...) { }
}

} // namespace pluginnetwork

#if !defined(PLUGINPING_MERGED)
extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return pluginnetwork::GetPluginInstanceInternal();
}

extern "C" __declspec(dllexport) void TMPluginShutdown()
{
    pluginnetwork::ShutdownPluginInternal();
}
#endif
