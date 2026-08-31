// PluginPing - shared route-generation implementation.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include "RouteEpoch.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace pluginping_shared {

namespace {

constexpr unsigned long long kNetworkRoutePollIntervalMs = 250;

std::wstring AddressText(const SOCKADDR_INET& address)
{
    wchar_t buffer[INET6_ADDRSTRLEN]{};
    if (address.si_family == AF_INET &&
        InetNtopW(AF_INET, &address.Ipv4.sin_addr,
                  buffer, ARRAYSIZE(buffer)))
        return buffer;
    if (address.si_family == AF_INET6 &&
        InetNtopW(AF_INET6, &address.Ipv6.sin6_addr,
                  buffer, ARRAYSIZE(buffer)))
        return buffer;
    return L"--";
}

void AppendBestRoute(std::wstring& identity, ADDRESS_FAMILY family,
                     const wchar_t* target, const wchar_t* label)
{
    SOCKADDR_INET destination{};
    destination.si_family = family;
    void* address = family == AF_INET
                        ? static_cast<void*>(&destination.Ipv4.sin_addr)
                        : static_cast<void*>(&destination.Ipv6.sin6_addr);

    identity += label;
    identity += L"=";
    if (InetPtonW(family, target, address) != 1)
    {
        identity += L"invalid";
        return;
    }

    MIB_IPFORWARD_ROW2 route{};
    SOCKADDR_INET source{};
    if (GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source) != NO_ERROR)
    {
        identity += L"none";
        return;
    }

    identity += L"if:" + std::to_wstring(route.InterfaceLuid.Value);
    identity += L",src:" + AddressText(source);
    identity += L",hop:" + AddressText(route.NextHop);
}

std::wstring CurrentNetworkRouteIdentity()
{
    std::wstring identity;
    AppendBestRoute(identity, AF_INET, L"1.1.1.1", L"v4");
    identity += L";";
    AppendBestRoute(identity, AF_INET6, L"2606:4700:4700::1111", L"v6");
    return identity;
}

} // namespace

std::wstring MakeProxyIdentity(const std::wstring& ip)
{
    if (ip.empty())
        return std::wstring();
    return L"proxy|" + ip;
}

std::wstring MakeDirectLeakIdentity(const std::wstring& ip)
{
    if (ip.empty())
        return std::wstring();
    return L"direct|" + ip;
}

RouteEpoch& RouteEpoch::Instance()
{
    // Process-lifetime by design: the probe workers read this on every round
    // and the IP worker writes it, so it must outlive both of their teardowns.
    static RouteEpoch* instance = new RouteEpoch();
    return *instance;
}

unsigned long long RouteEpoch::PublishProxyIdentity(const std::wstring& identity)
{
    if (identity.empty())
        return m_epoch.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_identity == identity)
        return m_epoch.load(std::memory_order_relaxed);

    m_identity = identity;
    m_proxyEpoch.fetch_add(1, std::memory_order_acq_rel);
    return m_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
}

unsigned long long RouteEpoch::ObserveLocalRoute()
{
    const unsigned long long now = GetTickCount64();
    unsigned long long previous = m_lastNetworkPollTick.load(std::memory_order_relaxed);
    for (;;)
    {
        if (previous != 0 && now - previous < kNetworkRoutePollIntervalMs)
            return m_epoch.load(std::memory_order_acquire);
        if (m_lastNetworkPollTick.compare_exchange_weak(previous, now,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_relaxed))
            break;
    }

    return ObserveLocalRouteNow();
}

unsigned long long RouteEpoch::ObserveLocalRouteNow()
{
    std::lock_guard<std::mutex> observeLock(m_observeMutex);
    m_lastNetworkPollTick.store(GetTickCount64(), std::memory_order_release);
    return PublishNetworkIdentity(CurrentNetworkRouteIdentity());
}

void RouteEpoch::InvalidateLocalRoute()
{
    m_lastNetworkPollTick.store(0, std::memory_order_release);
}

unsigned long long RouteEpoch::PublishNetworkIdentity(const std::wstring& identity)
{
    if (identity.empty())
        return m_epoch.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_networkIdentity == identity)
        return m_epoch.load(std::memory_order_relaxed);

    const bool seeding = m_networkIdentity.empty();
    m_networkIdentity = identity;
    if (seeding)
        return m_epoch.load(std::memory_order_relaxed);

    m_networkEpoch.fetch_add(1, std::memory_order_acq_rel);
    return m_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::wstring RouteEpoch::Identity() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_identity;
}

std::wstring RouteEpoch::NetworkIdentity() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_networkIdentity;
}

} // namespace pluginping_shared
