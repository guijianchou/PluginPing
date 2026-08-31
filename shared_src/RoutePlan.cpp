// PluginPing - shared proxy route resolution implementation.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "RoutePlan.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <vector>

namespace pluginping_shared {

namespace {

struct ProxySpec
{
    std::wstring plain;
    std::wstring http;
    std::wstring https;
    std::wstring socks;
};

std::wstring Trim(const std::wstring& value)
{
    size_t first = 0;
    while (first < value.size() && iswspace(value[first]))
        ++first;
    size_t last = value.size();
    while (last > first && iswspace(value[last - 1]))
        --last;
    return value.substr(first, last - first);
}

std::wstring Lower(std::wstring value)
{
    for (wchar_t& ch : value)
        ch = static_cast<wchar_t>(towlower(ch));
    return value;
}

ProxySpec ParseProxySpec(const std::wstring& raw)
{
    ProxySpec spec;
    size_t start = 0;
    while (start <= raw.size())
    {
        const size_t separator = raw.find(L';', start);
        const std::wstring token = Trim(raw.substr(
            start, separator == std::wstring::npos ? std::wstring::npos : separator - start));
        if (!token.empty())
        {
            const size_t equals = token.find(L'=');
            if (equals == std::wstring::npos)
            {
                if (spec.plain.empty())
                    spec.plain = token;
            }
            else
            {
                const std::wstring key = Lower(Trim(token.substr(0, equals)));
                const std::wstring value = Trim(token.substr(equals + 1));
                if (key == L"http" && spec.http.empty())
                    spec.http = value;
                else if (key == L"https" && spec.https.empty())
                    spec.https = value;
                else if ((key == L"socks" || key == L"socks5") && spec.socks.empty())
                    spec.socks = value;
            }
        }
        if (separator == std::wstring::npos)
            break;
        start = separator + 1;
    }
    return spec;
}

bool ParsePort(const std::wstring& text, unsigned short& port)
{
    if (text.empty())
        return false;
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(text.c_str(), &end, 10);
    if (!end || *end != L'\0' || parsed == 0 || parsed > 65535)
        return false;
    port = static_cast<unsigned short>(parsed);
    return true;
}

bool ParseEndpoint(const std::wstring& raw, unsigned short defaultPort,
                   std::wstring& host, unsigned short& port)
{
    std::wstring value = Trim(raw);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    if (value.empty() || value.find(L'@') != std::wstring::npos)
        return false;

    unsigned short inferredPort = defaultPort;
    const std::wstring lowered = Lower(value);
    const wchar_t* schemes[] = { L"http://", L"https://", L"socks://", L"socks5://" };
    for (const wchar_t* scheme : schemes)
    {
        const size_t length = wcslen(scheme);
        if (lowered.rfind(scheme, 0) == 0)
        {
            value.erase(0, length);
            if (inferredPort == 0)
                inferredPort = wcscmp(scheme, L"http://") == 0 ? 80 :
                               wcscmp(scheme, L"https://") == 0 ? 443 : 1080;
            break;
        }
    }

    if (value.empty() || value.front() == L'[' || value.find(L'/') != std::wstring::npos)
        return false; // Local proxy transports are IPv4/hostname only.

    const size_t colon = value.rfind(L':');
    if (colon == std::wstring::npos)
    {
        if (inferredPort == 0)
            return false;
        host = value;
        port = inferredPort;
    }
    else
    {
        host = value.substr(0, colon);
        if (!ParsePort(value.substr(colon + 1), port))
            return false;
    }
    return !host.empty() && host.find(L':') == std::wstring::npos;
}

std::wstring HttpCandidate(const ProxySpec& spec)
{
    if (!spec.https.empty())
        return spec.https;
    if (!spec.http.empty())
        return spec.http;
    return spec.plain;
}

std::wstring UdpCandidate(const ProxySpec& spec)
{
    if (!spec.socks.empty())
        return spec.socks;
    if (!spec.plain.empty())
        return spec.plain;
    if (!spec.https.empty())
        return spec.https;
    return spec.http;
}

void ConfigureHttp(RoutePlan& plan, const ProxySpec& spec, const std::wstring& bypass)
{
    std::wstring host;
    unsigned short port = 0;
    if (!ParseEndpoint(HttpCandidate(spec), 0, host, port))
    {
        plan.http = HttpRoute::Unavailable;
        plan.httpDiagnostic = L"local proxy has no usable HTTP/HTTPS endpoint";
        return;
    }
    plan.http = HttpRoute::Named;
    plan.httpProxy = host + L":" + std::to_wstring(port);
    plan.httpBypass = bypass;
}

void ConfigureUdp(RoutePlan& plan, const std::wstring& configured, const ProxySpec& detected)
{
    const std::wstring candidate = configured.empty() ? UdpCandidate(detected) : configured;
    if (!ParseEndpoint(candidate, 0, plan.socksHost, plan.socksPort))
    {
        plan.udp = UdpRoute::Unavailable;
        plan.udpDiagnostic = L"local proxy has no usable SOCKS5 endpoint";
        return;
    }
    plan.udp = UdpRoute::Socks5;
}

bool SharesNamedProxyEndpoint(const RoutePlan& plan)
{
    if (plan.http != HttpRoute::Named || plan.udp != UdpRoute::Socks5)
        return false;
    const std::wstring udpEndpoint =
        plan.socksHost + L":" + std::to_wstring(plan.socksPort);
    return _wcsicmp(plan.httpProxy.c_str(), udpEndpoint.c_str()) == 0;
}

bool SharesDeclaredProxyIdentity(const RoutePlan& plan, const RouteOptions& options)
{
    return options.shareProxyIdentityForUdp &&
           plan.http != HttpRoute::Unavailable &&
           plan.http != HttpRoute::Direct &&
           plan.udp == UdpRoute::Socks5;
}

void ReadRegistryString(HKEY key, const wchar_t* name, std::wstring& value)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
        return;

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(buffer.data()), &bytes) == ERROR_SUCCESS)
        value.assign(buffer.data());
}

