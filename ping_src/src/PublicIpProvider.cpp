// PluginPing - IP queries and reachability probes
#include "Common.h"
#include "PublicIpProvider.h"
#include "TraceParser.h"
#include "ConfigManager.h"
#include "../../shared_src/RoutePlan.h"
#include <ws2tcpip.h>
#include <winhttp.h>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <mutex>
#include <string>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace pluginping {

void HttpRequestCancellation::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_request)
        WinHttpCloseHandle(static_cast<HINTERNET>(m_request));
    m_cancelled = false;
    m_request = nullptr;
}

void HttpRequestCancellation::Cancel()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cancelled = true;
    if (m_request)
    {
        WinHttpCloseHandle(static_cast<HINTERNET>(m_request));
        m_request = nullptr;
    }
}

bool HttpRequestCancellation::Register(void* request)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cancelled || !request || m_request)
        return false;
    m_request = request;
    return true;
}

void HttpRequestCancellation::Complete(void* request)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_request == request)
    {
        WinHttpCloseHandle(static_cast<HINTERNET>(request));
        m_request = nullptr;
    }
}

bool HttpRequestCancellation::IsCancelled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cancelled;
}

namespace {

constexpr size_t kMaxResponseBodyBytes = 4096;

class WinHttpHandleScope
{
public:
    explicit WinHttpHandleScope(HINTERNET handle) : m_handle(handle) {}
    ~WinHttpHandleScope()
    {
        if (m_handle)
            WinHttpCloseHandle(m_handle);
    }
    WinHttpHandleScope(const WinHttpHandleScope&) = delete;
    WinHttpHandleScope& operator=(const WinHttpHandleScope&) = delete;

private:
    HINTERNET m_handle = nullptr;
};

class RequestHandleScope
{
public:
    RequestHandleScope(HINTERNET handle, HttpRequestCancellation* cancellation)
        : m_handle(handle), m_cancellation(cancellation),
          m_registered(!cancellation || cancellation->Register(handle))
    {
    }

    ~RequestHandleScope()
    {
        if (!m_handle)
            return;
        if (m_cancellation && m_registered)
            m_cancellation->Complete(m_handle);
        else
            WinHttpCloseHandle(m_handle);
    }

