// PluginPing - DIRECT/PROXY egress IP worker and internal adapter.
#include "Common.h"
#include "PluginPing.h"
#include "ProxyIpStatePolicy.h"
#include "PublicIpProvider.h"
#include "../../shared_src/RouteEpoch.h"

#include <algorithm>
#include <string>
#include <utility>

namespace pluginping {

namespace {

constexpr unsigned long long kDebugLogMinIntervalMs = 1000;
constexpr unsigned long long kDebugLogMaxBytes = 5ull * 1024 * 1024;
constexpr DWORD kIpWorkerRetryBackoffMs = 5000;

struct EndpointRuntime
{
    std::wstring ip;
    std::wstring country;
    bool displayAsnOnly = false;
    std::wstring diagnostic;
    int failures = 0;
};

void ApplyEndpointRuntime(const EndpointRuntime& runtime, EndpointStatus& output)
{
    output.ipAddress = runtime.displayAsnOnly ? L"" :
                       (runtime.ip.empty() ? L"--" : runtime.ip);
    output.countryCode = runtime.displayAsnOnly ? L"N/A" :
                         (runtime.country.empty() ? L"--" : runtime.country);
    output.displayAsnOnly = runtime.displayAsnOnly;
    output.ipDiagnostic = runtime.diagnostic;
}

const wchar_t* StageName(NetworkStage stage)
{
    switch (stage)
    {
    case NetworkStage::PrimaryIp:  return L"primary-ip";
    case NetworkStage::FallbackIp: return L"fallback-ip";
    default:                       return L"none";
    }
}

std::wstring FormatDiagnostic(const wchar_t* prefix, const NetworkDiagnostics& diag, bool ok)
{
    std::wstring out = prefix ? prefix : L"IP";
    out += ok ? L" ok " : L" fail ";
    out += StageName(diag.stage);
    if (!diag.target.empty())
        out += L" " + diag.target;
    if (diag.status != 0)
        out += L" HTTP " + std::to_wstring(diag.status);
    if (diag.error != 0)
        out += L" err " + std::to_wstring(diag.error);
    if (diag.elapsedMs >= 0)
        out += L" " + std::to_wstring(diag.elapsedMs) + L" ms";
    if (!diag.proxy.empty())
        out += L" via " + diag.proxy;
    return out;
}

std::wstring FormatSourceSummary(const wchar_t* name, const std::wstring& ip,
                                 const NetworkDiagnostics& diag, bool ok)
{
    std::wstring out = FormatDiagnostic(name, diag, ok);
    if (ok && !ip.empty())
        out += L" ip " + ip;
    return out;
}

std::wstring MakeTooltip(const LocalRemoteSnapshot& snapshot, bool showDirect, bool showProxy)
{
    std::wstring text;
    text.reserve(128);
    if (showDirect)
    {
        text += L"DIRECT: ";
        text += snapshot.local.countryCode.empty() ? L"--" : snapshot.local.countryCode;
        text += L" | ";
        text += snapshot.local.ipAddress.empty() ? L"--" : snapshot.local.ipAddress;
    }
    if (showProxy)
    {
        if (!text.empty())
            text += L"\r\n";
        text += L"PROXY: ";
        text += snapshot.remote.countryCode.empty() ? L"--" : snapshot.remote.countryCode;
        if (!snapshot.remote.displayAsnOnly)
        {
            text += L" | ";
            text += snapshot.remote.ipAddress.empty() ? L"--" : snapshot.remote.ipAddress;
        }
    }
    return text;
}

std::wstring Timestamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth,
               st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}

std::string Utf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 1)
        return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), needed, nullptr, nullptr);
    out.resize(static_cast<size_t>(needed - 1));
    return out;
}

