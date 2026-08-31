// pluginNetwork - custom renderer implementation
#include "Common.h"
#include "Renderer.h"
#include "Theme.h"
#include "../../shared_src/HostDrawing.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace pluginnetwork {

namespace {

struct Metrics
{
    int fontPx = 12;
    int leftPad = 5;
    int rightPad = 5;
};

Metrics ComputeMetrics(int fontPx)
{
    Metrics m;
    m.fontPx = fontPx;
    m.leftPad = (std::max)(2, static_cast<int>(std::lround(fontPx * 0.4)));
    m.rightPad = m.leftPad;
    return m;
}

int TextWidth(HDC dc, const std::wstring& text)
{
    if (text.empty())
        return 0;
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

// ---- text pieces -----------------------------------------------------------

std::wstring LabelText(ChannelKind kind, bool compact)
{
    if (kind == ChannelKind::Tcp)
        return compact ? L"TCP" : L"TCP:";
    return compact ? L"UDP" : L"UDP:";
}

std::wstring LatencyValueText(const ChannelSnapshot& channel)
{
    if (channel.avgRttMs < 0)
        return L"--";
    return std::to_wstring(channel.avgRttMs);
}

std::wstring JitterValueText(const ChannelSnapshot& channel)
{
    if (channel.jitterMs < 0)
        return L"--";
    return std::to_wstring(channel.jitterMs);
}

// Non-null only for the states that replace the whole measurement block.
const wchar_t* StateWord(ChannelState state)
{
    switch (state)
    {
    case ChannelState::Disabled:     return L"off";
    case ChannelState::Unsupported:  return L"N/A";
    case ChannelState::Disconnected: return L"DOWN";
    default:                         return nullptr;
    }
}

COLORREF ResolveToneColor(QualityTone tone, const ThemePalette& palette, const PluginConfig& cfg)
{
    COLORREF color = palette.text;
    const std::wstring* overrideText = nullptr;
    switch (tone)
    {
    case QualityTone::Good:
        color = palette.good;
        overrideText = &cfg.greenColor;
        break;
    case QualityTone::Warn:
        color = palette.warn;
        overrideText = &cfg.yellowColor;
        break;
    case QualityTone::Bad:
        color = palette.bad;
        overrideText = &cfg.redColor;
        break;
    default:
        return palette.muted;
    }

    COLORREF parsed = 0;
    if (overrideText && !overrideText->empty() && ParseHexColor(*overrideText, parsed))
        return parsed;
    return color;
}

// ---- layout ----------------------------------------------------------------

struct ColumnLayout
{
    int gap = 0;           // width of one space in the configured font
    int fieldGap = 0;      // separation between native-style metric cells
    int channelLabel = 0;
    int rttLabel = 0;
    int rttCell = 0;
    int jitterLabel = 0;
    int jitterValue = 0;
    int unit = 0;
    int total = 0;
};

ColumnLayout ComputeColumns(HDC dc, HFONT font, HFONT valueFont, const Metrics& m,
                            const MetricsSnapshot& snapshot, const PluginConfig& cfg,
                            bool showLabel, ChannelKind kind, bool currentOnly)
{
    ColumnLayout layout;
    HGDIOBJ previous = SelectObject(dc, font);

    if (showLabel)
    {
        layout.channelLabel = (std::max)(
            TextWidth(dc, LabelText(ChannelKind::Tcp, cfg.compactMode)),
            TextWidth(dc, LabelText(ChannelKind::Udp, cfg.compactMode)));
    }
    layout.gap = (std::max)(1, TextWidth(dc, L" "));
    layout.fieldGap = layout.gap * 2;
    layout.rttLabel = TextWidth(dc, L"RTT:");
    layout.jitterLabel = TextWidth(dc, L"JIT:");
    layout.unit = cfg.compactMode ? 0 : TextWidth(dc, L"ms");

    SelectObject(dc, valueFont);
    const ChannelSnapshot& current = kind == ChannelKind::Tcp ? snapshot.tcp : snapshot.udp;
    const int measuredLatency = currentOnly
        ? TextWidth(dc, LatencyValueText(current))
        : (std::max)(TextWidth(dc, LatencyValueText(snapshot.tcp)),
                     TextWidth(dc, LatencyValueText(snapshot.udp)));
    const int measuredJitter = currentOnly
        ? TextWidth(dc, JitterValueText(current))
        : (std::max)(TextWidth(dc, JitterValueText(snapshot.tcp)),
                     TextWidth(dc, JitterValueText(snapshot.udp)));

    // RTT behaves like a native TrafficMonitor value: "58 ms" is one cell, so
    // the unit follows the actual number. Reserving the 999-ms cell keeps the
    // following JIT cell aligned between DIRECT and PROXY.
    const int measuredRttCell = measuredLatency +
        (cfg.compactMode ? 0 : layout.gap + layout.unit);
    const int baselineRttCell = TextWidth(dc, cfg.compactMode ? L"999" : L"999 ms");
    const int baselineJitter = TextWidth(dc, L"999");
    layout.rttCell = (std::max)(measuredRttCell, baselineRttCell);
    layout.jitterValue = (std::max)(measuredJitter, baselineJitter);

    SelectObject(dc, previous);

    layout.total = m.leftPad + layout.channelLabel + (showLabel ? layout.gap : 0) +
                   layout.rttLabel + layout.gap + layout.rttCell + layout.fieldGap +
                   layout.jitterLabel + layout.gap + layout.jitterValue + m.rightPad;
    return layout;
}

void DrawTextIn(HDC dc, const std::wstring& text, int left, int top, int right, int bottom,
                COLORREF color, UINT alignment)
{
    if (text.empty() || right <= left)
        return;

    SetTextColor(dc, color);
    RECT rect{ left, top, right, bottom };
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
                alignment | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

} // namespace

int MeasureChannelWidth(HDC hDC, ChannelKind kind, const MetricsSnapshot& snapshot,
                        const PluginConfig& cfg, bool showLabel, bool currentOnly)
{
    if (!hDC)
        return 0;

    const pluginping_shared::HostFont hostFont = pluginping_shared::CaptureHostFont(hDC);
    if (!hostFont.handle)
        return 0;
    Metrics metrics = ComputeMetrics(hostFont.pixelHeight);
    if (!showLabel)
    {
        // The enclosing IP section already owns the row's outer padding.
        metrics.leftPad = 0;
        metrics.rightPad = 0;
    }
    return ComputeColumns(hDC, hostFont.handle, hostFont.handle, metrics, snapshot, cfg,
                          showLabel, kind, currentOnly).total;
}

void DrawChannel(HDC hDC, int x, int y, int w, int h, bool darkMode,
                 ChannelKind kind, const MetricsSnapshot& snapshot, const PluginConfig& cfg,
                 bool showLabel)
{
    if (!hDC || w <= 0 || h <= 0)
        return;

    const pluginping_shared::HostFont hostFont = pluginping_shared::CaptureHostFont(hDC);
    if (!hostFont.handle)
        return;
    Metrics metrics = ComputeMetrics(hostFont.pixelHeight);
    if (!showLabel)
    {
        metrics.leftPad = 0;
        metrics.rightPad = 0;
    }
    const ChannelSnapshot& channel = kind == ChannelKind::Tcp ? snapshot.tcp : snapshot.udp;
    ThemePalette palette = GetPalette(darkMode);
    palette.text = pluginping_shared::HostTextColor(hDC, palette.text);
    const QualityTone rttTone = ToneForLatencyValue(channel.avgRttMs);
    const QualityTone jitterTone = ToneForLatencyValue(channel.jitterMs);
    const COLORREF rttColor = ResolveToneColor(rttTone, palette, cfg);
    const COLORREF jitterColor = ResolveToneColor(jitterTone, palette, cfg);

    // SaveDC/RestoreDC hands the host back every attribute this row touches
    // (font, background mode, text colour, clip region), and the clip stops a
    // layout that grew since the last width measurement from painting over the
    // neighbouring taskbar item.
    const int savedDc = SaveDC(hDC);
    if (savedDc == 0)
        return;
    IntersectClipRect(hDC, x, y, x + w, y + h);
    SetBkMode(hDC, TRANSPARENT);
    SelectObject(hDC, hostFont.handle);

    const ColumnLayout layout = ComputeColumns(hDC, hostFont.handle, hostFont.handle, metrics,
                                                snapshot, cfg, showLabel, kind, false);
    const int top = y;
    const int bottom = y + h;
    int cursor = x + metrics.leftPad;

    if (const wchar_t* word = StateWord(channel.state))
    {
        if (showLabel)
        {
            const std::wstring label = LabelText(kind, cfg.compactMode);
            DrawTextIn(hDC, label, cursor, top, cursor + layout.channelLabel, bottom,
                       palette.text, DT_LEFT);
            cursor += layout.channelLabel + layout.gap;
        }

        DrawTextIn(hDC, L"RTT:", cursor, top, cursor + layout.rttLabel, bottom,
                   palette.text, DT_LEFT);
        cursor += layout.rttLabel + layout.gap;

        const COLORREF wordColor = (channel.state == ChannelState::Disconnected)
                                       ? ResolveToneColor(QualityTone::Bad, palette, cfg)
                                       : palette.muted;
        DrawTextIn(hDC, word, cursor, top, x + w - metrics.rightPad, bottom,
                   wordColor, DT_LEFT);
        RestoreDC(hDC, savedDc);
        return;
    }

    if (showLabel)
    {
        const std::wstring label = LabelText(kind, cfg.compactMode);
        DrawTextIn(hDC, label, cursor, top, cursor + layout.channelLabel, bottom,
                   palette.text, DT_LEFT);
        cursor += layout.channelLabel + layout.gap;
    }

    DrawTextIn(hDC, L"RTT:", cursor, top, cursor + layout.rttLabel, bottom,
               palette.text, DT_LEFT);
    cursor += layout.rttLabel + layout.gap;

    const int rttCellLeft = cursor;
    const std::wstring latency = LatencyValueText(channel);
    const int latencyWidth = TextWidth(hDC, latency);
    DrawTextIn(hDC, latency, cursor, top, cursor + latencyWidth, bottom,
               rttColor, DT_LEFT);
    cursor += latencyWidth;

    if (!cfg.compactMode)
    {
        cursor += layout.gap;
        DrawTextIn(hDC, L"ms", cursor, top, cursor + layout.unit, bottom,
                   palette.text, DT_LEFT);
        cursor += layout.unit;
    }

    cursor = rttCellLeft + layout.rttCell + layout.fieldGap;
    DrawTextIn(hDC, L"JIT:", cursor, top, cursor + layout.jitterLabel, bottom,
               palette.text, DT_LEFT);
    cursor += layout.jitterLabel + layout.gap;

    const std::wstring jitter = JitterValueText(channel);
    DrawTextIn(hDC, jitter, cursor, top, cursor + layout.jitterValue, bottom,
               jitterColor, DT_LEFT);
    cursor += layout.jitterValue;
    RestoreDC(hDC, savedDc);
}

} // namespace pluginnetwork