    bool Ready() const { return m_registered; }
    RequestHandleScope(const RequestHandleScope&) = delete;
    RequestHandleScope& operator=(const RequestHandleScope&) = delete;

private:
    HINTERNET m_handle = nullptr;
    HttpRequestCancellation* m_cancellation = nullptr;
    bool m_registered = false;
};

struct HttpResponse
{
    DWORD status = 0;
    DWORD error = 0;
    std::string body;
    int elapsedMs = -1;
    std::wstring proxySummary;
};

struct ProxySettings
{
    DWORD accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
    std::wstring proxy;
    bool unavailable = false;
    std::wstring diagnostic;
};

std::wstring ProxySummary(const PluginConfig& cfg, EndpointProxyPolicy policy, const ProxySettings& proxy)
{
    if (policy == EndpointProxyPolicy::DirectNoProxy)
        return L"DirectNoProxy direct";

    std::wstring out = cfg.proxyMode.empty() ? L"Auto" : cfg.proxyMode;
    if (proxy.unavailable)
        return out + L" unavailable" + (proxy.diagnostic.empty() ? L"" : L" (" + proxy.diagnostic + L")");
    if (proxy.accessType == WINHTTP_ACCESS_TYPE_NO_PROXY)
        return out + L" direct";
    if (proxy.accessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY)
    {
        out += L" ";
        out += proxy.proxy.empty() ? L"named-proxy" : proxy.proxy;
        return out;
    }
    if (proxy.accessType == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY)
        return out + L" automatic-proxy";
    return out + L" default-proxy";
}

ProxySettings ResolveProxySettings(const PluginConfig& cfg, EndpointProxyPolicy policy)
{
    ProxySettings ps;
    if (policy == EndpointProxyPolicy::DirectNoProxy)
    {
        ps.accessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
        return ps;
    }

    pluginping_shared::RouteOptions options;
    options.mode = cfg.proxyMode;
    options.proxyServer = cfg.proxyServer;
    options.proxyBypass = cfg.proxyBypass;
    options.socks5Server = cfg.socks5Server;
    options.shareProxyIdentityForUdp = cfg.shareProxyIdentityForUdp;
    const pluginping_shared::RoutePlan plan =
        pluginping_shared::ResolveRoutePlan(options, cfg.proxyCacheMs);
    ps.proxy = plan.httpProxy;
    ps.diagnostic = plan.httpDiagnostic;
    switch (plan.http)
    {
    case pluginping_shared::HttpRoute::Automatic:
        ps.accessType = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
        break;
    case pluginping_shared::HttpRoute::Named:
        ps.accessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        break;
    case pluginping_shared::HttpRoute::Unavailable:
        ps.accessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
        ps.unavailable = true;
        break;
    default:
        ps.accessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
        break;
    }
    return ps;
}

bool HttpGet(const wchar_t* host, const wchar_t* path, HttpResponse& response,
             const PluginConfig& cfg, EndpointProxyPolicy proxyPolicy,
             HttpRequestCancellation* cancellation)
{
    response = HttpResponse{};

    // ResolveRoutePlan already owns the configured route cache. A second cache
    // here would stack two expiry windows and delay proxy-setting changes.
    const ProxySettings proxy = ResolveProxySettings(cfg, proxyPolicy);
    response.proxySummary = ProxySummary(cfg, proxyPolicy, proxy);
    if (proxy.unavailable)
    {
        response.error = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (cancellation && cancellation->IsCancelled())
    {
        response.error = ERROR_WINHTTP_OPERATION_CANCELLED;
        return false;
    }
    // Public-IP requests are infrequent and PROXY requests disable keep-alive.
    // An owned session keeps its lifetime exact and cannot be evicted while
    // the other IP worker is still using it.
    HINTERNET hSession = WinHttpOpen(kPluginPingUserAgent, proxy.accessType,
                                    proxy.proxy.empty() ? WINHTTP_NO_PROXY_NAME
                                                        : proxy.proxy.c_str(),
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    WinHttpHandleScope sessionScope(hSession);
    if (!hSession)
    {
        response.error = GetLastError();
        return false;
    }
    WinHttpSetTimeouts(hSession, cfg.timeoutMs, cfg.timeoutMs,
                       cfg.timeoutMs, cfg.timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, host,
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    WinHttpHandleScope connectScope(hConnect);
    if (!hConnect)
    {
        response.error = GetLastError();
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest)
    {
        response.error = GetLastError();
        return false;
    }
    RequestHandleScope requestScope(hRequest, cancellation);
    if (!requestScope.Ready())
    {
        response.error = ERROR_WINHTTP_OPERATION_CANCELLED;
        return false;
    }

    if (proxyPolicy == EndpointProxyPolicy::ConfiguredProxy)
    {
        // The local proxy address usually stays constant while its selected
        // upstream node changes. Do not inherit the previous trace's tunnel.
        DWORD disabledFeatures = WINHTTP_DISABLE_KEEP_ALIVE;
        if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE,
                              &disabledFeatures, sizeof(disabledFeatures)))
        {
            response.error = GetLastError();
            return false;
        }
    }

    const ULONGLONG started = GetTickCount64();
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr))
    {
        response.error = GetLastError();
        return false;
    }
    response.elapsedMs = static_cast<int>(GetTickCount64() - started);

    DWORD statusSize = sizeof(response.status);
    if (!WinHttpQueryHeaders(hRequest,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &response.status,
                             &statusSize, WINHTTP_NO_HEADER_INDEX))
    {
        response.error = GetLastError();
        return false;
    }

    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available))
        {
            response.error = GetLastError();
            return false;
        }
        if (available == 0)
            return true;

        const size_t remaining = kMaxResponseBodyBytes - response.body.size();
        if (remaining == 0)
        {
            response.error = ERROR_MORE_DATA;
            return false;
        }

        const DWORD toRead = static_cast<DWORD>((std::min)(
            static_cast<size_t>(available), remaining));
        std::string chunk(toRead, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), toRead, &read))
        {
            response.error = GetLastError();
            return false;
        }
        if (read == 0)
        {
            response.error = ERROR_READ_FAULT;
            return false;
        }
        response.body.append(chunk.data(), read);
    }
}

std::wstring ExtractIpToken(const std::string& body)
{
    size_t start = 0;
    while (start < body.size() && (body[start] == ' ' || body[start] == '\r' ||
                                   body[start] == '\n' || body[start] == '\t' ||
                                   body[start] == '"'))
        ++start;

    size_t end = start;
    while (end < body.size() && body[end] != ' ' && body[end] != '\r' &&
           body[end] != '\n' && body[end] != '\t' && body[end] != '"' &&
           body[end] != ',' && body[end] != '}')
        ++end;

    std::wstring w;
    for (size_t i = start; i < end; ++i)
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(body[i])));
    return w;
}

