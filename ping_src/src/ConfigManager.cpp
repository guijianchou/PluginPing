// PluginPing - IP configuration loader.
#include "Common.h"
#include "ConfigManager.h"
#include "../../shared_src/ConfigFile.h"

#include <algorithm>

namespace pluginping {

namespace {

constexpr const wchar_t* kIniFileName = L"PluginPing.ini";

std::wstring GetModuleDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(PLUGINPING_HMODULE, path, MAX_PATH);
    const std::wstring full(path);
    const size_t separator = full.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : full.substr(0, separator + 1);
}

std::wstring EnsureTrailingSeparator(const std::wstring& directory)
{
    if (directory.empty() || directory.back() == L'\\' || directory.back() == L'/')
        return directory;
    return directory + L'\\';
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

} // namespace

void ConfigManager::SetConfigDir(const std::wstring& dir)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configDir = dir;
    m_configPath.clear();
    m_loaded = false;
    m_configGeneration = 0;
}

std::shared_ptr<const PluginConfig> ConfigManager::Reload()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_loaded)
        LoadLocked();
    else if (pluginping_shared::GetIniSnapshot(m_configPath)->Generation() !=
             m_configGeneration)
        LoadLocked();
    return m_config;
}

std::shared_ptr<const PluginConfig> ConfigManager::GetShared() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

std::wstring ConfigManager::DefaultSiblingFile(const std::wstring& fileName) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::wstring dllPath = GetModuleDir() + fileName;
    if (!m_configDir.empty())
    {
        const std::wstring configuredPath = EnsureTrailingSeparator(m_configDir) + fileName;
        if (FileExists(configuredPath) || !FileExists(dllPath))
            return configuredPath;
    }
    return dllPath;
}

std::wstring ConfigManager::ResolveConfigPath() const
{
    const std::wstring dllPath = GetModuleDir() + kIniFileName;
    if (!m_configDir.empty())
    {
        const std::wstring configuredPath = EnsureTrailingSeparator(m_configDir) + kIniFileName;
        if (FileExists(configuredPath) || !FileExists(dllPath))
            return configuredPath;
    }
    return dllPath;
}

void ConfigManager::LoadLocked()
{
    if (m_configPath.empty())
        m_configPath = ResolveConfigPath();

    PluginConfig config;
    const std::wstring& path = m_configPath;

    const auto snapshot = pluginping_shared::GetIniSnapshot(path);
    const int configVersion = snapshot->Integer(L"General", L"ConfigVersion", 1);
    config.width = snapshot->Integer(L"IP", L"Width", config.width);
    config.localResolveIntervalMs = snapshot->Integer(L"IP", L"DirectResolveIntervalMs",
                                                       config.localResolveIntervalMs);
    config.remoteResolveIntervalMs = snapshot->Integer(L"IP", L"ProxyResolveIntervalMs",
                                                        config.remoteResolveIntervalMs);
    MigrateLegacyResolveIntervals(configVersion, config.localResolveIntervalMs,
                                  config.remoteResolveIntervalMs);
    config.ipFailureBackoffMs = snapshot->Integer(L"IP", L"FailureBackoffMs",
                                                   config.ipFailureBackoffMs);
    config.timeoutMs = snapshot->Integer(L"IP", L"TimeoutMs", config.timeoutMs);
    config.failThreshold = snapshot->Integer(L"IP", L"FailThreshold", config.failThreshold);
    config.debugLog = snapshot->Boolean(L"IP", L"DebugLog", config.debugLog);
    config.localDisplayIp = snapshot->String(L"IP", L"DirectDisplayIp", config.localDisplayIp);

    config.proxyMode = snapshot->String(L"Network", L"ProxyMode", config.proxyMode);
    config.proxyServer = snapshot->String(L"Network", L"ProxyServer", config.proxyServer);
    config.proxyBypass = snapshot->String(L"Network", L"ProxyBypass", config.proxyBypass);
    config.proxyCacheMs = snapshot->Integer(L"Network", L"ProxyCacheMs", config.proxyCacheMs);
    config.socks5Server = snapshot->String(L"Network", L"Socks5Server", config.socks5Server);
    config.shareProxyIdentityForUdp = snapshot->Boolean(
        L"Network", L"ShareProxyIdentityForUdp", config.shareProxyIdentityForUdp);

    config.idleAfterMs = snapshot->Integer(L"General", L"IdleAfterMs", config.idleAfterMs);

    config.compactMode = snapshot->Boolean(L"Style", L"CompactMode", config.compactMode);

    config.width = std::clamp(config.width, 80, 3840);
    config.localResolveIntervalMs = std::clamp(config.localResolveIntervalMs, 5000, 600000);
    config.remoteResolveIntervalMs = std::clamp(config.remoteResolveIntervalMs, 1000, 600000);
    config.ipFailureBackoffMs = std::clamp(config.ipFailureBackoffMs, 10000, 300000);
    config.timeoutMs = std::clamp(config.timeoutMs, 100, 10000);
    config.failThreshold = std::clamp(config.failThreshold, 1, 20);
    config.proxyCacheMs = std::clamp(config.proxyCacheMs, 1000, 60000);
    if (config.idleAfterMs != 0)
        config.idleAfterMs = std::clamp(config.idleAfterMs, 10000, 3600000);
    if (_wcsicmp(config.proxyMode.c_str(), L"Auto") != 0 &&
        _wcsicmp(config.proxyMode.c_str(), L"Direct") != 0 &&
        _wcsicmp(config.proxyMode.c_str(), L"System") != 0 &&
        _wcsicmp(config.proxyMode.c_str(), L"Manual") != 0)
        config.proxyMode = L"Auto";

    m_config = std::make_shared<PluginConfig>(std::move(config));
    m_configGeneration = snapshot->Generation();
    m_loaded = true;
}

} // namespace pluginping
