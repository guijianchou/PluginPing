// PluginPing - adapter from the shared route plan to network probe routes.
#include "Common.h"
#include "ProxyResolver.h"
#include "../../shared_src/RoutePlan.h"

namespace pluginnetwork {

std::wstring ProxyRoute::TcpKey() const
{
    return std::to_wstring(static_cast<int>(tcp)) + L"|" + tcpProxy + L"|" + tcpBypass;
}

std::wstring ProxyRoute::UdpKey() const
{
    return std::to_wstring(static_cast<int>(udp)) + L"|" + socksHost + L"|" +
           std::to_wstring(socksPort) + L"|" +
           (udpSharesHttpIdentity ? L"shared" : L"independent");
}

ProxyRoute ResolveProxyRoute(const PluginConfig& cfg)
{
    pluginping_shared::RouteOptions options;
    options.mode = cfg.proxyMode;
    options.proxyServer = cfg.proxyServer;
    options.proxyBypass = cfg.proxyBypass;
    options.socks5Server = cfg.socks5Server;
    options.shareProxyIdentityForUdp = cfg.shareProxyIdentityForUdp;
    const pluginping_shared::RoutePlan plan =
        pluginping_shared::ResolveRoutePlan(options, cfg.proxyCacheMs);

    ProxyRoute route;
    switch (plan.http)
    {
    case pluginping_shared::HttpRoute::Automatic:
        route.tcp = TcpProxyRoute::Automatic;
        break;
    case pluginping_shared::HttpRoute::Named:
        route.tcp = TcpProxyRoute::Named;
        break;
    case pluginping_shared::HttpRoute::Unavailable:
        route.tcp = TcpProxyRoute::Unavailable;
        break;
    default:
        route.tcp = TcpProxyRoute::Direct;
        break;
    }

    switch (plan.udp)
    {
    case pluginping_shared::UdpRoute::Socks5:
        route.udp = UdpProxyRoute::Socks5;
        break;
    case pluginping_shared::UdpRoute::Unavailable:
        route.udp = UdpProxyRoute::Unavailable;
        break;
    default:
        route.udp = UdpProxyRoute::Direct;
        break;
    }

    route.tcpProxy = plan.httpProxy;
    route.tcpBypass = plan.httpBypass;
    route.socksHost = plan.socksHost;
    route.socksPort = plan.socksPort;
    route.tcpDiagnostic = plan.httpDiagnostic;
    route.udpDiagnostic = plan.udpDiagnostic;
    route.udpSharesHttpIdentity = plan.udpSharesHttpIdentity;
    return route;
}

} // namespace pluginnetwork
