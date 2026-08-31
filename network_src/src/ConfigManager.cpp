// pluginNetwork - configuration manager implementation
#include "Common.h"
#include "ConfigManager.h"
#include "../../shared_src/ConfigFile.h"

namespace pluginnetwork {

namespace {

const wchar_t* const kIniFileName = L"PluginPing.ini";

// Written verbatim (as UTF-16LE) the first time the plugin runs without an ini,
// so every knob is discoverable without reading the source. Newlines are
// converted to CRLF on write for Notepad's benefit.
const wchar_t* const kDefaultIni =
LR"INI(; PluginPing 1.0.14
; One configuration surface for DIRECT, TCP, PROXY and UDP.

[General]
ConfigVersion=2
RefreshInterval=1000
Timeout=1000
MaxFailCount=3
WindowMs=20000
MinSamples=5
RecentAvgCount=6
IdleAfterMs=60000
IdleRefreshInterval=30000
Width=110
DebugLog=false

[IP]
Width=160
DirectResolveIntervalMs=10000
ProxyResolveIntervalMs=1000
; Applies to DIRECT resolution only; PROXY keeps the fast interval above.
FailureBackoffMs=60000
TimeoutMs=3000
FailThreshold=3
DebugLog=false
DirectDisplayIp=

[Color_Rules]
GreenThreshold=97
YellowThreshold=85
GreenColor=
YellowColor=
RedColor=

[Network]
; Auto | Direct | System | Manual
ProxyMode=Auto
ProxyServer=127.0.0.1:7890
ProxyBypass=<local>
ProxyCacheMs=5000
Socks5Server=
; Set true only when the HTTP and SOCKS endpoints select the same upstream node.
ShareProxyIdentityForUdp=false

[TCP_Settings]
Enable=1
TargetUrl=https://www.google.com/generate_204
FallbackUrl=https://cp.cloudflare.com/generate_204
ExpectStatus=204

[UDP_Settings]
Enable=1
EchoServer=sg.falsemeet.site:3478
FallbackStunServer=stun.cloudflare.com:3478
BurstPackets=1
DnsRefreshMs=300000

[Style]
CompactMode=false
ShowResourceGraph=false
)INI";

std::wstring GetModuleDir()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(PLUGINNETWORK_HMODULE, path, MAX_PATH);
    const std::wstring full(path);
    const size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : full.substr(0, pos + 1);
}

std::wstring EnsureTrailingSep(const std::wstring& dir)
{
    if (dir.empty())
        return dir;
    const wchar_t last = dir.back();
    if (last == L'\\' || last == L'/')
        return dir;
    return dir + L'\\';
}

bool FileExists(const std::wstring& path)
{
    if (path.empty())
        return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// CREATE_NEW gives the "only when absent" semantics for free, so a user's file
// can never be clobbered by a race with another instance.
void WriteDefaultIni(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;     // exists already, or the directory is not writable

    std::wstring text;
    text.reserve(wcslen(kDefaultIni) + 256);
    text.push_back(static_cast<wchar_t>(0xFEFF));      // BOM: makes the profile APIs read it as UTF-16
    for (const wchar_t* p = kDefaultIni; *p; ++p)
    {
        if (*p == L'\n')
            text.push_back(L'\r');
        text.push_back(*p);
    }

    DWORD written = 0;
    const DWORD expected = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const bool complete = WriteFile(file, text.data(), expected, &written, nullptr) != FALSE &&
                          written == expected;
    CloseHandle(file);
    if (!complete)
        DeleteFileW(path.c_str());
}

} // namespace

void ConfigManager::SetConfigDir(const std::wstring& dir)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_configDir = dir;
    m_configPath.clear();   // force re-resolution
    m_loaded = false;
    m_configGeneration = 0;
}

std::shared_ptr<const PluginConfig> ConfigManager::Reload()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_loaded)
        LoadLocked();
    else if (pluginping_shared::GetIniSnapshot(m_configPath)->Generation() !=
             m_configGeneration)
        LoadLocked();
    return m_config;
}