UserProxyState ReadUserProxyState()
{
    UserProxyState state;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return state;

    DWORD enabled = 0;
    DWORD type = 0;
    DWORD bytes = sizeof(enabled);
    state.staticProxyEnabled =
        RegQueryValueExW(key, L"ProxyEnable", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&enabled), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && enabled != 0;
    if (state.staticProxyEnabled)
    {
        ReadRegistryString(key, L"ProxyServer", state.server);
        ReadRegistryString(key, L"ProxyOverride", state.bypass);
        state.staticProxyEnabled = !Trim(state.server).empty();
    }

    std::wstring autoConfigUrl;
    ReadRegistryString(key, L"AutoConfigURL", autoConfigUrl);
    state.autoConfigEnabled = !Trim(autoConfigUrl).empty();
    RegCloseKey(key);
    return state;
}

bool SameOptions(const RouteOptions& a, const RouteOptions& b)
{
    return _wcsicmp(a.mode.c_str(), b.mode.c_str()) == 0 &&
           a.proxyServer == b.proxyServer &&
           a.proxyBypass == b.proxyBypass &&
           a.socks5Server == b.socks5Server &&
           a.shareProxyIdentityForUdp == b.shareProxyIdentityForUdp;
}

} // namespace

RoutePlan BuildRoutePlan(const RouteOptions& options, const UserProxyState& userProxy)
{
    RoutePlan plan;
    const std::wstring mode = options.mode.empty() ? L"Auto" : options.mode;
    if (_wcsicmp(mode.c_str(), L"Direct") == 0)
    {
        plan.udpSharesHttpIdentity = true;
        return plan;
    }

    if (_wcsicmp(mode.c_str(), L"Manual") == 0)
    {
        if (Trim(options.proxyServer).empty())
        {
            plan.http = HttpRoute::Unavailable;
            plan.udp = UdpRoute::Unavailable;
            plan.httpDiagnostic = L"Manual proxy server is empty";
            plan.udpDiagnostic = plan.httpDiagnostic;
            return plan;
        }
        plan.localProxyDetected = true;
        const ProxySpec spec = ParseProxySpec(options.proxyServer);
        ConfigureHttp(plan, spec, options.proxyBypass);
        ConfigureUdp(plan, options.socks5Server, spec);
        plan.udpSharesHttpIdentity = SharesNamedProxyEndpoint(plan) ||
                                     SharesDeclaredProxyIdentity(plan, options);
        return plan;
    }

    if (_wcsicmp(mode.c_str(), L"System") == 0)
    {
        plan.http = HttpRoute::Automatic;
        plan.localProxyDetected = userProxy.staticProxyEnabled || userProxy.autoConfigEnabled;
        if (!options.socks5Server.empty())
            ConfigureUdp(plan, options.socks5Server, ProxySpec{});
        else if (userProxy.staticProxyEnabled)
        {
            ConfigureUdp(plan, std::wstring(), ParseProxySpec(userProxy.server));
        }
        else if (userProxy.autoConfigEnabled)
        {
            plan.udp = UdpRoute::Unavailable;
            plan.udpDiagnostic = L"PAC/WPAD cannot provide a SOCKS5 UDP route";
        }
        plan.udpSharesHttpIdentity = SharesDeclaredProxyIdentity(plan, options);
        return plan;
    }

    // Auto deliberately follows only an enabled static user proxy. With no
    // local proxy it is Direct, which preserves router-side transparent routing.
    if (!userProxy.staticProxyEnabled)
    {
        plan.udpSharesHttpIdentity = true;
        return plan;
    }

    plan.localProxyDetected = true;
    const ProxySpec spec = ParseProxySpec(userProxy.server);
    ConfigureHttp(plan, spec,
                  userProxy.bypass.empty() ? options.proxyBypass : userProxy.bypass);
    ConfigureUdp(plan, options.socks5Server, spec);
    plan.udpSharesHttpIdentity = SharesNamedProxyEndpoint(plan) ||
                                 SharesDeclaredProxyIdentity(plan, options);
    return plan;
}

RoutePlan ResolveRoutePlan(const RouteOptions& options, int cacheMs)
{
    struct Cache
    {
        std::mutex mutex;
        RouteOptions options;
        RoutePlan plan;
        int cacheMs = 0;
        unsigned long long refreshedAt = 0;
    };
    static Cache* cache = new Cache();

    std::lock_guard<std::mutex> lock(cache->mutex);
    const unsigned long long now = GetTickCount64();
    const int boundedCacheMs = std::clamp(cacheMs, 1000, 60000);
    if (cache->refreshedAt == 0 || !SameOptions(cache->options, options) ||
        cache->cacheMs != boundedCacheMs ||
        now - cache->refreshedAt >= static_cast<unsigned long long>(boundedCacheMs))
    {
        cache->plan = BuildRoutePlan(options, ReadUserProxyState());
        cache->options = options;
        cache->cacheMs = boundedCacheMs;
        cache->refreshedAt = now;
    }
    return cache->plan;
}

} // namespace pluginping_shared
