// PluginPing - IP and shared presentation configuration.
#pragma once

#include <memory>
#include <mutex>
#include <string>

namespace pluginping {

inline void MigrateLegacyResolveIntervals(int configVersion,
                                          int& directIntervalMs,
                                          int& proxyIntervalMs)
{
    if (configVersion >= 2)
        return;
    if (directIntervalMs == 30000)
        directIntervalMs = 10000;
    if (proxyIntervalMs == 5000)
        proxyIntervalMs = 1000;
}

// Size values are expressed at 96 DPI and scaled while drawing.
struct PluginConfig
{
    // [IP]
    int width = 160;
    int localResolveIntervalMs = 10000;
    int remoteResolveIntervalMs = 1000;
    int ipFailureBackoffMs = 60000;
    int timeoutMs = 3000;
    int failThreshold = 3;
    bool debugLog = false;
    std::wstring localDisplayIp;

    // [Network] - shared with TCP/UDP routing.
    std::wstring proxyMode = L"Auto"; // Auto | Direct | System | Manual
    std::wstring proxyServer = L"127.0.0.1:7890";
    std::wstring proxyBypass = L"<local>";
    int proxyCacheMs = 5000;
    std::wstring socks5Server;
    bool shareProxyIdentityForUdp = false;

    // [General] visibility age shared with the probe engine.
    int idleAfterMs = 60000;

    // [Style] - typography is inherited from the host drawing context.
    bool compactMode = false;
};

class ConfigManager
{
public:
    // Records the host directory only. Disk access remains on the worker thread.
    void SetConfigDir(const std::wstring& dir);
    std::shared_ptr<const PluginConfig> Reload();
    std::shared_ptr<const PluginConfig> GetShared() const;
    std::wstring DefaultSiblingFile(const std::wstring& fileName) const;

private:
    std::wstring ResolveConfigPath() const;
    void LoadLocked();
    mutable std::mutex m_mutex;
    std::shared_ptr<const PluginConfig> m_config = std::make_shared<PluginConfig>();
    std::wstring m_configDir;
    std::wstring m_configPath;
    unsigned long long m_configGeneration = 0;
    bool m_loaded = false;
};

} // namespace pluginping
