// pluginNetwork - configuration manager
#pragma once
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

namespace pluginnetwork {

// Size values are expressed at 96 DPI and scaled while drawing.
struct PluginConfig
{
    // [General]
    int refreshIntervalMs = 1000;       // sampling cadence per channel
    int timeoutMs = 1000;               // single probe hard timeout
    int maxFailCount = 3;               // consecutive failures before DISCONNECTED
    int windowMs = 20000;               // success/failure quality window length
    int minSamples = 5;                 // below this the row stays in warm-up
    int recentAvgCount = 6;             // latency/jitter use this many recent successes
    int idleAfterMs = 60000;            // no draw for this long -> idle cadence
    int idleRefreshIntervalMs = 30000;  // cadence while nothing is displaying the row
    int width = 110;                    // GetItemWidth fallback at 96 DPI
    bool debugLog = false;

    // [Color_Rules]
    int greenThreshold = 97;            // stability >= this is green
    int yellowThreshold = 85;           // stability >= this is yellow, below is red
    std::wstring greenColor;            // optional #RRGGBB overrides; empty = theme default
    std::wstring yellowColor;
    std::wstring redColor;

    // [Network]
    // Auto follows the current user's enabled static Windows proxy. With no
    // enabled local proxy it becomes Direct, preserving router-side routing.
    std::wstring proxyMode = L"Auto";     // Auto | Direct | System | Manual
    std::wstring proxyServer = L"127.0.0.1:7890"; // Manual HTTP/mixed endpoint
    std::wstring proxyBypass = L"<local>";
    int proxyCacheMs = 5000;
    std::wstring socks5Server;            // optional UDP override, host:port
    bool shareProxyIdentityForUdp = false; // explicit same-upstream declaration

    // [TCP_Settings]
    bool tcpEnabled = true;
    // Measured over a keep-alive HTTPS connection so the timing covers a full
    // round trip through the proxy tunnel. A bare TCP handshake would be
    // answered locally by a router-side transparent proxy and report LAN
    // latency for a link that may be completely dead.
    std::wstring tcpTargetUrl = L"https://www.google.com/generate_204";
    std::wstring tcpFallbackUrl = L"https://cp.cloudflare.com/generate_204";
    int tcpExpectStatus = 204;           // <= 0 accepts any 2xx

    // [UDP_Settings]
    bool udpEnabled = true;
    std::wstring udpEchoServer = L"sg.falsemeet.site:3478";
    std::wstring udpFallbackStunServer = L"stun.cloudflare.com:3478";
    int burstPackets = 1;                 // UDP requests per sampling round
    int dnsRefreshMs = 300000;            // re-resolve the UDP host at most this often

    // [Style]
    bool compactMode = false;
    bool showResourceGraph = false;

    // Samples the window holds at the active probe cadence.
    int WindowCapacity(int cadenceMs, int samplesPerRound) const
    {
        const int rounds = (std::max)(1, windowMs / (std::max)(cadenceMs, 1));
        return (std::max)(1, rounds * (std::max)(1, samplesPerRound));
    }
};

class ConfigManager
{
public:
    // Records the host-supplied directory. Deliberately does not touch the disk:
    // the host calls this on its monitor thread during plugin load, and the
    // actual read happens on a worker.
    void SetConfigDir(const std::wstring& dir);

    std::shared_ptr<const PluginConfig> Reload();
    // Hot-path read for draw callbacks: hands back the immutable published
    // config without copying every string field.
    std::shared_ptr<const PluginConfig> GetShared() const;
    std::wstring DefaultSiblingFile(const std::wstring& fileName) const;
    std::wstring ConfigPath() const;

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

} // namespace pluginnetwork
