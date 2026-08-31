// PluginPing - DIRECT/PROXY IP display item.
#pragma once

#include "Common.h"
#include "../include/PluginInterface.h"
#include "StatusDataStore.h"
#include "ConfigManager.h"
#include "../../shared_src/CompositeSection.h"
#include "../../shared_src/HostDrawing.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace pluginping {

enum class PingEndpoint
{
    Local,
    Remote
};

struct EndpointWidthState
{
    std::mutex mutex;
    bool hasGeometry = false;
    int dpi = 96;
    bool compactMode = false;
    pluginping_shared::HostFontSignature hostFont;
    int highWater = 0;
};

class CPingStatusItem : public IPluginItem, public pluginping_shared::ICompositeSection
{
public:
    explicit CPingStatusItem(PingEndpoint endpoint);

    void Attach(StatusDataStore* store, ConfigManager* config,
                std::shared_ptr<EndpointWidthState> widthState = {})
    {
        m_store = store;
        m_config = config;
        m_widthState = std::move(widthState);
    }

    unsigned long long LastDrawTick() const { return m_lastDrawTick.load(); }
    // Shared measured DIRECT/PROXY IP-section width used by both composite rows.
    int GetEndpointWidthEx(void* hDC) const;
    IPluginItem* HostItem() override { return this; }
    int MeasureCompositeWidth(void* hDC) const override { return GetEndpointWidthEx(hDC); }
    std::wstring CompositeTooltip() const override;

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

private:
    struct WidthCacheKey
    {
        int dpi = 96;
        int configWidth = 0;
        bool compactMode = false;
        pluginping_shared::HostFontSignature hostFont;
        std::wstring localLabel;
        std::wstring localCountryCode;
        std::wstring localIpAddress;
        std::wstring remoteLabel;
        std::wstring remoteCountryCode;
        std::wstring remoteIpAddress;
    };

    static WidthCacheKey BuildWidthCacheKey(const LocalRemoteSnapshot& snapshot,
                                            const PluginConfig& cfg, HDC dc);
    static bool SameWidthCacheKey(const WidthCacheKey& a, const WidthCacheKey& b);
    void MarkDisplayed() const;

    PingEndpoint m_endpoint;
    StatusDataStore* m_store = nullptr;
    ConfigManager* m_config = nullptr;
    std::shared_ptr<EndpointWidthState> m_widthState;
    mutable std::atomic<unsigned long long> m_lastDrawTick{ 0 };
    mutable std::mutex m_widthMutex;
    mutable bool m_hasWidthKey = false;
    mutable WidthCacheKey m_widthKey;
    mutable int m_widthCacheValue = 0;
};

} // namespace pluginping
