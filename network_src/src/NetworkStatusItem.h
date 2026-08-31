// pluginNetwork - display item (IPluginItem)
#pragma once
#include "Common.h"
#include "../../ping_src/include/PluginInterface.h"
#include "ConfigManager.h"
#include "MetricsData.h"
#include "MetricsStore.h"
#include "../../shared_src/CompositeSection.h"
#include "../../shared_src/HostDrawing.h"
#include <atomic>
#include <mutex>
#include <string>

namespace pluginnetwork {

// One taskbar/main-window row. Custom drawing keeps RTT/JIT in stable columns
// and applies the semantic quality colour to RTT only.
class CNetworkStatusItem : public IPluginItem, public pluginping_shared::ICompositeSection
{
public:
    explicit CNetworkStatusItem(ChannelKind kind);
    ~CNetworkStatusItem();

    // store and config outlive this item.
    void Attach(MetricsStore* store, ConfigManager* config)
    {
        m_store = store;
        m_config = config;
    }

    void SetEmbeddedMode(bool embedded) { m_embeddedMode = embedded; }

    // Tick of the last time the host asked to measure or paint this row. The
    // workers use it to drop to the idle cadence when nothing is displaying it.
    unsigned long long LastDrawTick() const { return m_lastDrawTick.load(); }
    HANDLE ActivityEvent() const { return m_activityEvent; }
    int GetChannelWidthEx(void* hDC) const;
    IPluginItem* HostItem() override { return this; }
    int MeasureCompositeWidth(void* hDC) const override { return GetChannelWidthEx(hDC); }
    std::wstring CompositeTooltip() const override;

    // ===== IPluginItem =====
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;

    bool IsCustomDraw() const override;
    int GetItemWidth() const override;
    int GetItemWidthEx(void* hDC) const override;
    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override;

    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;

    int IsDrawResourceUsageGraph() const override;
    float GetResourceUsageGraphValue() const override;

private:
    // Width follows the larger of a compact baseline and the current two-row
    // content, so the cache key covers both styling and displayed metrics.
    struct WidthCacheKey
    {
        int dpi = 96;
        bool compactMode = false;
        bool embeddedMode = false;
        int tcpAvgRttMs = -1;
        int tcpJitterMs = -1;
        int udpAvgRttMs = -1;
        int udpJitterMs = -1;
        pluginping_shared::HostFontSignature hostFont;

        bool operator==(const WidthCacheKey& other) const
        {
            return dpi == other.dpi &&
                   compactMode == other.compactMode &&
                   embeddedMode == other.embeddedMode &&
                   tcpAvgRttMs == other.tcpAvgRttMs &&
                   tcpJitterMs == other.tcpJitterMs &&
                   udpAvgRttMs == other.udpAvgRttMs &&
                   udpJitterMs == other.udpJitterMs &&
                   pluginping_shared::SameHostFont(hostFont, other.hostFont);
        }
    };

    const ChannelSnapshot& ChannelOf(const MetricsSnapshot& snapshot) const;
    void MarkDisplayed() const;

    ChannelKind m_kind;
    MetricsStore* m_store = nullptr;
    ConfigManager* m_config = nullptr;
    bool m_embeddedMode = false;
    mutable std::atomic<unsigned long long> m_lastDrawTick{ 0 };
    HANDLE m_activityEvent = nullptr;

    mutable std::mutex m_widthMutex;
    mutable bool m_hasWidthKey = false;
    mutable WidthCacheKey m_widthKey;
    mutable int m_widthCacheValue = 0;
};

} // namespace pluginnetwork
