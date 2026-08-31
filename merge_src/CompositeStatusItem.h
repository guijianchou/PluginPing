// PluginPing - one host-visible row composed from an IP item and a probe item.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "../ping_src/include/PluginInterface.h"
#include "../shared_src/CompositeSection.h"

namespace pluginpingmerge {

enum class CompositeRoute
{
    Direct,
    Proxy
};

class CCompositeStatusItem final : public IPluginItem
{
public:
    CCompositeStatusItem(CompositeRoute route,
                         pluginping_shared::ICompositeSection* ipSection,
                         pluginping_shared::ICompositeSection* probeSection);

    bool IsReady() const { return m_ipSection != nullptr && m_probeSection != nullptr; }

    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;

    bool IsCustomDraw() const override;
    std::wstring CompositeTooltip() const;
    int GetItemWidth() const override;
    int GetItemWidthEx(void* hDC) const override;
    void DrawItem(void* hDC, int x, int y, int w, int h, bool darkMode) override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;

    int IsDrawResourceUsageGraph() const override;
    float GetResourceUsageGraphValue() const override;

private:
    int ChildWidth(pluginping_shared::ICompositeSection* section, HDC dc) const;

    CompositeRoute m_route;
    pluginping_shared::ICompositeSection* m_ipSection = nullptr;
    pluginping_shared::ICompositeSection* m_probeSection = nullptr;
};

} // namespace pluginpingmerge
