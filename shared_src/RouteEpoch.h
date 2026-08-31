// PluginPing - shared proxy-identity change signal.
#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace pluginping_shared {

// Builds identities from the raw Cloudflare egress IP. Route status is part of
// the identity, so entering or leaving a direct leak rebuilds the transports,
// while a country-code-only change does not invalidate RTT samples.
std::wstring MakeProxyIdentity(const std::wstring& ip);
std::wstring MakeDirectLeakIdentity(const std::wstring& ip);

// Process-wide route generations shared by the IP, TCP and UDP workers.
// ProxyEpoch tracks confirmed HTTP egress changes. NetworkEpoch tracks the
// Windows best-route signature, so an interface/default-route switch still
// rebuilds transports when the public egress IP happens to stay unchanged.
class RouteEpoch
{
public:
    static RouteEpoch& Instance();

    RouteEpoch(const RouteEpoch&) = delete;
    RouteEpoch& operator=(const RouteEpoch&) = delete;

    // Records one confirmed proxy observation. Empty identities are ignored;
    // every new confirmed identity, including the first, advances ProxyEpoch
    // because probe transports may already have opened while confirmation was
    // pending. Returns the process-wide epoch after the call.
    unsigned long long PublishProxyIdentity(const std::wstring& identity);

    // Polls the current IPv4/IPv6 best-route signature at a bounded cadence.
    // The first successful observation seeds the signature; later changes
    // advance NetworkEpoch. Returns the process-wide epoch after the call.
    unsigned long long ObserveLocalRoute();

    // Performs an immediate best-route query regardless of the polling
    // throttle. Used to reject network results that cross a route change.
    unsigned long long ObserveLocalRouteNow();

    // Invalidates the bounded poll after a Windows route notification. The
    // next observer performs the best-route query immediately.
    void InvalidateLocalRoute();

    // Monotonic process-wide count, used for diagnostics and interruptible
    // waits. Probe target keys consume the channel-relevant counters below.
    unsigned long long Epoch() const { return m_epoch.load(std::memory_order_acquire); }
    unsigned long long ProxyEpoch() const
    {
        return m_proxyEpoch.load(std::memory_order_acquire);
    }
    unsigned long long NetworkEpoch() const
    {
        return m_networkEpoch.load(std::memory_order_acquire);
    }

    std::wstring Identity() const;
    std::wstring NetworkIdentity() const;

private:
    RouteEpoch() = default;
    unsigned long long PublishNetworkIdentity(const std::wstring& identity);

    mutable std::mutex m_mutex;
    std::mutex m_observeMutex;
    std::wstring m_identity;
    std::wstring m_networkIdentity;
    std::atomic<unsigned long long> m_epoch{ 0 };
    std::atomic<unsigned long long> m_proxyEpoch{ 0 };
    std::atomic<unsigned long long> m_networkEpoch{ 0 };
    std::atomic<unsigned long long> m_lastNetworkPollTick{ 0 };
};

} // namespace pluginping_shared
