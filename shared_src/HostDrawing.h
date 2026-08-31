// PluginPing - host-owned drawing state shared by both composite sections.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>

namespace pluginping_shared {

struct HostFontSignature
{
    LOGFONTW value{};
    bool valid = false;
};

struct HostFont
{
    HFONT handle = nullptr;
    HostFontSignature signature;
    int pixelHeight = 12;
};

inline HostFont CaptureHostFont(HDC dc) noexcept
{
    HostFont result;
    if (!dc)
        return result;

    HGDIOBJ object = GetCurrentObject(dc, OBJ_FONT);
    if (!object)
        return result;

    LOGFONTW descriptor{};
    if (GetObjectW(object, sizeof(descriptor), &descriptor) !=
        static_cast<int>(sizeof(descriptor)))
        return result;

    result.handle = reinterpret_cast<HFONT>(object);
    result.signature.value = descriptor;
    result.signature.valid = true;
    result.pixelHeight = descriptor.lfHeight < 0 ? -descriptor.lfHeight : descriptor.lfHeight;
    if (result.pixelHeight <= 0)
    {
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics))
            result.pixelHeight = metrics.tmHeight;
    }
    if (result.pixelHeight <= 0)
        result.pixelHeight = 12;
    return result;
}

inline bool SameHostFont(const HostFontSignature& left,
                         const HostFontSignature& right) noexcept
{
    if (left.valid != right.valid)
        return false;
    if (!left.valid)
        return true;

    const LOGFONTW& a = left.value;
    const LOGFONTW& b = right.value;
    return a.lfHeight == b.lfHeight &&
           a.lfWidth == b.lfWidth &&
           a.lfEscapement == b.lfEscapement &&
           a.lfOrientation == b.lfOrientation &&
           a.lfWeight == b.lfWeight &&
           a.lfItalic == b.lfItalic &&
           a.lfUnderline == b.lfUnderline &&
           a.lfStrikeOut == b.lfStrikeOut &&
           a.lfCharSet == b.lfCharSet &&
           a.lfOutPrecision == b.lfOutPrecision &&
           a.lfClipPrecision == b.lfClipPrecision &&
           a.lfQuality == b.lfQuality &&
           a.lfPitchAndFamily == b.lfPitchAndFamily &&
           std::wcscmp(a.lfFaceName, b.lfFaceName) == 0;
}

inline COLORREF HostTextColor(HDC dc, COLORREF fallback) noexcept
{
    if (!dc)
        return fallback;
    const COLORREF color = GetTextColor(dc);
    return color == CLR_INVALID ? fallback : color;
}

} // namespace pluginping_shared
