#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../shared_src/ConfigFile.h"
#include "../shared_src/RoutePlan.h"
#include "../shared_src/RouteEpoch.h"
#include "../network_src/src/ChannelStatePolicy.h"
#include "../ping_src/src/ConfigManager.h"
#include "../ping_src/src/ProxyIpStatePolicy.h"
#include "../ping_src/src/TraceParser.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message)
{
    if (condition)
        return;
    ++failures;
    std::wcerr << L"FAIL: " << message << L'\n';
}

bool WriteBytes(const std::wstring& path, const std::string& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const DWORD expected = static_cast<DWORD>(bytes.size());
    const bool ok = WriteFile(file, bytes.data(), expected, &written, nullptr) != FALSE &&
                    written == expected;
    CloseHandle(file);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    using namespace pluginping_shared;

    RouteOptions options;
    UserProxyState noProxy;
    RoutePlan plan = BuildRoutePlan(options, noProxy);
    Check(plan.http == HttpRoute::Direct, L"Auto without a local proxy must use direct HTTP");
    Check(plan.udp == UdpRoute::Direct, L"Auto without a local proxy must use direct UDP");
    Check(plan.udpSharesHttpIdentity,
          L"Fully direct Auto must share the HTTP/direct egress identity");

    UserProxyState staticProxy;
    staticProxy.staticProxyEnabled = true;
    staticProxy.server = L"http=127.0.0.1:7890;https=127.0.0.1:7890";
    plan = BuildRoutePlan(options, staticProxy);
    Check(plan.http == HttpRoute::Named && plan.httpProxy == L"127.0.0.1:7890",
          L"Auto must use the enabled static proxy for HTTP");
    Check(plan.udp == UdpRoute::Socks5,
          L"Auto mixed proxy endpoints must expose their SOCKS5-capable route");
    Check(plan.udpSharesHttpIdentity,
          L"Auto-derived HTTP and SOCKS routes must share proxy identity");

    staticProxy.server = L"http=127.0.0.1:7890;socks=127.0.0.1:7891";
    plan = BuildRoutePlan(options, staticProxy);
    Check(plan.http == HttpRoute::Named && plan.udp == UdpRoute::Socks5 &&
              !plan.udpSharesHttpIdentity,
          L"Distinct HTTP and SOCKS endpoints must keep independent identities");

    options.shareProxyIdentityForUdp = true;
    plan = BuildRoutePlan(options, staticProxy);
    Check(plan.udpSharesHttpIdentity,
          L"An explicit same-upstream declaration must share split proxy endpoints");
    options.shareProxyIdentityForUdp = false;

    staticProxy.server = L"http=127.0.0.1";
    plan = BuildRoutePlan(options, staticProxy);
    Check(plan.http == HttpRoute::Unavailable && plan.udp == UdpRoute::Unavailable,
          L"Auto must fail closed for an invalid local proxy endpoint");

    staticProxy.server = L"127.0.0.1:7890";
    plan = BuildRoutePlan(options, staticProxy);
    Check(plan.http == HttpRoute::Named && plan.udp == UdpRoute::Socks5,
          L"A mixed proxy endpoint must be shared by HTTP and UDP");

    options.mode = L"Manual";
    options.proxyServer.clear();
    plan = BuildRoutePlan(options, noProxy);
    Check(plan.http == HttpRoute::Unavailable && plan.udp == UdpRoute::Unavailable,
          L"Manual without a server must fail closed");

    RouteOptions machineAuto;
    const RoutePlan machinePlan = ResolveRoutePlan(machineAuto, 1000);
    std::wcout << L"Machine Auto route: HTTP=" << static_cast<int>(machinePlan.http)
               << L", UDP=" << static_cast<int>(machinePlan.udp)
               << L", localProxy=" << (machinePlan.localProxyDetected ? L"yes" : L"no")
               << L'\n';
    if (argc > 1 && wcscmp(argv[1], L"--expect-auto-direct") == 0)
    {
        Check(machinePlan.http == HttpRoute::Direct && machinePlan.udp == UdpRoute::Direct &&
              !machinePlan.localProxyDetected,
              L"This machine is expected to resolve Auto as fully direct");
    }

    using namespace pluginnetwork;
    ChannelStateInput state;
    state.udp = true;
    state.fallbackPending = true;
    state.consecutiveFailures = 3;
    state.maxFailCount = 3;
    Check(EvaluateChannelState(state) == ChannelState::Warming,
          L"An untried UDP fallback must be warming, not down");

    state.fallbackPending = false;
    Check(EvaluateChannelState(state) == ChannelState::Disconnected,
          L"A tried failing fallback must become down");

    ChannelSnapshot metricChannel;
    metricChannel.state = ChannelState::Ok;
    metricChannel.stabilityPct = 100;
    metricChannel.avgRttMs = 42;
    metricChannel.jitterMs = -1;
    Check(ToneForLatencyValue(metricChannel.avgRttMs) == QualityTone::Good,
          L"RTT values through 70 ms must be green");
    Check(ToneForLatencyValue(metricChannel.jitterMs) == QualityTone::Neutral,
          L"An unavailable JIT value must not inherit the RTT colour");
    Check(ToneForLatencyValue(70) == QualityTone::Good &&
              ToneForLatencyValue(71) == QualityTone::Warn &&
              ToneForLatencyValue(149) == QualityTone::Warn &&
              ToneForLatencyValue(150) == QualityTone::Bad,
          L"RTT and JIT must share the 70/150 ms numeric colour boundaries");
    metricChannel.avgRttMs = 180;
    metricChannel.jitterMs = 4;
    Check(ToneForLatencyValue(metricChannel.avgRttMs) == QualityTone::Bad &&
              ToneForLatencyValue(metricChannel.jitterMs) == QualityTone::Good,
          L"JIT must use its own value instead of inheriting the RTT colour");

    state.tcpHealthy = true;
    state.sampleCount = 20;
    state.windowCapacity = 20;
    Check(EvaluateChannelState(state) == ChannelState::Unsupported,
          L"A full UDP failure window with healthy TCP must be unsupported");

    pluginping::CloudflareTraceFields trace;
    Check(pluginping::ParseCloudflareTraceBody("ip=203.0.113.7\r\nloc=us\r\n", trace) &&
              trace.ip == L"203.0.113.7" && trace.loc == L"US",
          L"A valid Cloudflare trace must publish its paired IP and normalized loc");
    Check(!pluginping::ParseCloudflareTraceBody("loc=US\r\n", trace),
          L"A trace without ip must fail closed");
    Check(!pluginping::ParseCloudflareTraceBody("ip=not-an-ip\r\nloc=US", trace),
          L"An invalid trace ip must fail closed");
    Check(pluginping::ParseCloudflareTraceBody("ip=203.0.113.8\r\nloc=U1", trace) &&
              trace.ip == L"203.0.113.8" && trace.loc.empty(),
          L"An invalid loc must not poison the valid trace IP");
    Check(pluginping::ParseCloudflareTraceBody("ip=203.0.113.9\r\nlo", trace) &&
              trace.loc.empty(),
          L"A truncated loc line must keep the valid IP and empty loc");

    std::wstring proxyIp = L"198.51.100.8";
    std::wstring proxyCountry = L"US";
    bool proxyAsnOnly = false;
    pluginping::ApplyProxyIdentityFailure(
        proxyIp, proxyCountry, proxyAsnOnly, pluginping::ProxyFailureKind::Transient);
    Check(proxyIp == L"198.51.100.8" && proxyCountry == L"US",
          L"A transient node-switch failure must retain the last confirmed PROXY identity");
    pluginping::ApplyProxyIdentityFailure(
        proxyIp, proxyCountry, proxyAsnOnly, pluginping::ProxyFailureKind::RejectedRoute);
    Check(proxyIp.empty() && proxyCountry == L"--",
          L"A PROXY identity matching DIRECT must be cleared");
    proxyCountry = L"US";
    pluginping::ApplyProxyIdentityFailure(
        proxyIp, proxyCountry, proxyAsnOnly, pluginping::ProxyFailureKind::Transient);
    Check(proxyIp.empty() && proxyCountry == L"--",
          L"A first transient failure must publish the empty placeholder state");
    proxyIp.clear();
    proxyCountry = L"N/A";
    proxyAsnOnly = true;
    pluginping::ApplyProxyIdentityFailure(
        proxyIp, proxyCountry, proxyAsnOnly, pluginping::ProxyFailureKind::Transient);
    Check(proxyIp.empty() && proxyCountry == L"N/A" && proxyAsnOnly,
          L"A transient failure must retain the one-token loc=CN display");
    pluginping::ApplyProxyIdentityFailure(
        proxyIp, proxyCountry, proxyAsnOnly, pluginping::ProxyFailureKind::RejectedRoute);
    Check(proxyIp.empty() && proxyCountry == L"--" && !proxyAsnOnly,
          L"A rejected PROXY route must clear the loc=CN display state");
    int legacyDirectInterval = 30000;
    int legacyProxyInterval = 5000;
    pluginping::MigrateLegacyResolveIntervals(
        1, legacyDirectInterval, legacyProxyInterval);
    Check(legacyDirectInterval == 10000 && legacyProxyInterval == 1000,
          L"ConfigVersion 1 defaults must migrate to the fast resolve cadence");
    int explicitDirectInterval = 30000;
    int explicitProxyInterval = 5000;
    pluginping::MigrateLegacyResolveIntervals(
        2, explicitDirectInterval, explicitProxyInterval);
    Check(explicitDirectInterval == 30000 && explicitProxyInterval == 5000,
          L"ConfigVersion 2 must preserve explicit resolve intervals");

    Check(MakeProxyIdentity(L"203.0.113.7") == L"proxy|203.0.113.7",
          L"A confirmed trace must identify its proxy egress IP");
    Check(MakeProxyIdentity(L"203.0.113.7") == MakeProxyIdentity(L"203.0.113.7"),
          L"Country-only trace changes must not invalidate the route identity");
    Check(MakeDirectLeakIdentity(L"203.0.113.7") == L"direct|203.0.113.7" &&
              MakeDirectLeakIdentity(L"203.0.113.7") != MakeProxyIdentity(L"203.0.113.7"),
          L"A direct leak must retain its raw IP and distinct route status");
    Check(MakeProxyIdentity(L"").empty() && MakeDirectLeakIdentity(L"").empty(),
          L"An observation without an egress IP must not form an identity");

    RouteEpoch& epoch = RouteEpoch::Instance();
    Check(epoch.Epoch() == 0, L"No confirmed observation must leave the epoch unset");
    Check(epoch.PublishProxyIdentity(L"") == 0,
          L"An unconfirmed observation must not advance the epoch");
    Check(epoch.PublishProxyIdentity(L"proxy|203.0.113.7") == 1 &&
              epoch.ProxyEpoch() == 1,
          L"The first confirmed identity must invalidate transports opened before confirmation");
    Check(epoch.PublishProxyIdentity(L"proxy|203.0.113.7") == 1,
          L"An unchanged node must not rebuild the probe transports");
    Check(epoch.PublishProxyIdentity(L"") == 1,
          L"A transient failure retaining the previous identity must not rebuild");
    Check(epoch.PublishProxyIdentity(L"proxy|198.51.100.4") == 2,
          L"A node switch must advance the epoch exactly once");
    Check(epoch.PublishProxyIdentity(L"direct|198.51.100.4") == 3,
          L"A tunnel leaking to the DIRECT egress must rebuild the probe transports");
    Check(epoch.PublishProxyIdentity(L"proxy|198.51.100.4") == 4 &&
              epoch.Identity() == L"proxy|198.51.100.4",
          L"Recovering from a leak back to a node must advance and record the identity");

    wchar_t tempDirectory[MAX_PATH]{};
    wchar_t tempFile[MAX_PATH]{};
    const bool tempReady = GetTempPathW(MAX_PATH, tempDirectory) != 0 &&
                           GetTempFileNameW(tempDirectory, L"ppg", 0, tempFile) != 0;
    Check(tempReady, L"The shared INI snapshot test needs a temporary file");
    if (tempReady)
    {
        const std::wstring path = tempFile;
        Check(WriteBytes(path, "[General]\r\nRefreshInterval=1000\r\n[Style]\r\nCompactMode=false\r\n"),
              L"The initial shared INI test file must be writable");
        const auto first = GetIniSnapshot(path);
        const auto same = GetIniSnapshot(path);
        Check(first == same && first->Integer(L"General", L"RefreshInterval", 0) == 1000 &&
                  !first->Boolean(L"Style", L"CompactMode", true),
              L"Unchanged readers must receive the same immutable INI generation");
        Check(WriteBytes(path, "[General]\r\nRefreshInterval=2500\r\n[Style]\r\nCompactMode=true\r\n"),
              L"The updated shared INI test file must be writable");
        const auto second = GetIniSnapshot(path);
        Check(second->Generation() == first->Generation() + 1 &&
                  second->Integer(L"General", L"RefreshInterval", 0) == 2500 &&
                  second->Boolean(L"Style", L"CompactMode", false),
              L"A complete INI save must publish exactly one new shared generation");
        DeleteFileW(path.c_str());
    }

    if (failures != 0)
        return 1;
    std::wcout << L"PluginPing core route/state/config/trace tests passed.\n";
    return 0;
}
