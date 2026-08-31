// pluginNetwork - theme palette and colour parsing
#pragma once
#include "Common.h"
#include <string>

namespace pluginnetwork {

struct ThemePalette
{
    COLORREF text;   // normal text
    COLORREF muted;  // placeholders while warming up / unsupported
    COLORREF good;   // stability >= GreenThreshold
    COLORREF warn;   // stability >= YellowThreshold
    COLORREF bad;    // stability below YellowThreshold, or disconnected
};

// Light mode anchors on the Material values from the specification
// (#00C853 / #FFD600-derived / #D50000). Dark mode lifts them so they stay
// readable on a dark taskbar. All three accents can be overridden per theme
// from [Color_Rules] in the ini.
inline ThemePalette GetPalette(bool dark)
{
    if (dark)
    {
        return ThemePalette{
            RGB(235, 238, 242),
            RGB(150, 158, 168),
            RGB(89, 214, 120),
            RGB(245, 196, 64),
            RGB(255, 86, 86)
        };
    }

    return ThemePalette{
        RGB(31, 45, 61),
        RGB(110, 120, 133),
        RGB(0, 200, 83),
        RGB(202, 138, 4),
        RGB(213, 0, 0)
    };
}

// Accepts "#RRGGBB", "RRGGBB" and "R,G,B". Returns false and leaves `out`
// untouched for anything else, so a typo in the ini falls back to the theme
// default instead of painting the row black.
inline bool ParseHexColor(const std::wstring& value, COLORREF& out)
{
    std::wstring s;
    for (wchar_t c : value)
    {
        if (c != L' ' && c != L'\t')
            s.push_back(c);
    }
    if (s.empty())
        return false;

    if (s.find(L',') != std::wstring::npos)
    {
        int channel[3] = { -1, -1, -1 };
        size_t start = 0;
        for (int i = 0; i < 3; ++i)
        {
            const size_t comma = s.find(L',', start);
            const std::wstring part = s.substr(start, comma == std::wstring::npos
                                                          ? std::wstring::npos
                                                          : comma - start);
            if (part.empty())
                return false;
            for (wchar_t c : part)
            {
                if (c < L'0' || c > L'9')
                    return false;
            }
            channel[i] = _wtoi(part.c_str());
            if (channel[i] < 0 || channel[i] > 255)
                return false;
            if (comma == std::wstring::npos)
            {
                if (i != 2)
                    return false;
                start = s.size();
                break;
            }
            start = comma + 1;
        }
        out = RGB(channel[0], channel[1], channel[2]);
        return true;
    }

    if (s.front() == L'#')
        s.erase(s.begin());
    if (s.size() != 6)
        return false;

    int channel[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; ++i)
    {
        int value8 = 0;
        for (int j = 0; j < 2; ++j)
        {
            const wchar_t c = s[static_cast<size_t>(i) * 2 + j];
            int digit = 0;
            if (c >= L'0' && c <= L'9')
                digit = c - L'0';
            else if (c >= L'a' && c <= L'f')
                digit = c - L'a' + 10;
            else if (c >= L'A' && c <= L'F')
                digit = c - L'A' + 10;
            else
                return false;
            value8 = value8 * 16 + digit;
        }
        channel[i] = value8;
    }
    out = RGB(channel[0], channel[1], channel[2]);
    return true;
}

} // namespace pluginnetwork
