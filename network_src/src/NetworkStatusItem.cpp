// pluginNetwork - display item implementation
#include "Common.h"
#include "NetworkStatusItem.h"
#include "Renderer.h"
#include <algorithm>
#include <memory>

namespace pluginnetwork {

namespace {

const wchar_t* TooltipState(ChannelState state)
{
    switch (state)
    {
    case ChannelState::Disabled: return L"off";
    case ChannelState::Warming: return L"warming";
    case ChannelState::Ok: return L"ok";
    case ChannelState::Degraded: return L"degraded";
    case ChannelState::Disconnected: return L"down";
    case ChannelState::Unsupported: return L"N/A";
    default: return L"unknown";
    }
}

std::wstring TooltipMetric(int value)
{
    return value < 0 ? L"--" : std::to_wstring(value);
}

} // namespace

CNetworkStatusItem::CNetworkStatusItem(ChannelKind kind)
    : m_kind(kind), m_activityEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
}

CNetworkStatusItem::~CNetworkStatusItem()
{
    if (m_activityEvent)
        CloseHandle(m_activityEvent);
}

const ChannelSnapshot& CNetworkStatusItem::ChannelOf(const MetricsSnapshot& snapshot) const
{
    return (m_kind == ChannelKind::Tcp) ? snapshot.tcp : snapshot.udp;
}

void CNetworkStatusItem::MarkDisplayed() const
{
    const unsigned long long now = GetTickCount64();
    const unsigned long long previous = m_lastDrawTick.exchange(now);
    if (m_activityEvent && (previous == 0 || now - previous >= 1000))
        SetEvent(m_activityEvent);
}

std::wstring CNetworkStatusItem::CompositeTooltip() const
{
    try
    {
        if (!m_store || LastDrawTick() == 0)
            return {};
        const auto snapshot = m_store->GetShared();
        const ChannelSnapshot& channel = ChannelOf(*snapshot);
        std::wstring text = L"RTT: " + TooltipMetric(channel.avgRttMs) + L" ms | JIT: " +
                             TooltipMetric(channel.jitterMs) + L" ms | ";
        text += TooltipState(channel.state);
        if (!channel.activeTarget.empty())
            text += L" | " + channel.activeTarget;
        return text;
    }
    catch (...)
    {
        return {};
    }
}

const wchar_t* CNetworkStatusItem::GetItemName() const
{
    return (m_kind == ChannelKind::Tcp) ? L"PluginPing TCP internal" : L"PluginPing UDP internal";
}

const wchar_t* CNetworkStatusItem::GetItemId() const
{
    // Stable ASCII IDs. Never change these: the host keys the user's display
    // and ordering settings off them.
    return (m_kind == ChannelKind::Tcp) ? L"PluginPingTcpInternal" : L"PluginPingUdpInternal";
}

const wchar_t* CNetworkStatusItem::GetItemLableText() const
{
    return L"";
}

// TrafficMonitor ignores all three text getters for custom-draw items. Keep
// their host-visible behaviour identical to the proven PluginPing contract:
// return a stable, non-null empty string and expose data only through DrawItem.
const wchar_t* CNetworkStatusItem::GetItemValueText() const
{
    return L"";
}

const wchar_t* CNetworkStatusItem::GetItemValueSampleText() const
{
    return L"";
}

bool CNetworkStatusItem::IsCustomDraw() const
{
    return true;
}

// These run on the host's UI thread and allocate or take locks, so each seals
// the ABI boundary with a catch-all that degrades to a safe value. A C++
// exception unwinding into TrafficMonitor would terminate the host process.
int CNetworkStatusItem::GetItemWidth() const
{
    try
    {
        // 96-DPI fallback; the host scales it when GetItemWidthEx returns 0.
        if (m_config)
            return m_config->GetShared()->width;
    }
    catch (...)
    {
    }
    return 110;
}

