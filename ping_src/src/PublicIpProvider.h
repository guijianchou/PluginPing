// PluginPing - HTTP IP queries and reachability probes (WinHTTP)
#pragma once
#include <mutex>
#include <string>

namespace pluginping {

struct PluginConfig;

// WinHTTP cancellation is owned by the IP worker. Cancel() may be called by
// the host thread; the request handle is closed under the same mutex used when
// it is registered, so the worker never closes it twice.
class HttpRequestCancellation
{
public:
    void Reset();
    void Cancel();
    bool Register(void* request);
    void Complete(void* request);
    bool IsCancelled() const;

private:
    mutable std::mutex m_mutex;
    void* m_request = nullptr;
    bool m_cancelled = false;
};

enum class EndpointProxyPolicy
{
    DirectNoProxy,
    ConfiguredProxy
};

enum class NetworkStage
{
    None,
    PrimaryIp,
    FallbackIp
};

struct NetworkDiagnostics
{
    NetworkStage stage = NetworkStage::None;
    DWORD status = 0;
    DWORD error = 0;
    int elapsedMs = -1;
    std::wstring target;
    std::wstring proxy;
};

// Query only the domestic primary source: Bilibili zone API data.addr.
bool QueryBilibiliPrimaryIp(std::wstring& ipOut, const PluginConfig& cfg,
                            EndpointProxyPolicy proxyPolicy,
                            NetworkDiagnostics* diag = nullptr,
                            HttpRequestCancellation* cancellation = nullptr);

// Query only the domestic fallback source: pconline ipJson.jsp JSON ip.
bool QueryPconlineIp(std::wstring& ipOut, const PluginConfig& cfg,
                     EndpointProxyPolicy proxyPolicy,
                     NetworkDiagnostics* diag = nullptr,
                     HttpRequestCancellation* cancellation = nullptr);

// Query the remote primary source: cloudflare.com/cdn-cgi/trace.
// ipOut receives the observed egress IP. countryCodeOut receives the optional
// two-letter `loc` value from the same response, so IP and country stay paired.
bool QueryCloudflareTraceIp(std::wstring& ipOut, std::wstring& countryCodeOut,
                            const PluginConfig& cfg,
                            EndpointProxyPolicy proxyPolicy,
                            NetworkDiagnostics* diag = nullptr,
                            HttpRequestCancellation* cancellation = nullptr);

} // namespace pluginping