void AppendDebugLog(const std::wstring& path, const LocalRemoteSnapshot& snapshot)
{
    if (path.empty())
        return;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
    {
        const unsigned long long size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
                                         data.nFileSizeLow;
        if (size >= kDebugLogMaxBytes)
            MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    const pluginping_shared::RouteEpoch& epoch = pluginping_shared::RouteEpoch::Instance();
    const std::wstring proxyIdentity = epoch.Identity();
    const std::wstring networkIdentity = epoch.NetworkIdentity();
    const std::wstring entry = Timestamp() + L"\r\nDIRECT ip=" +
        (snapshot.local.ipAddress.empty() ? L"--" : snapshot.local.ipAddress) +
        L" country=" + (snapshot.local.countryCode.empty() ? L"--" : snapshot.local.countryCode) +
        L"\r\nPROXY ip=" +
        (snapshot.remote.ipAddress.empty() ? L"--" : snapshot.remote.ipAddress) +
        L" country=" + (snapshot.remote.countryCode.empty() ? L"--" : snapshot.remote.countryCode) +
        L"\r\nDIRECT diagnostic=" +
        (snapshot.local.ipDiagnostic.empty() ? L"--" : snapshot.local.ipDiagnostic) +
        L"\r\nPROXY diagnostic=" +
        (snapshot.remote.ipDiagnostic.empty() ? L"--" : snapshot.remote.ipDiagnostic) +
        L"\r\nproxy identity=" +
        (proxyIdentity.empty() ? L"unconfirmed" : proxyIdentity) +
        L"\r\nnetwork identity=" +
        (networkIdentity.empty() ? L"unobserved" : networkIdentity) +
        L"\r\nproxy epoch=" + std::to_wstring(epoch.ProxyEpoch()) +
        L" network epoch=" + std::to_wstring(epoch.NetworkEpoch()) +
        L"\r\n\r\n";
    const std::string bytes = Utf8(entry);
    if (bytes.empty())
        return;
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

void PinModuleForWorkerLifetime()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&PinModuleForWorkerLifetime), &self);
}

unsigned long long EffectiveResolveInterval(int configuredMs,
                                            const EndpointRuntime& runtime,
                                            const PluginConfig& cfg)
{
    const unsigned long long normal = static_cast<unsigned long long>((std::max)(configuredMs, 1));
    if (runtime.failures < cfg.failThreshold)
        return normal;
    return (std::max)(normal, static_cast<unsigned long long>(cfg.ipFailureBackoffMs));
}

bool IsRecentlyDisplayed(unsigned long long lastDraw, const PluginConfig& cfg,
                         unsigned long long now)
{
    return lastDraw != 0 &&
           (cfg.idleAfterMs == 0 ||
            now - lastDraw < static_cast<unsigned long long>(cfg.idleAfterMs));
}

void SetProxyFailure(const std::wstring& diagnostic, EndpointRuntime& runtime,
                     EndpointStatus& output, ProxyFailureKind kind)
{
    ++runtime.failures;
    runtime.diagnostic = diagnostic;
    ApplyProxyIdentityFailure(runtime.ip, runtime.country, runtime.displayAsnOnly, kind);
    ApplyEndpointRuntime(runtime, output);
}

