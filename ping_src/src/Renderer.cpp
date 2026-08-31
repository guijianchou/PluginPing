// PluginPing - DIRECT/PROXY IP renderer.
#include "Common.h"
#include "Renderer.h"
#include "../../shared_src/HostDrawing.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace pluginping {

namespace {

int TextWidth(HDC dc, const std::wstring& text)
{
    if (text.empty())
        return 0;
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

struct Metrics
{
    int fontPx = 12;
    int gap = 6;
    int leftPad = 5;
};

Metrics ComputeMetrics(HDC dc, int fontPx)
{
    Metrics m;
    m.fontPx = fontPx;
    m.gap = (std::max)(1, TextWidth(dc, L" "));
    m.leftPad = (std::max)(2, static_cast<int>(std::lround(fontPx * 0.4)));
    return m;
}

bool LabelHasColon(const std::wstring& label)
{
    if (label.empty())
        return false;
    const wchar_t last = label.back();
    return last == L':' || last == L'\uFF1A';
}

std::wstring DisplayLabel(const EndpointStatus& endpoint, bool compact)
{
    if (endpoint.label.empty() || compact || LabelHasColon(endpoint.label))
        return endpoint.label;
    return endpoint.label + L":";
}

struct ColumnLayout
{
    int label = 0;
    int country = 0;
    int ip = 0;
    int total = 0;
};

ColumnLayout ComputeColumns(HDC dc, HFONT font, HFONT ipFont, const Metrics& metrics,
                            const LocalRemoteSnapshot& snapshot, bool compact)
{
    HGDIOBJ previous = SelectObject(dc, font);
    ColumnLayout layout;
    const EndpointStatus& first = snapshot.local;
    const EndpointStatus& second = snapshot.remote;
    const int labelWidth = (std::max)(TextWidth(dc, DisplayLabel(first, compact)),
                                      TextWidth(dc, DisplayLabel(second, compact)));
    const int countryWidth = (std::max)(TextWidth(dc, first.countryCode),
                                        TextWidth(dc, second.countryCode));
    SelectObject(dc, ipFont);
    const std::wstring firstIp = first.ipAddress.empty()
        ? (first.displayAsnOnly ? L"" : L"--") : first.ipAddress;
    const std::wstring secondIp = second.ipAddress.empty()
        ? (second.displayAsnOnly ? L"" : L"--") : second.ipAddress;
    const int ipWidth = (std::max)(TextWidth(dc, firstIp), TextWidth(dc, secondIp));
    SelectObject(dc, previous);

    layout.label = labelWidth + metrics.gap;
    layout.country = countryWidth > 0 ? countryWidth + metrics.gap : 0;
    layout.ip = ipWidth;
    layout.total = metrics.leftPad + layout.label + layout.country + layout.ip;
    return layout;
}

void DrawTextLeft(HDC dc, const std::wstring& text, int left, int top, int right, int bottom,
                  COLORREF color)
{
    if (text.empty() || right <= left)
        return;
    SetTextColor(dc, color);
    RECT rect{left, top, right, bottom};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void DrawRow(HDC dc, const EndpointStatus& endpoint, int rowTop, int rowBottom,
             const ColumnLayout& layout, const Metrics& metrics, int baseX,
             COLORREF textColor, HFONT font, HFONT ipFont, bool compact)
{
    int cursor = baseX + metrics.leftPad;
    SelectObject(dc, font);
    DrawTextLeft(dc, DisplayLabel(endpoint, compact), cursor, rowTop,
                 cursor + layout.label - metrics.gap,
                 rowBottom, textColor);
    cursor += layout.label;
    if (layout.country > 0)
    {
        DrawTextLeft(dc, endpoint.countryCode, cursor, rowTop,
                     cursor + layout.country - metrics.gap, rowBottom, textColor);
        cursor += layout.country;
    }
    if (layout.ip > 0 && !endpoint.displayAsnOnly)
    {
        SelectObject(dc, ipFont);
        DrawTextLeft(dc, endpoint.ipAddress.empty() ? L"--" : endpoint.ipAddress,
                     cursor, rowTop, cursor + layout.ip, rowBottom, textColor);
    }
}

} // namespace

int MeasureSnapshotWidth(HDC hDC, const LocalRemoteSnapshot& snapshot, const PluginConfig& cfg)
{
    if (!hDC)
        return 0;
    const pluginping_shared::HostFont hostFont = pluginping_shared::CaptureHostFont(hDC);
    if (!hostFont.handle)
        return 0;
    const Metrics metrics = ComputeMetrics(hDC, hostFont.pixelHeight);
    return ComputeColumns(hDC, hostFont.handle, hostFont.handle, metrics, snapshot,
                          cfg.compactMode).total;
}

void DrawStatusRow(HDC hDC, int x, int y, int w, int h, bool darkMode,
                  const LocalRemoteSnapshot& snapshot, bool isLocal, const PluginConfig& cfg)
{
    if (!hDC || w <= 0 || h <= 0)
        return;
    const pluginping_shared::HostFont hostFont = pluginping_shared::CaptureHostFont(hDC);
    if (!hostFont.handle)
        return;
    const Metrics metrics = ComputeMetrics(hDC, hostFont.pixelHeight);

    const ColumnLayout layout = ComputeColumns(hDC, hostFont.handle, hostFont.handle,
                                                metrics, snapshot, cfg.compactMode);
    const EndpointStatus& endpoint = isLocal ? snapshot.local : snapshot.remote;
    const COLORREF fallbackTextColor = darkMode ? RGB(235, 238, 242)
                                                : RGB(31, 45, 61);
    const COLORREF textColor =
        pluginping_shared::HostTextColor(hDC, fallbackTextColor);
    const int saved = SaveDC(hDC);
    if (saved == 0)
        return;
    IntersectClipRect(hDC, x, y, x + w, y + h);
    SetBkMode(hDC, TRANSPARENT);
    DrawRow(hDC, endpoint, y, y + h, layout, metrics, x, textColor, hostFont.handle,
            hostFont.handle, cfg.compactMode);
    RestoreDC(hDC, saved);
}

} // namespace pluginping
