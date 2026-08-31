// PluginPing - composite DIRECT/PROXY display item.
#include "CompositeStatusItem.h"
#include <algorithm>

namespace pluginpingmerge {

namespace {

constexpr int kTrailingPadding96 = 6;
constexpr int kSectionGap96 = 6;

int ScaledTrailingPadding(HDC dc)
{
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dpi <= 0)
        dpi = 96;
    return (std::max)(1, MulDiv(kTrailingPadding96, dpi, 96));
}

int SectionGap(HDC dc)
{
    if (dc)
    {
        SIZE size{};
        if (GetTextExtentPoint32W(dc, L" ", 1, &size) && size.cx > 0)
            return size.cx * 2;
    }

    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dpi <= 0)
        dpi = 96;
    return (std::max)(1, MulDiv(kSectionGap96, dpi, 96));
}

} // namespace

CCompositeStatusItem::CCompositeStatusItem(
    CompositeRoute route, pluginping_shared::ICompositeSection* ipSection,
    pluginping_shared::ICompositeSection* probeSection)
    : m_route(route), m_ipSection(ipSection), m_probeSection(probeSection)
{
}

const wchar_t* CCompositeStatusItem::GetItemName() const
{
    return m_route == CompositeRoute::Direct ? L"PluginPing DIRECT"
                                              : L"PluginPing PROXY";
}

const wchar_t* CCompositeStatusItem::GetItemId() const
{
    // Preserve the original PluginPing IDs so existing TrafficMonitor item
    // visibility and ordering settings continue to apply.
    return m_route == CompositeRoute::Direct ? L"PluginPingLocal"
                                              : L"PluginPingRemote";
}

const wchar_t* CCompositeStatusItem::GetItemLableText() const { return L""; }
const wchar_t* CCompositeStatusItem::GetItemValueText() const { return L""; }
const wchar_t* CCompositeStatusItem::GetItemValueSampleText() const { return L""; }
bool CCompositeStatusItem::IsCustomDraw() const { return true; }

std::wstring CCompositeStatusItem::CompositeTooltip() const
{
    try
    {
        const std::wstring ip = m_ipSection ? m_ipSection->CompositeTooltip() : std::wstring();
        const std::wstring probe = m_probeSection ? m_probeSection->CompositeTooltip() : std::wstring();
        if (ip.empty())
            return probe;
        if (probe.empty())
            return ip;
        return ip + L" | " + probe;
    }
    catch (...)
    {
        return {};
    }
}

int CCompositeStatusItem::GetItemWidth() const
{
    try
    {
        if (!IsReady())
            return 0;
        return (std::max)(m_ipSection->HostItem()->GetItemWidth(), 1) +
               (std::max)(m_probeSection->HostItem()->GetItemWidth(), 1) +
               kSectionGap96 + kTrailingPadding96;
    }
    catch (...)
    {
        return 0;
    }
}

int CCompositeStatusItem::ChildWidth(pluginping_shared::ICompositeSection* section,
                                     HDC dc) const
{
    if (!section || !dc)
        return 0;
    const int sectionWidth = section->MeasureCompositeWidth(dc);
    if (sectionWidth > 0)
        return sectionWidth;
    IPluginItem* item = section->HostItem();
    if (!item)
        return 0;
    const int measured = item->GetItemWidthEx(dc);
    if (measured > 0)
        return measured;
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    if (dpi <= 0)
        dpi = 96;
    return MulDiv((std::max)(item->GetItemWidth(), 1), dpi, 96);
}

int CCompositeStatusItem::GetItemWidthEx(void* hDC) const
{
    try
    {
        if (!hDC || !IsReady())
            return 0;
        HDC dc = static_cast<HDC>(hDC);
        const int ipWidth = ChildWidth(m_ipSection, dc);
        const int probeWidth = ChildWidth(m_probeSection, dc);
        return ipWidth > 0 && probeWidth > 0
                   ? ipWidth + SectionGap(dc) + probeWidth + ScaledTrailingPadding(dc)
                   : 0;
    }
    catch (...)
    {
        return 0;
    }
}

void CCompositeStatusItem::DrawItem(void* hDC, int x, int y, int w, int h,
                                    bool darkMode)
{
    try
    {
        if (!hDC || !IsReady() || w < 2 || h <= 0)
            return;

        HDC dc = static_cast<HDC>(hDC);
        const int saved = SaveDC(dc);
        if (saved == 0)
            return;
        IntersectClipRect(dc, x, y, x + w, y + h);

        IPluginItem* ipItem = m_ipSection->HostItem();
        IPluginItem* probeItem = m_probeSection->HostItem();
        if (!ipItem || !probeItem)
        {
            RestoreDC(dc, saved);
            return;
        }
        const int trailingPadding = (std::min)(ScaledTrailingPadding(dc), w - 2);
        const int contentWidth = w - trailingPadding;
        const int sectionGap = (std::min)(SectionGap(dc), contentWidth - 2);
        const int childContentWidth = contentWidth - sectionGap;
        const int requestedIpWidth = ChildWidth(m_ipSection, dc);
        const int ipWidth = std::clamp(requestedIpWidth, 1, childContentWidth - 1);
        ipItem->DrawItem(dc, x, y, ipWidth, h, darkMode);
        probeItem->DrawItem(dc, x + ipWidth + sectionGap, y,
                            childContentWidth - ipWidth, h, darkMode);

        RestoreDC(dc, saved);
    }
    catch (...)
    {
    }
}

int CCompositeStatusItem::OnMouseEvent(MouseEventType type, int x, int y,
                                       void* hWnd, int flag)
{
    try
    {
        IPluginItem* ipItem = m_ipSection ? m_ipSection->HostItem() : nullptr;
        IPluginItem* probeItem = m_probeSection ? m_probeSection->HostItem() : nullptr;
        if (ipItem && ipItem->OnMouseEvent(type, x, y, hWnd, flag) != 0)
            return 1;
        return probeItem ? probeItem->OnMouseEvent(type, x, y, hWnd, flag) : 0;
    }
    catch (...)
    {
        return 0;
    }
}

int CCompositeStatusItem::IsDrawResourceUsageGraph() const
{
    try
    {
        IPluginItem* item = m_probeSection ? m_probeSection->HostItem() : nullptr;
        return item ? item->IsDrawResourceUsageGraph() : 0;
    }
    catch (...)
    {
        return 0;
    }
}

float CCompositeStatusItem::GetResourceUsageGraphValue() const
{
    try
    {
        IPluginItem* item = m_probeSection ? m_probeSection->HostItem() : nullptr;
        return item ? item->GetResourceUsageGraphValue() : 0.0f;
    }
    catch (...)
    {
        return 0.0f;
    }
}

} // namespace pluginpingmerge
