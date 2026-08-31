// PluginPing - shared proxy route resolution for HTTP, TCP and UDP.
#pragma once

#include <string>

namespace pluginping_shared {

enum class HttpRoute
{
    Direct,
    Automatic,
    Named,
    Unavailable
};

enum class UdpRoute
{
    Direct,
    Socks5,
    Unavailable
};

struct RouteOptions
{
    std::wstring mode = L"Auto";
    std::wstring proxyServer;
    std::wstring proxyBypass;
    std::wstring socks5Server;
    bool shareProxyIdentityForUdp = false;
};

struct UserProxyState
{
    bool staticProxyEnabled = false;
    bool autoConfigEnabled = false;
    std::wstring server;
    std::wstring bypass;
};

struct RoutePlan
{
    HttpRoute http = HttpRoute::Direct;
    UdpRoute udp = UdpRoute::Direct;
    std::wstring httpProxy;
    std::wstring httpBypass;
    std::wstring socksHost;
    unsigned short socksPort = 0;
    std::wstring httpDiagnostic;
    std::wstring udpDiagnostic;
    bool localProxyDetected = false;
    // True when UDP is direct alongside HTTP, its normalized SOCKS endpoint
    // exactly matches the named HTTP endpoint, or the user explicitly declares
    // that the configured HTTP and SOCKS endpoints select one upstream node.
    bool udpSharesHttpIdentity = false;

};

// Pure route builder used by production and deterministic contract tests.
RoutePlan BuildRoutePlan(const RouteOptions& options, const UserProxyState& userProxy);

// Reads the current user's Windows proxy settings and caches the resulting
// immutable route plan for cacheMs. Both internal engines call this function.
RoutePlan ResolveRoutePlan(const RouteOptions& options, int cacheMs);

} // namespace pluginping_shared
