// PluginPing - UDP transport with Echo and RFC 5389 codecs.
#pragma once
#include "Common.h"
#include "ConfigManager.h"
#include "ProbeCommon.h"
#include "ProxyResolver.h"
#include <mutex>
#include <string>
#include <vector>

namespace pluginnetwork {

enum class UdpProbeProtocol
{
    Echo,
    Stun
};

// The transport owns direct UDP or SOCKS5 UDP ASSOCIATE state. The selected
// protocol codec builds and validates either an exact echo payload or an RFC
// 5389 binding transaction.
//
// The UDP socket is kept between rounds on purpose: reusing it holds the NAT
// mapping steady, so a change in the reported egress address is a real route
// change rather than an artefact of a new source port.
//
// Run() is called only from the owning worker thread. Cancel() may be called
// from another thread and shuts down the transport to break an in-flight wait;
// Close() releases the handles after the worker has exited.
class UdpProbe
{
public:
    UdpProbe() = default;
    ~UdpProbe();
    UdpProbe(const UdpProbe&) = delete;
    UdpProbe& operator=(const UdpProbe&) = delete;

    // `server` accepts "host", "host:port" and "scheme://host:port".
    void Configure(const std::wstring& server, unsigned short defaultPort,
                   const PluginConfig& cfg, const ProxyRoute& route,
                   UdpProbeProtocol protocol);

    // Performs one Echo or STUN transaction and reports its round-trip time.
    ProbeResult Run();

    // Releases the current UDP/SOCKS transport without permanently cancelling
    // the probe. The next Run() rebuilds it from the existing configuration.
    void ResetTransport();
    void Cancel();
    void Close();

    const std::wstring& Target() const { return m_target; }

private:
    bool EnsureResolved();
    bool EnsureSocket();
    bool EnsureSocksTunnel();
    void DropSocket();
    void DrainStaleDatagrams();
    bool BuildOutboundDatagram(const unsigned char* payload, size_t payloadSize,
                               std::vector<unsigned char>& datagram) const;
    bool LocateInboundPayload(const unsigned char* datagram, size_t datagramSize,
                              const unsigned char*& payload, size_t& payloadSize) const;

    std::wstring m_target;      // display form, "host:port"
    std::wstring m_host;
    unsigned short m_port = 19302;
    int m_timeoutMs = 1000;
    int m_dnsRefreshMs = 300000;
    int m_recycleAfterFailures = 3;
    UdpProbeProtocol m_protocol = UdpProbeProtocol::Stun;
    ProxyRoute m_route;
    std::wstring m_routeKey;
    std::wstring m_lastSocketDiagnostic;

    sockaddr_in m_address{};
    bool m_resolved = false;
    unsigned long long m_lastResolveTick = 0;
    int m_failStreak = 0;

    // Guards the socket so Cancel() can close it without racing Run()'s own use.
    std::mutex m_socketMutex;
    SOCKET m_socket = INVALID_SOCKET;
    SOCKET m_socksControl = INVALID_SOCKET;
    bool m_cancelled = false;
};

} // namespace pluginnetwork
