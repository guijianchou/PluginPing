// pluginNetwork - overseas TCP path probe (keep-alive HTTPS round trip)
#pragma once
#include "Common.h"
#include "ConfigManager.h"
#include "ProbeCommon.h"
#include "ProxyResolver.h"
#include <mutex>
#include <string>

namespace pluginnetwork {

// Measures the overseas TCP path with one HTTPS request over a pooled
// connection. A bare connect() is deliberately not used: with a router-side
// transparent proxy the SYN is answered on the LAN, so the handshake would
// report ~1 ms for a link that may be entirely down. Requiring a specific
// status code also means an intercepting portal counts as a failure.
//
// Run() is called only from the owning worker thread. Cancel() is the one
// method safe to call from another thread and aborts an in-flight request by
// closing its handle, which is WinHTTP's documented way to interrupt a
// synchronous call.
class TcpProbe
{
public:
    TcpProbe() = default;
    ~TcpProbe();
    TcpProbe(const TcpProbe&) = delete;
    TcpProbe& operator=(const TcpProbe&) = delete;

    // Re-targets the probe. Any pooled connection to the previous host is
    // dropped so the next measurement cannot be attributed to the wrong server.
    void Configure(const std::wstring& url, const PluginConfig& cfg,
                   const ProxyRoute& route);

    ProbeResult Run();

    // Drops pooled handles after a route change or worker exception. It does
    // not clear terminal cancellation, so reset cannot race shutdown and start
    // a new request after StopWorkers has begun.
    void ResetTransport();
    void Cancel();
    void Close();

    const std::wstring& Target() const { return m_url; }

private:
    bool EnsureSession();
    void DropSession();

    std::wstring m_url;
    std::wstring m_host;
    std::wstring m_path;
    unsigned short m_port = 443;
    bool m_secure = true;

    ProxyRoute m_route;
    std::wstring m_routeKey;
    int m_timeoutMs = 1000;
    int m_expectStatus = 204;

    HINTERNET m_session = nullptr;
    HINTERNET m_connect = nullptr;

    // Guards the in-flight request handle so Cancel() can close it without
    // racing the worker's own close.
    std::mutex m_requestMutex;
    HINTERNET m_request = nullptr;
    bool m_cancelled = false;
};

} // namespace pluginnetwork
