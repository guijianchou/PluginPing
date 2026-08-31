// pluginNetwork - overseas TCP path probe implementation
#include "Common.h"
#include "TcpProbe.h"
#include "ProbeCommon.h"
#include <algorithm>
#include <vector>

namespace pluginnetwork {

namespace {

// generate_204 returns no body at all; the cap only exists so a misconfigured
// target that streams megabytes cannot stall the sampling cadence.
constexpr DWORD kMaxDrainBytes = 64 * 1024;

bool CrackUrl(const std::wstring& url, std::wstring& host, unsigned short& port,
              std::wstring& path, bool& secure)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t hostBuffer[256] = {};
    wchar_t pathBuffer[1024] = {};
    wchar_t extraBuffer[1024] = {};
    parts.lpszHostName = hostBuffer;
    parts.dwHostNameLength = ARRAYSIZE(hostBuffer);
    parts.lpszUrlPath = pathBuffer;
    parts.dwUrlPathLength = ARRAYSIZE(pathBuffer);
    parts.lpszExtraInfo = extraBuffer;
    parts.dwExtraInfoLength = ARRAYSIZE(extraBuffer);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return false;

    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    if (host.empty())
        return false;
    port = parts.nPort;
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty())
        path = L"/";
    secure = (parts.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

const wchar_t* WinHttpErrorName(DWORD error)
{
    switch (error)
    {
    case ERROR_WINHTTP_TIMEOUT:              return L"timeout";
    case ERROR_WINHTTP_CANNOT_CONNECT:       return L"cannot-connect";
    case ERROR_WINHTTP_CONNECTION_ERROR:     return L"connection-error";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:    return L"dns-fail";
    case ERROR_WINHTTP_SECURE_FAILURE:       return L"tls-fail";
    case ERROR_WINHTTP_OPERATION_CANCELLED:  return L"cancelled";
    case ERROR_WINHTTP_INVALID_URL:          return L"invalid-url";
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:  return L"bad-scheme";
    case ERROR_WINHTTP_INTERNAL_ERROR:       return L"internal";
    default:                                 return L"error";
    }
}

std::wstring FailDiagnostic(const wchar_t* stage, DWORD error)
{
    std::wstring text = L"tcp fail ";
    text += stage;
    text += L" ";
    text += WinHttpErrorName(error);
    text += L" (";
    text += std::to_wstring(error);
    text += L")";
    return text;
}

} // namespace

TcpProbe::~TcpProbe()
{
    Close();
}

void TcpProbe::Configure(const std::wstring& url, const PluginConfig& cfg,
                         const ProxyRoute& route)
{
    const std::wstring routeKey = route.TcpKey();
    const bool routingChanged = url != m_url ||
                                routeKey != m_routeKey ||
                                cfg.timeoutMs != m_timeoutMs;

    m_url = url;
    m_route = route;
    m_routeKey = routeKey;
    m_timeoutMs = cfg.timeoutMs;
    m_expectStatus = cfg.tcpExpectStatus;

    if (routingChanged)
    {
        // A pooled connection to the previous host or through the previous
        // proxy would produce a measurement for a path nobody asked about.
        DropSession();
        if (!CrackUrl(m_url, m_host, m_port, m_path, m_secure))
        {
            // Accept a bare host by assuming the documented default endpoint.
            const std::wstring assumed = L"https://" + m_url + L"/generate_204";
            if (!CrackUrl(assumed, m_host, m_port, m_path, m_secure))
                m_host.clear();
            else
                m_url = assumed;
        }
    }
}

void TcpProbe::DropSession()
{
    if (m_connect)
    {
        WinHttpCloseHandle(m_connect);
        m_connect = nullptr;
    }
    if (m_session)
    {
        WinHttpCloseHandle(m_session);
        m_session = nullptr;
    }
}

void TcpProbe::Close()
{
    Cancel();
    DropSession();
}

void TcpProbe::ResetTransport()
{
    DropSession();
}

void TcpProbe::Cancel()
{
    std::lock_guard<std::mutex> lk(m_requestMutex);
    m_cancelled = true;
    if (m_request)
    {
        // Closing the handle is WinHTTP's documented way to make a pending
        // synchronous call return immediately. Run() sees m_request cleared and
        // therefore will not close it a second time.
        WinHttpCloseHandle(m_request);
        m_request = nullptr;
    }
}

bool TcpProbe::EnsureSession()
{
    if (m_session && m_connect)
        return true;
    if (m_host.empty())
        return false;

    DropSession();

    if (m_route.tcp == TcpProxyRoute::Unavailable)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    DWORD accessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
    const wchar_t* proxyName = WINHTTP_NO_PROXY_NAME;
    if (m_route.tcp == TcpProxyRoute::Automatic)
    {
        accessType = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    }
    else if (m_route.tcp == TcpProxyRoute::Named)
    {
        accessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        proxyName = m_route.tcpProxy.c_str();
    }

    // The TCP health probe is authoritative for the configured route. A
    // user-supplied bypass list must not silently turn Google into a direct
    // request, otherwise a broken proxy can report healthy TCP.
    m_session = WinHttpOpen(kPluginNetworkUserAgent, accessType, proxyName,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!m_session && accessType == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY)
    {
        // AUTOMATIC_PROXY needs Windows 8.1; fall back rather than losing the
        // channel entirely on an older build.
        m_session = WinHttpOpen(kPluginNetworkUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!m_session)
        return false;

    const int timeout = (std::max)(m_timeoutMs, 100);
    WinHttpSetTimeouts(m_session, timeout, timeout, timeout, timeout);

    m_connect = WinHttpConnect(m_session, m_host.c_str(), m_port, 0);
    if (!m_connect)
    {
        DropSession();
        return false;
    }
    return true;
}

ProbeResult TcpProbe::Run()
{
    ProbeResult result;

    {
        std::lock_guard<std::mutex> lk(m_requestMutex);
        if (m_cancelled)
        {
            result.cancelled = true;
            result.diagnostic = L"tcp cancelled";
            return result;
        }
    }

    if (m_host.empty())
    {
        result.diagnostic = L"tcp fail invalid target";
        return result;
    }
    if (m_route.tcp == TcpProxyRoute::Unavailable)
    {
        result.diagnostic = L"tcp fail " +
            (m_route.tcpDiagnostic.empty() ? std::wstring(L"proxy unavailable") :
                                             m_route.tcpDiagnostic);
        return result;
    }
    if (!EnsureSession())
    {
        result.diagnostic = FailDiagnostic(L"session", GetLastError());
        return result;
    }

    const DWORD requestFlags = (m_secure ? WINHTTP_FLAG_SECURE : 0) | WINHTTP_FLAG_REFRESH;
    HINTERNET request = WinHttpOpenRequest(m_connect, L"GET", m_path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           requestFlags);
    if (!request)
    {
        const DWORD error = GetLastError();
        DropSession();
        result.diagnostic = FailDiagnostic(L"open", error);
        return result;
    }

    {
        std::lock_guard<std::mutex> lk(m_requestMutex);
        if (m_cancelled)
        {
            WinHttpCloseHandle(request);
            result.cancelled = true;
            result.diagnostic = L"tcp cancelled";
            return result;
        }
        m_request = request;
    }

    // Reclaims the handle on every exit path, and stays silent if Cancel()
    // already closed it.
    struct RequestScope
    {
        TcpProbe* probe;
        ~RequestScope()
        {
            HINTERNET owned = nullptr;
            {
                std::lock_guard<std::mutex> lk(probe->m_requestMutex);
                owned = probe->m_request;
                probe->m_request = nullptr;
            }
            if (owned)
                WinHttpCloseHandle(owned);
        }
    } scope{ this };

    static const wchar_t kHeaders[] = L"Cache-Control: no-cache\r\nPragma: no-cache\r\n";

    Stopwatch watch;
    watch.Start();

    if (!WinHttpSendRequest(request, kHeaders, static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        const DWORD error = GetLastError();
        DropSession();
        if (error == ERROR_WINHTTP_OPERATION_CANCELLED)
        {
            result.cancelled = true;
            result.diagnostic = L"tcp cancelled";
            return result;
        }
        result.diagnostic = FailDiagnostic(L"send", error);
        return result;
    }

    if (!WinHttpReceiveResponse(request, nullptr))
    {
        const DWORD error = GetLastError();
        DropSession();
        if (error == ERROR_WINHTTP_OPERATION_CANCELLED)
        {
            result.cancelled = true;
            result.diagnostic = L"tcp cancelled";
            return result;
        }
        result.diagnostic = FailDiagnostic(L"receive", error);
        return result;
    }

    // Stop here: the headers are back, which is the full application round trip.
    // Draining the (empty) body afterwards must not inflate the measurement.
    const int elapsedMs = watch.ElapsedMs();

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX))
    {
        const DWORD error = GetLastError();
        DropSession();
        result.diagnostic = FailDiagnostic(L"status", error);
        return result;
    }

    // The body has to be consumed for the connection to go back into the pool
    // and give the next probe a clean single round trip.
    DWORD drained = 0;
    bool drainComplete = true;
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            drainComplete = false;
            break;
        }
        if (available == 0)
            break;

        char buffer[4096];
        const DWORD want = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, want, &read) || read == 0)
        {
            drainComplete = false;
            break;
        }
        drained += read;
        if (drained > kMaxDrainBytes)
        {
            drainComplete = false;
            break;
        }
    }
    if (!drainComplete)
    {
        // The pooled connection is no longer in a known state.
        DropSession();
    }

    const bool statusOk = (m_expectStatus > 0)
                              ? (status == static_cast<DWORD>(m_expectStatus))
                              : (status >= 200 && status < 300);
    if (!statusOk)
    {
        // A portal or interception box answering 200/302 with HTML lands here,
        // which is the intent: the tunnel is not delivering the real endpoint.
        result.diagnostic = L"tcp fail status " + std::to_wstring(status);
        if (m_expectStatus > 0)
            result.diagnostic += L" (expected " + std::to_wstring(m_expectStatus) + L")";
        DropSession();
        return result;
    }

    result.ok = true;
    result.rttMs = elapsedMs;
    result.diagnostic = L"tcp ok " + std::to_wstring(status) + L" " +
                        std::to_wstring(elapsedMs) + L" ms";
    return result;
}

} // namespace pluginnetwork