int CNetworkStatusItem::GetItemWidthEx(void* hDC) const
{
    try
    {
        if (!hDC || !m_store || !m_config)
            return 0;
        MarkDisplayed();

        HDC dc = static_cast<HDC>(hDC);
        const auto snapshot = m_store->GetShared();
        const auto cfg = m_config->GetShared();

        WidthCacheKey key;
        key.dpi = GetDeviceCaps(dc, LOGPIXELSX);
        if (key.dpi <= 0)
            key.dpi = 96;
        key.compactMode = cfg->compactMode;
        key.embeddedMode = m_embeddedMode;
        key.tcpAvgRttMs = snapshot->tcp.avgRttMs;
        key.tcpJitterMs = snapshot->tcp.jitterMs;
        key.udpAvgRttMs = snapshot->udp.avgRttMs;
        key.udpJitterMs = snapshot->udp.jitterMs;
        key.hostFont = pluginping_shared::CaptureHostFont(dc).signature;

        {
            std::lock_guard<std::mutex> lk(m_widthMutex);
            if (m_hasWidthKey && m_widthCacheValue > 0 && key == m_widthKey)
                return m_widthCacheValue;
        }

        const int measured = MeasureChannelWidth(dc, m_kind, *snapshot, *cfg,
                                                 !m_embeddedMode);
        if (measured <= 0)
            return 0;   // transient font failure: do not cache, host uses GetItemWidth()

        // cfg->width is declared at 96 DPI while `measured` is device pixels,
        // and the host does not scale a GetItemWidthEx result, so the floor has
        // to be scaled before the comparison.
        const int minimum = MulDiv(cfg->width, key.dpi, 96);
        const int width = (std::max)(measured, minimum);
        {
            std::lock_guard<std::mutex> lk(m_widthMutex);
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

int CNetworkStatusItem::GetChannelWidthEx(void* hDC) const
{
    try
    {
        if (!hDC || !m_store || !m_config)
            return 0;
        MarkDisplayed();
        const auto snapshot = m_store->GetShared();
        const auto cfg = m_config->GetShared();
        // Embedded rows share one probe column width. Measuring only the
        // current channel would make DIRECT/TCP and PROXY/UDP drift by a few
        // pixels as their placeholders change independently.
        return MeasureChannelWidth(static_cast<HDC>(hDC), m_kind, *snapshot, *cfg,
                                   !m_embeddedMode, false);
    }
    catch (...)
    {
        return 0;
    }
}

void CNetworkStatusItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    try
    {
        if (!hDC || !m_store || !m_config)
            return;
        MarkDisplayed();
        // Draws from the published snapshot only; no network or disk work here.
        const auto snapshot = m_store->GetShared();
        const auto cfg = m_config->GetShared();
        DrawChannel(static_cast<HDC>(hDC), x, y, w, h, dark_mode,
                    m_kind, *snapshot, *cfg, !m_embeddedMode);
    }
    catch (...)
    {
        // Skip this frame rather than unwinding into the host's paint handler.
    }
}

int CNetworkStatusItem::OnMouseEvent(MouseEventType /*type*/, int /*x*/, int /*y*/,
                                     void* /*hWnd*/, int /*flag*/)
{
    // Do not consume events; TrafficMonitor keeps its default menu behaviour.
    return 0;
}

int CNetworkStatusItem::IsDrawResourceUsageGraph() const
{
    try
    {
        if (m_config && m_config->GetShared()->showResourceGraph)
            return 1;
    }
    catch (...)
    {
    }
    return 0;
}

float CNetworkStatusItem::GetResourceUsageGraphValue() const
{
    try
    {
        if (!m_store)
            return 0.0f;
        const auto snapshot = m_store->GetShared();
        const int stability = ChannelOf(*snapshot).stabilityPct;
        if (stability < 0)
            return 0.0f;
        return std::clamp(static_cast<float>(stability) / 100.0f, 0.0f, 1.0f);
    }
    catch (...)
    {
        return 0.0f;
    }
}

} // namespace pluginnetwork
