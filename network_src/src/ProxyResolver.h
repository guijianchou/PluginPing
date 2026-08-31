// pluginNetwork - local proxy route resolution
#pragma once
#include "ConfigManager.h"
#include <string>

namespace pluginnetwork {

enum class TcpProxyRoute
{
    Direct,
    Automatic,
    Named,
    Unavailable
};

enum class UdpProxyRoute
{
    Direct,
    Socks5,
    Unavailable
};

struct ProxyRoute
{
    TcpProxyRoute tcp = TcpProxyRoute::Direct;
    UdpProxyRoute udp = UdpProxyRoute::Direct;
    std::wstring tcpProxy;
    std::wstring tcpBypass;
    std::wstring socksHost;
    unsigned short socksPort = 0;
    std::wstring tcpDiagnostic;
    std::wstring udpDiagnostic;
    bool udpSharesHttpIdentity = false;

    std::wstring TcpKey() const;
    std::wstring UdpKey() const;
};

// Resolves one immutable route snapshot. Auto follows the current user's
// enabled static Windows proxy. If no proxy is enabled, Auto is direct; if one
// is enabled, UDP is fail-closed unless a SOCKS5 endpoint can be derived.
ProxyRoute ResolveProxyRoute(const PluginConfig& cfg);

} // namespace pluginnetwork