void RefreshDirect(const PluginConfig& cfg, EndpointRuntime& runtime, EndpointStatus& output,
                   HttpRequestCancellation* cancellation)
{
    if (!cfg.localDisplayIp.empty())
    {
        runtime.ip = cfg.localDisplayIp;
        runtime.country = L"CN";
        runtime.displayAsnOnly = false;
        runtime.failures = 0;
        runtime.diagnostic = L"Direct IP display override ip " + runtime.ip;
        ApplyEndpointRuntime(runtime, output);
        return;
    }

    std::wstring primaryIp;
    std::wstring fallbackIp;
    NetworkDiagnostics primaryDiag;
    NetworkDiagnostics fallbackDiag;
    const bool primaryOk = QueryBilibiliPrimaryIp(primaryIp, cfg,
                                                  EndpointProxyPolicy::DirectNoProxy,
                                                  &primaryDiag, cancellation);
    // The fallback is a fallback in the literal sense: do not add a second
    // public-IP request to every successful DIRECT refresh.
    const bool fallbackOk = !primaryOk && QueryPconlineIp(fallbackIp, cfg,
                                                           EndpointProxyPolicy::DirectNoProxy,
                                                           &fallbackDiag, cancellation);

    const std::wstring& selected = primaryOk ? primaryIp : fallbackIp;
    std::wstring diagnostic = L"Direct IP; " +
        FormatSourceSummary(L"primary", primaryIp, primaryDiag, primaryOk);
    if (!primaryOk)
        diagnostic += L"; " +
            FormatSourceSummary(L"fallback", fallbackIp, fallbackDiag, fallbackOk);

    if (selected.empty())
    {
        ++runtime.failures;
        runtime.ip.clear();
        runtime.country = L"CN";
        runtime.displayAsnOnly = false;
        runtime.diagnostic = diagnostic;
        ApplyEndpointRuntime(runtime, output);
        return;
    }

    runtime.ip = selected;
    runtime.country = L"CN";
    runtime.displayAsnOnly = false;
    runtime.failures = 0;
    runtime.diagnostic = diagnostic;
    ApplyEndpointRuntime(runtime, output);
}

bool RefreshProxy(const PluginConfig& cfg, EndpointRuntime& runtime, EndpointStatus& output,
                  HttpRequestCancellation* cancellation, std::wstring& identityOut,
                  std::wstring& rawIpOut)
{
    std::wstring ip;
    std::wstring loc;
    identityOut.clear();
    rawIpOut.clear();
    NetworkDiagnostics diag;
    const bool ok = QueryCloudflareTraceIp(ip, loc, cfg,
                                          EndpointProxyPolicy::ConfiguredProxy, &diag,
                                          cancellation);
    const std::wstring diagnostic = FormatDiagnostic(L"Proxy IP", diag, ok) +
                                    (ok && !ip.empty() ? L" ip " + ip : L"");
    if (!ok || ip.empty())
    {
        // A proxy-node switch can briefly tear down the active tunnel. Keep
        // the last Cloudflare-confirmed result visible while the normal retry
        // cadence obtains a result from the new node.
        SetProxyFailure(diagnostic, runtime, output, ProxyFailureKind::Transient);
        return false;
    }

    // Reported before the loc=CN display collapse below, so the probe workers
    // see the node this measurement actually traversed.
    identityOut = pluginping_shared::MakeProxyIdentity(ip);
    rawIpOut = ip;

    const bool china = _wcsicmp(loc.c_str(), L"CN") == 0;
    runtime.displayAsnOnly = china;
    runtime.ip = china ? L"" : ip;
    runtime.country = china ? L"N/A" : (loc.empty() ? L"--" : loc);
    runtime.failures = 0;
    runtime.diagnostic = diagnostic + (loc.empty() ? L" loc --" : L" loc " + loc);
    ApplyEndpointRuntime(runtime, output);
    return true;
}

} // namespace

CPluginPing::CPluginPing()
    : m_localItem(PingEndpoint::Local), m_remoteItem(PingEndpoint::Remote)
{
    auto widthState = std::make_shared<EndpointWidthState>();
    m_localItem.Attach(&m_store, &m_config, widthState);
    m_remoteItem.Attach(&m_store, &m_config, std::move(widthState));

    LocalRemoteSnapshot initial;
    initial.local.label = L"DIRECT";
    initial.local.countryCode = L"CN";
    initial.local.ipAddress = L"--";
    initial.local.ipDiagnostic = L"DIRECT pending initial probe";
    initial.remote.label = L"PROXY";
    initial.remote.countryCode = L"--";
    initial.remote.ipAddress = L"--";
    initial.remote.ipDiagnostic = L"PROXY pending initial probe";
    m_store.UpdateSnapshot(initial);
}

CPluginPing::~CPluginPing() { StopWorkers(); }