bool IsValidIPv4(const std::wstring& ip)
{
    IN_ADDR parsed{};
    return InetPtonW(AF_INET, ip.c_str(), &parsed) == 1;
}

bool ExtractJsonStringField(const std::string& body, const char* field, std::string& value)
{
    const std::string needle = std::string("\"") + field + "\"";
    size_t pos = body.find(needle);
    while (pos != std::string::npos)
    {
        pos = body.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return false;
        ++pos;
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t' ||
                                     body[pos] == '\r' || body[pos] == '\n'))
            ++pos;
        if (pos >= body.size() || body[pos] != '"')
        {
            pos = body.find(needle, pos);
            continue;
        }
        ++pos;

        value.clear();
        bool escaping = false;
        for (; pos < body.size(); ++pos)
        {
            const char ch = body[pos];
            if (escaping)
            {
                value.push_back(ch);
                escaping = false;
            }
            else if (ch == '\\')
            {
                escaping = true;
            }
            else if (ch == '"')
            {
                return true;
            }
            else
            {
                value.push_back(ch);
            }
        }
        return false;
    }
    return false;
}

void SetDiag(NetworkDiagnostics* diag, NetworkStage stage, const wchar_t* target,
             const HttpResponse& response)
{
    if (!diag)
        return;
    diag->stage = stage;
    diag->target = target ? target : L"";
    diag->status = response.status;
    diag->error = response.error;
    diag->elapsedMs = response.elapsedMs;
    diag->proxy = response.proxySummary;
}

} // namespace

bool QueryBilibiliPrimaryIp(std::wstring& ipOut, const PluginConfig& cfg,
                            EndpointProxyPolicy proxyPolicy,
                            NetworkDiagnostics* diag,
                            HttpRequestCancellation* cancellation)
{
    HttpResponse response;
    const bool fetched = HttpGet(L"api.bilibili.com", L"/x/web-interface/zone",
                                 response, cfg, proxyPolicy, cancellation);
    SetDiag(diag, NetworkStage::PrimaryIp, L"api.bilibili.com", response);
    if (fetched && response.status == 200 && !response.body.empty())
    {
        std::string addr;
        if (ExtractJsonStringField(response.body, "addr", addr))
        {
            const std::wstring ip = ExtractIpToken(addr);
            if (IsValidIPv4(ip))
            {
                ipOut = ip;
                return true;
            }
        }
    }
    return false;
}

bool QueryPconlineIp(std::wstring& ipOut, const PluginConfig& cfg,
                     EndpointProxyPolicy proxyPolicy,
                     NetworkDiagnostics* diag,
                     HttpRequestCancellation* cancellation)
{
    HttpResponse response;
    const bool fetched = HttpGet(L"whois.pconline.com.cn", L"/ipJson.jsp?json=true",
                                 response, cfg, proxyPolicy, cancellation);
    SetDiag(diag, NetworkStage::FallbackIp, L"whois.pconline.com.cn", response);
    if (!fetched || response.status != 200 || response.body.empty())
        return false;

    std::string fallbackIp;
    if (!ExtractJsonStringField(response.body, "ip", fallbackIp))
        return false;

    const std::wstring ip = ExtractIpToken(fallbackIp);
    if (!IsValidIPv4(ip))
        return false;

    ipOut = ip;
    return true;
}

bool QueryCloudflareTraceIp(std::wstring& ipOut, std::wstring& countryCodeOut,
                            const PluginConfig& cfg,
                            EndpointProxyPolicy proxyPolicy,
                            NetworkDiagnostics* diag,
                            HttpRequestCancellation* cancellation)
{
    ipOut.clear();
    countryCodeOut.clear();

    HttpResponse response;
    const bool fetched = HttpGet(L"www.cloudflare.com", L"/cdn-cgi/trace",
                                 response, cfg, proxyPolicy, cancellation);
    SetDiag(diag, NetworkStage::PrimaryIp,
            L"www.cloudflare.com/cdn-cgi/trace", response);
    if (!fetched || response.status != 200 || response.body.empty())
        return false;

    CloudflareTraceFields fields;
    if (!ParseCloudflareTraceBody(response.body, fields))
        return false;
    ipOut = fields.ip;
    countryCodeOut = fields.loc;
    return true;
}

} // namespace pluginping