std::shared_ptr<const PluginConfig> ConfigManager::GetShared() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_config;
}

std::wstring ConfigManager::ConfigPath() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_configPath.empty() ? ResolveConfigPath() : m_configPath;
}

std::wstring ConfigManager::DefaultSiblingFile(const std::wstring& fileName) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const std::wstring dllPath = GetModuleDir() + fileName;
    if (!m_configDir.empty())
    {
        const std::wstring configPath = EnsureTrailingSep(m_configDir) + fileName;
        if (FileExists(configPath) || !FileExists(dllPath))
            return configPath;
    }
    return dllPath;
}

std::wstring ConfigManager::ResolveConfigPath() const
{
    // The host always supplies EI_CONFIG_DIR, so without the DLL-directory
    // fallback an ini deployed next to the DLL would never be read.
    const std::wstring dllPath = GetModuleDir() + kIniFileName;
    if (!m_configDir.empty())
    {
        const std::wstring configPath = EnsureTrailingSep(m_configDir) + kIniFileName;
        if (FileExists(configPath) || !FileExists(dllPath))
            return configPath;
    }
    return dllPath;
}

void ConfigManager::LoadLocked()
{
    if (m_configPath.empty())
        m_configPath = ResolveConfigPath();
    if (!FileExists(m_configPath))
        WriteDefaultIni(m_configPath);

    const std::wstring& p = m_configPath;
    PluginConfig c;     // start from defaults; every missing key keeps its default
    const auto snapshot = pluginping_shared::GetIniSnapshot(p);

    // [General]
    c.refreshIntervalMs = snapshot->Integer(L"General", L"RefreshInterval", c.refreshIntervalMs);
    c.timeoutMs = snapshot->Integer(L"General", L"Timeout", c.timeoutMs);
    c.maxFailCount = snapshot->Integer(L"General", L"MaxFailCount", c.maxFailCount);
    c.windowMs = snapshot->Integer(L"General", L"WindowMs", c.windowMs);
    c.minSamples = snapshot->Integer(L"General", L"MinSamples", c.minSamples);
    c.recentAvgCount = snapshot->Integer(L"General", L"RecentAvgCount", c.recentAvgCount);
    c.idleAfterMs = snapshot->Integer(L"General", L"IdleAfterMs", c.idleAfterMs);
    c.idleRefreshIntervalMs = snapshot->Integer(L"General", L"IdleRefreshInterval", c.idleRefreshIntervalMs);
    c.width = snapshot->Integer(L"General", L"Width", c.width);
    c.debugLog = snapshot->Boolean(L"General", L"DebugLog", c.debugLog);

    // [Color_Rules]
    c.greenThreshold = snapshot->Integer(L"Color_Rules", L"GreenThreshold", c.greenThreshold);
    c.yellowThreshold = snapshot->Integer(L"Color_Rules", L"YellowThreshold", c.yellowThreshold);
    c.greenColor = snapshot->String(L"Color_Rules", L"GreenColor", c.greenColor);
    c.yellowColor = snapshot->String(L"Color_Rules", L"YellowColor", c.yellowColor);
    c.redColor = snapshot->String(L"Color_Rules", L"RedColor", c.redColor);

    // [Network]
    c.proxyMode = snapshot->String(L"Network", L"ProxyMode", c.proxyMode);
    c.proxyServer = snapshot->String(L"Network", L"ProxyServer", c.proxyServer);
    c.proxyBypass = snapshot->String(L"Network", L"ProxyBypass", c.proxyBypass);
    c.proxyCacheMs = snapshot->Integer(L"Network", L"ProxyCacheMs", c.proxyCacheMs);
    c.socks5Server = snapshot->String(L"Network", L"Socks5Server", c.socks5Server);
    c.shareProxyIdentityForUdp = snapshot->Boolean(
        L"Network", L"ShareProxyIdentityForUdp", c.shareProxyIdentityForUdp);

    // [TCP_Settings]
    c.tcpEnabled = snapshot->Boolean(L"TCP_Settings", L"Enable", c.tcpEnabled);
    // Google is the sole authority for TCP health. The legacy TargetUrl key is
    // deliberately ignored; otherwise a directly reachable custom endpoint can
    // turn a disabled overseas proxy into a false healthy result.
    c.tcpTargetUrl = L"https://www.google.com/generate_204";
    c.tcpFallbackUrl = snapshot->String(L"TCP_Settings", L"FallbackUrl", c.tcpFallbackUrl);
    c.tcpExpectStatus = snapshot->Integer(L"TCP_Settings", L"ExpectStatus", c.tcpExpectStatus);

    // [UDP_Settings]
    c.udpEnabled = snapshot->Boolean(L"UDP_Settings", L"Enable", c.udpEnabled);
    c.udpEchoServer = snapshot->String(L"UDP_Settings", L"EchoServer", c.udpEchoServer);
    c.udpFallbackStunServer = snapshot->String(L"UDP_Settings", L"FallbackStunServer",
                                               c.udpFallbackStunServer);
    c.burstPackets = snapshot->Integer(L"UDP_Settings", L"BurstPackets", c.burstPackets);
    c.dnsRefreshMs = snapshot->Integer(L"UDP_Settings", L"DnsRefreshMs", c.dnsRefreshMs);

    // [Style]
    c.compactMode = snapshot->Boolean(L"Style", L"CompactMode", c.compactMode);
    c.showResourceGraph = snapshot->Boolean(L"Style", L"ShowResourceGraph", c.showResourceGraph);

    // Robustness clamps. A hand-edited ini must never be able to produce a
    // divide-by-zero, a probe storm, or an unreadable row.
    c.refreshIntervalMs = std::clamp(c.refreshIntervalMs, 500, 600000);
    c.timeoutMs = std::clamp(c.timeoutMs, 100, 10000);
    c.maxFailCount = std::clamp(c.maxFailCount, 1, 20);
    c.windowMs = std::clamp(c.windowMs, 10000, 3600000);
    c.recentAvgCount = std::clamp(c.recentAvgCount, 1, 60);
    c.width = std::clamp(c.width, 40, 3840);
    c.burstPackets = std::clamp(c.burstPackets, 1, 10);
    c.dnsRefreshMs = std::clamp(c.dnsRefreshMs, 10000, 86400000);
    c.proxyCacheMs = std::clamp(c.proxyCacheMs, 1000, 60000);
    if (c.tcpExpectStatus < 0 || c.tcpExpectStatus > 599)
        c.tcpExpectStatus = 0;

    c.greenThreshold = std::clamp(c.greenThreshold, 1, 100);
    c.yellowThreshold = std::clamp(c.yellowThreshold, 0, 100);
    // An inverted pair would make the yellow band unreachable and silently turn
    // every degraded state straight to red.
    if (c.yellowThreshold >= c.greenThreshold)
        c.yellowThreshold = (std::max)(0, c.greenThreshold - 1);

    if (c.idleAfterMs != 0)
        c.idleAfterMs = std::clamp(c.idleAfterMs, 10000, 3600000);
    c.idleRefreshIntervalMs = std::clamp(c.idleRefreshIntervalMs, c.refreshIntervalMs, 600000);

    // MinSamples above the window capacity would pin the row in warm-up forever.
    const int smallestCapacity = c.WindowCapacity(c.refreshIntervalMs, 1);
    c.minSamples = std::clamp(c.minSamples, 1, smallestCapacity);

    if (c.udpEchoServer.empty() || c.udpFallbackStunServer.empty())
        c.udpEnabled = false;

    if (_wcsicmp(c.proxyMode.c_str(), L"Auto") != 0 &&
        _wcsicmp(c.proxyMode.c_str(), L"Direct") != 0 &&
        _wcsicmp(c.proxyMode.c_str(), L"System") != 0 &&
        _wcsicmp(c.proxyMode.c_str(), L"Manual") != 0)
        c.proxyMode = L"Auto";

    m_config = std::make_shared<PluginConfig>(c);
    m_configGeneration = snapshot->Generation();
    m_loaded = true;
}

} // namespace pluginnetwork