CPluginPing& CPluginPing::Instance()
{
    static CPluginPing* instance = new CPluginPing();
    return *instance;
}

void CPluginPing::Shutdown()
{
    m_shutdown.store(true);
    StopWorkers();
}

IPluginItem* CPluginPing::GetItem(int index)
{
    if (index == 0) return &m_localItem;
    if (index == 1) return &m_remoteItem;
    return nullptr;
}

void CPluginPing::DataRequired()
{
    try
    {
        if (!HasDisplayedItem())
            return;
        EnsureWorkersStarted();
    }
    catch (...)
    {
    }
}

const wchar_t* CPluginPing::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:        return L"PluginPing IP internal";
    case TMI_DESCRIPTION: return L"Shows DIRECT and PROXY egress IP and route code.";
    case TMI_AUTHOR:      return L"PluginPing contributors";
    case TMI_COPYRIGHT:   return L"Copyright (C) 2026 PluginPing contributors";
    case TMI_VERSION:     return kPluginPingVersion;
    case TMI_URL:         return L"";
    default:              return L"";
    }
}

const wchar_t* CPluginPing::GetTooltipInfo()
{
    thread_local std::wstring copy;
    try
    {
        if (!HasDisplayedItem())
            return L"";
        const auto snapshot = m_store.GetSnapshotShared();
        copy = MakeTooltip(*snapshot, m_localItem.LastDrawTick() != 0,
                           m_remoteItem.LastDrawTick() != 0);
        return copy.c_str();
    }
    catch (...)
    {
        return L"";
    }
}

void CPluginPing::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    try
    {
        if (index == EI_CONFIG_DIR && data)
            m_config.SetConfigDir(data);
    }
    catch (...)
    {
    }
}

void CPluginPing::PublishEndpoint(PingEndpoint endpoint, const EndpointStatus& status,
                                  const PluginConfig& cfg)
{
    if (endpoint == PingEndpoint::Local)
        m_store.UpdateLocal(status);
    else
        m_store.UpdateRemote(status);

    if (cfg.debugLog)
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        const unsigned long long now = GetTickCount64();
        const unsigned long long previous = m_lastDebugLogWrite;
        if (previous == 0 || now - previous >= kDebugLogMinIntervalMs)
        {
            AppendDebugLog(m_config.DefaultSiblingFile(L"PluginPing_debug.log"),
                           m_store.GetSnapshot());
            m_lastDebugLogWrite = now;
        }
    }
}

bool CPluginPing::HasDisplayedItem() const
{
    return m_localItem.LastDrawTick() != 0 || m_remoteItem.LastDrawTick() != 0;
}

void CPluginPing::EnsureWorkersStarted()
{
    if (m_shutdown.load())
        return;
    std::lock_guard<std::mutex> lock(m_workerMutex);
    if (m_shutdown.load() || m_running.load())
        return;
    m_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_wake)
        return;
    m_directHttpCancellation.Reset();
    m_proxyHttpCancellation.Reset();
    m_directRevalidateRequest.store(0);
    m_directRevalidateCompleted.store(0);
    m_running.store(true);
    PinModuleForWorkerLifetime();

    try
    {
        m_directWorker = std::thread(&CPluginPing::DirectWorkerLoop, this);
        m_proxyWorker = std::thread(&CPluginPing::ProxyWorkerLoop, this);
    }
    catch (...)
    {
        m_running.store(false);
        SetEvent(m_wake);
        m_directHttpCancellation.Cancel();
        m_proxyHttpCancellation.Cancel();
        if (m_directWorker.joinable())
            m_directWorker.join();
        if (m_proxyWorker.joinable())
            m_proxyWorker.join();
        CloseHandle(m_wake);
        m_wake = nullptr;
        return;
    }
}

