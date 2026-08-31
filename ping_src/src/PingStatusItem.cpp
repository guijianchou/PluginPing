// PluginPing - IP display item implementation.
#include "Common.h"
#include "PingStatusItem.h"
#include "Renderer.h"

#include <algorithm>
#include <memory>

namespace pluginping {

CPingStatusItem::CPingStatusItem(PingEndpoint endpoint)
    : m_endpoint(endpoint)
{
}

void CPingStatusItem::MarkDisplayed() const
{
    m_lastDrawTick.store(GetTickCount64());
}

std::wstring CPingStatusItem::CompositeTooltip() const
{
    try
    {
        if (!m_store || LastDrawTick() == 0)
            return {};
        const auto snapshot = m_store->GetSnapshotShared();
        const EndpointStatus& endpoint = m_endpoint == PingEndpoint::Local
            ? snapshot->local : snapshot->remote;
        const wchar_t* label = m_endpoint == PingEndpoint::Local ? L"DIRECT" : L"PROXY";
        const std::wstring country = endpoint.countryCode.empty() ? L"--" : endpoint.countryCode;
        if (endpoint.displayAsnOnly)
            return std::wstring(label) + L": " + country;
        const std::wstring ip = endpoint.ipAddress.empty() ? L"--" : endpoint.ipAddress;
        return std::wstring(label) + L": " + country + L" | " + ip;
    }
    catch (...)
    {
        return {};
    }
}

const wchar_t* CPingStatusItem::GetItemName() const
{
    return m_endpoint == PingEndpoint::Local ? L"PluginPing DIRECT IP"
                                             : L"PluginPing PROXY IP";
}

const wchar_t* CPingStatusItem::GetItemId() const
{
    return m_endpoint == PingEndpoint::Local ? L"PluginPingDirectIpInternal"
                                             : L"PluginPingProxyIpInternal";
}

const wchar_t* CPingStatusItem::GetItemLableText() const { return L""; }
const wchar_t* CPingStatusItem::GetItemValueText() const { return L""; }
const wchar_t* CPingStatusItem::GetItemValueSampleText() const { return L""; }
bool CPingStatusItem::IsCustomDraw() const { return true; }

int CPingStatusItem::GetItemWidth() const
{
    try
    {
        if (m_config)
            return m_config->GetShared()->width;
    }
    catch (...)
    {
    }
    return 160;
}

int CPingStatusItem::GetItemWidthEx(void* hDC) const
{
    try
    {
        if (!hDC || !m_store || !m_config)
            return 0;
        MarkDisplayed();
        HDC dc = static_cast<HDC>(hDC);
        const auto snapshot = m_store->GetSnapshotShared();
        const auto cfg = m_config->GetShared();
        WidthCacheKey key = BuildWidthCacheKey(*snapshot, *cfg, dc);
        {
            std::lock_guard<std::mutex> lock(m_widthMutex);
            if (m_hasWidthKey && m_widthCacheValue > 0 && SameWidthCacheKey(key, m_widthKey))
                return m_widthCacheValue;
        }

        const int measured = MeasureSnapshotWidth(dc, *snapshot, *cfg);
        if (measured <= 0)
            return 0;
        const int dpi = key.dpi > 0 ? key.dpi : 96;
        const int minimum = MulDiv(cfg->width, dpi, 96);
        const int width = (std::max)(measured, minimum);
        {
            std::lock_guard<std::mutex> lock(m_widthMutex);
            m_widthKey = std::move(key);
            m_hasWidthKey = true;
            m_widthCacheValue = width;
        }
        return width;
    }
    catch (...)
    {
        return 0;
    }
}

int CPingStatusItem::GetEndpointWidthEx(void* hDC) const
{
    try
    {
        if (!hDC || !m_store || !m_config)
            return 0;
        MarkDisplayed();
        const auto snapshot = m_store->GetSnapshotShared();
        const auto cfg = m_config->GetShared();
        // Both composite rows use the same measured IP geometry so RTT starts
        // on one vertical axis. The standalone Width setting is deliberately
        // excluded here: applying its 160-pixel floor creates blank space
        // between the rendered IP and the embedded RTT section. Content width
        // remains a session high-water mark, so N/A or -- cannot move RTT.
        const HDC dc = static_cast<HDC>(hDC);
        const int measured = MeasureSnapshotWidth(dc, *snapshot, *cfg);
        if (measured <= 0)
            return 0;
        const pluginping_shared::HostFont hostFont =
            pluginping_shared::CaptureHostFont(dc);
        const int dpi = (std::max)(GetDeviceCaps(dc, LOGPIXELSX), 96);
        const int candidate = measured;
        if (!m_widthState)
            return candidate;
        std::lock_guard<std::mutex> lock(m_widthState->mutex);
        const bool sameGeometry = m_widthState->hasGeometry &&
            m_widthState->dpi == dpi &&
            m_widthState->compactMode == cfg->compactMode &&
            pluginping_shared::SameHostFont(m_widthState->hostFont,
                                            hostFont.signature);
        if (!sameGeometry)
        {
            m_widthState->hasGeometry = true;
            m_widthState->dpi = dpi;
            m_widthState->compactMode = cfg->compactMode;
            m_widthState->hostFont = hostFont.signature;
            m_widthState->highWater = candidate;
        }
        else
        {
            m_widthState->highWater = (std::max)(m_widthState->highWater, candidate);
        }
        return m_widthState->highWater;
    }
    catch (...)
    {
        return 0;
    }
}

void CPingStatusItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    try
    {
        if (!hDC || !m_store || !m_config)
            return;
        MarkDisplayed();
        const auto snapshot = m_store->GetSnapshotShared();
        const auto cfg = m_config->GetShared();
        DrawStatusRow(static_cast<HDC>(hDC), x, y, w, h, dark_mode, *snapshot,
                      m_endpoint == PingEndpoint::Local, *cfg);
    }
    catch (...)
    {
    }
}

int CPingStatusItem::OnMouseEvent(MouseEventType /*type*/, int /*x*/, int /*y*/,
                                  void* /*hWnd*/, int /*flag*/)
{
    return 0;
}

CPingStatusItem::WidthCacheKey CPingStatusItem::BuildWidthCacheKey(
    const LocalRemoteSnapshot& snapshot, const PluginConfig& cfg, HDC dc)
{
    WidthCacheKey key;
    key.dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (key.dpi <= 0)
        key.dpi = 96;
    key.configWidth = cfg.width;
    key.compactMode = cfg.compactMode;
    key.hostFont = pluginping_shared::CaptureHostFont(dc).signature;
    key.localLabel = snapshot.local.label;
    key.localCountryCode = snapshot.local.countryCode;
    key.localIpAddress = snapshot.local.ipAddress;
    key.remoteLabel = snapshot.remote.label;
    key.remoteCountryCode = snapshot.remote.countryCode;
    key.remoteIpAddress = snapshot.remote.ipAddress;
    return key;
}

bool CPingStatusItem::SameWidthCacheKey(const WidthCacheKey& a, const WidthCacheKey& b)
{
    return a.dpi == b.dpi && a.configWidth == b.configWidth &&
           a.compactMode == b.compactMode &&
           pluginping_shared::SameHostFont(a.hostFont, b.hostFont) &&
           a.localLabel == b.localLabel &&
           a.localCountryCode == b.localCountryCode && a.localIpAddress == b.localIpAddress &&
           a.remoteLabel == b.remoteLabel && a.remoteCountryCode == b.remoteCountryCode &&
           a.remoteIpAddress == b.remoteIpAddress;
}

} // namespace pluginping