void CPluginPing::StopWorkers()
{
    std::lock_guard<std::mutex> lock(m_workerMutex);
    m_running.store(false);
    if (m_wake)
        SetEvent(m_wake);
    m_directHttpCancellation.Cancel();
    m_proxyHttpCancellation.Cancel();
    if (m_directWorker.joinable())
        m_directWorker.join();
    if (m_proxyWorker.joinable())
        m_proxyWorker.join();
    if (m_wake)
    {
        CloseHandle(m_wake);
        m_wake = nullptr;
    }
}

void CPluginPing::PublishWorkerFailure(PingEndpoint endpoint) noexcept
{
    try
    {
        EndpointStatus failed = endpoint == PingEndpoint::Local
            ? m_store.GetSnapshot().local
            : m_store.GetSnapshot().remote;
        failed.ipAddress = L"--";
        failed.ipDiagnostic = endpoint == PingEndpoint::Local
            ? L"DIRECT worker retrying after an unexpected exception"
            : L"PROXY worker retrying after an unexpected exception";
        if (endpoint == PingEndpoint::Remote)
        {
            failed.countryCode = L"--";
            failed.displayAsnOnly = false;
        }
        const auto cfg = m_config.GetShared();
        PublishEndpoint(endpoint, failed, *cfg);
    }
    catch (...)
    {
    }
}

void CPluginPing::DirectWorkerLoop()
{
    while (m_running.load())
    {
        try
        {
            EndpointRuntime direct;
            ULONGLONG lastDirect = 0;
            unsigned long long observedNetworkEpoch = 0;
            std::shared_ptr<const PluginConfig> observedConfig;
            unsigned long long observedRevalidateRequest = m_directRevalidateRequest.load();

            while (m_running.load())
            {
                const auto config = m_config.Reload();
                const bool reloaded = config != observedConfig;
                observedConfig = config;
                const PluginConfig& cfg = *config;
                pluginping_shared::RouteEpoch& routeEpoch = pluginping_shared::RouteEpoch::Instance();
                routeEpoch.ObserveLocalRoute();
                const unsigned long long networkEpoch = routeEpoch.NetworkEpoch();
                const unsigned long long revalidateRequest = m_directRevalidateRequest.load();
                if (networkEpoch != observedNetworkEpoch || revalidateRequest != observedRevalidateRequest)
                {
                    lastDirect = 0;
                    direct.failures = 0;
                    observedNetworkEpoch = networkEpoch;
                    observedRevalidateRequest = revalidateRequest;
                }

                const ULONGLONG now = GetTickCount64();
                const bool identityNeeded = IsRecentlyDisplayed(m_localItem.LastDrawTick(), cfg, now) ||
                                            IsRecentlyDisplayed(m_remoteItem.LastDrawTick(), cfg, now);
                const ULONGLONG directInterval = EffectiveResolveInterval(cfg.localResolveIntervalMs, direct, cfg);
                const bool refreshDirect =
                    identityNeeded && (reloaded || lastDirect == 0 || now - lastDirect >= directInterval);
                if (refreshDirect)
                {
                    EndpointStatus local = m_store.GetSnapshot().local;
                    local.label = L"DIRECT";
                    const EndpointRuntime runtimeBeforeRequest = direct;
                    const unsigned long long requestNetworkEpoch = networkEpoch;
                    RefreshDirect(cfg, direct, local, &m_directHttpCancellation);
                    if (!m_running.load())
                        return;
                    routeEpoch.ObserveLocalRouteNow();
                    if (routeEpoch.NetworkEpoch() != requestNetworkEpoch)
                    {
                        direct = runtimeBeforeRequest;
                        lastDirect = 0;
                        continue;
                    }
                    lastDirect = GetTickCount64();
                    PublishEndpoint(PingEndpoint::Local, local, cfg);
                    // A failed fresh attempt publishes "--" before releasing the
                    // proxy gate, so unavailable DIRECT services cannot freeze a
                    // valid Cloudflare route identity indefinitely.
                    m_directRevalidateCompleted.store(observedRevalidateRequest);
                }

                const ULONGLONG waitNow = GetTickCount64();
                const ULONGLONG nextInterval = EffectiveResolveInterval(cfg.localResolveIntervalMs, direct, cfg);
                const ULONGLONG nextDirect = identityNeeded ? lastDirect + nextInterval : waitNow + 1000;
                ULONGLONG waitMs = waitNow >= nextDirect ? 0 : nextDirect - waitNow;
                waitMs = (std::min)(waitMs, 1000ull);
                WaitForSingleObject(m_wake, static_cast<DWORD>(waitMs));
            }
            return;
        }
        catch (...)
        {
            PublishWorkerFailure(PingEndpoint::Local);
            if (!m_running.load())
                return;
            if (WaitForSingleObject(m_wake, kIpWorkerRetryBackoffMs) == WAIT_OBJECT_0)
                return;
        }
    }
}

void CPluginPing::ProxyWorkerLoop()
{
    while (m_running.load())
    {
        try
        {
            EndpointRuntime proxy;
            ULONGLONG lastProxy = 0;
            unsigned long long observedNetworkEpoch = 0;
            std::shared_ptr<const PluginConfig> observedConfig;
            std::wstring pendingIdentity;
            std::wstring pendingRawIp;
            unsigned long long pendingRevalidateRequest = 0;
            const auto clearPendingIdentity = [&]() {
                pendingIdentity.clear();
                pendingRawIp.clear();
                pendingRevalidateRequest = 0;
            };

            while (m_running.load())
            {
                const auto config = m_config.Reload();
                const bool reloaded = config != observedConfig;
                observedConfig = config;
                const PluginConfig& cfg = *config;
                pluginping_shared::RouteEpoch& routeEpoch = pluginping_shared::RouteEpoch::Instance();
                routeEpoch.ObserveLocalRoute();
                const unsigned long long networkEpoch = routeEpoch.NetworkEpoch();
                if (networkEpoch != observedNetworkEpoch)
                {
                    lastProxy = 0;
                    proxy.failures = 0;
                    clearPendingIdentity();
                    observedNetworkEpoch = networkEpoch;
                }
                if (reloaded)
                    clearPendingIdentity();

                const ULONGLONG now = GetTickCount64();
                // TCP is embedded in the DIRECT row but follows the proxy route.
                const bool proxyIdentityNeeded = IsRecentlyDisplayed(m_localItem.LastDrawTick(), cfg, now) ||
                                                 IsRecentlyDisplayed(m_remoteItem.LastDrawTick(), cfg, now);
                const ULONGLONG proxyInterval = static_cast<unsigned long long>(
                    (std::max)(cfg.remoteResolveIntervalMs, 1));
                const bool pendingReady =
                    !pendingIdentity.empty() && m_directRevalidateCompleted.load() >= pendingRevalidateRequest;
                const bool refreshProxy = proxyIdentityNeeded && (pendingReady || reloaded || lastProxy == 0 ||
                                                                  now - lastProxy >= proxyInterval);
                if (refreshProxy)
                {
                    EndpointStatus remote = m_store.GetSnapshot().remote;
                    remote.label = L"PROXY";
                    std::wstring proxyIdentity;
                    std::wstring proxyRawIp;
                    const EndpointRuntime runtimeBeforeRequest = proxy;
                    const unsigned long long requestNetworkEpoch = networkEpoch;
                    const int failuresBeforeRefresh = proxy.failures;
                    bool proxyConfirmed = false;
                    if (pendingReady)
                    {
                        proxyIdentity = pendingIdentity;
                        proxyRawIp = pendingRawIp;
                        remote.ipDiagnostic = proxy.diagnostic;
                    }
                    else
                    {
                        proxyConfirmed =
                            RefreshProxy(cfg, proxy, remote, &m_proxyHttpCancellation, proxyIdentity, proxyRawIp);
                    }
                    if (!m_running.load())
                        return;
                    routeEpoch.ObserveLocalRouteNow();
                    if (routeEpoch.NetworkEpoch() != requestNetworkEpoch)
                    {
                        proxy = runtimeBeforeRequest;
                        lastProxy = 0;
                        clearPendingIdentity();
                        continue;
                    }

                    if (proxyConfirmed && !proxyIdentity.empty())
                    {
                        const std::wstring currentIdentity = routeEpoch.Identity();
                        const bool rawIdentityChanged =
                            currentIdentity != proxyIdentity &&
                            currentIdentity != pluginping_shared::MakeDirectLeakIdentity(proxyRawIp);
                        if (rawIdentityChanged && pendingIdentity != proxyIdentity)
                        {
                            pendingIdentity = proxyIdentity;
                            pendingRawIp = proxyRawIp;
                            pendingRevalidateRequest = m_directRevalidateRequest.fetch_add(1) + 1;
                        }
                        if (rawIdentityChanged && m_directRevalidateCompleted.load() < pendingRevalidateRequest)
                        {
                            remote.ipDiagnostic = proxy.diagnostic + L"; awaiting DIRECT revalidation";
                            proxyIdentity.clear();
                        }
                    }

                    // DIRECT completion validates the already confirmed Cloudflare
                    // result. Do not require another successful trace merely to
                    // consume that completed validation.
                    if (proxyIdentity.empty() && !pendingIdentity.empty() &&
                        m_directRevalidateCompleted.load() >= pendingRevalidateRequest)
                    {
                        proxyIdentity = pendingIdentity;
                        proxyRawIp = pendingRawIp;
                    }

                    if (!proxyIdentity.empty())
                    {
                        const LocalRemoteSnapshot latest = m_store.GetSnapshot();
                        if (!proxyRawIp.empty() && cfg.localDisplayIp.empty() &&
                            !latest.local.ipAddress.empty() && latest.local.ipAddress != L"--" &&
                            proxyRawIp == latest.local.ipAddress)
                        {
                            proxy.diagnostic += L"; proxy egress matches DIRECT";
                            SetProxyFailure(proxy.diagnostic, proxy, remote,
                                            ProxyFailureKind::RejectedRoute);
                            proxy.failures = (std::min)(failuresBeforeRefresh + 1,
                                                        cfg.failThreshold);
                            proxyIdentity = pluginping_shared::MakeDirectLeakIdentity(proxyRawIp);
                        }
                        clearPendingIdentity();
                    }

                    routeEpoch.PublishProxyIdentity(proxyIdentity);
                    lastProxy = GetTickCount64();
                    PublishEndpoint(PingEndpoint::Remote, remote, cfg);
                }

                const ULONGLONG waitNow = GetTickCount64();
                const ULONGLONG nextProxy = proxyIdentityNeeded
                    ? lastProxy + proxyInterval
                    : waitNow + 1000;
                ULONGLONG waitMs = waitNow >= nextProxy ? 0 : nextProxy - waitNow;
                waitMs = (std::min)(waitMs, 1000ull);
                WaitForSingleObject(m_wake, static_cast<DWORD>(waitMs));
            }
            return;
        }
        catch (...)
        {
            PublishWorkerFailure(PingEndpoint::Remote);
            if (!m_running.load())
                return;
            if (WaitForSingleObject(m_wake, kIpWorkerRetryBackoffMs) == WAIT_OBJECT_0)
                return;
        }
    }
}

ITMPlugin* GetPluginInstanceInternal()
{
    try { return &CPluginPing::Instance(); }
    catch (...) { return nullptr; }
}

void ShutdownPluginInternal()
{
    try { CPluginPing::Instance().Shutdown(); }
    catch (...) { }
}

} // namespace pluginping

#if !defined(PLUGINPING_MERGED)
extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return pluginping::GetPluginInstanceInternal();
}

extern "C" __declspec(dllexport) void TMPluginShutdown()
{
    pluginping::ShutdownPluginInternal();
}
#endif
