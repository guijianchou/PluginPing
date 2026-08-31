// Exercises PluginPing through the TrafficMonitor ABI without linking to it.
#include "../ping_src/include/PluginInterface.h"

#include <crtdbg.h>
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <string>

namespace {

class MockTrafficMonitor final : public ITrafficMonitor
{
public:
    int GetAPIVersion() override { return 0; }
    const wchar_t* GetVersion() override { return L"probe"; }
    double GetMonitorValue(MonitorItem) override { return 0.0; }
    const wchar_t* GetMonitorValueString(MonitorItem, int) override { return L"0"; }
    void ShowNotifyMessage(const wchar_t*) override {}
    unsigned short GetLanguageId() const override
    {
        return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
    }
    const wchar_t* GetPluginConfigDir() const override { return L""; }
    int GetDPI(DPIType) const override { return 96; }
    unsigned int GetThemeColor() const override { return RGB(0, 120, 215); }
    const wchar_t* GetStringRes(const wchar_t*, const wchar_t*) override { return L""; }
};

using GetInstanceProc = ITMPlugin* (*)();
using ShutdownProc = void (*)();

[[noreturn]] void Fail(const wchar_t* message)
{
    std::wcerr << L"FAIL: " << message << L" (error " << GetLastError() << L")\n";
    ExitProcess(1);
}

void InvalidParameter(const wchar_t* expression, const wchar_t* function,
                      const wchar_t* file, unsigned int line, uintptr_t)
{
    std::wcerr << L"INVALID PARAMETER\n  expression: " << (expression ? expression : L"--")
               << L"\n  function: " << (function ? function : L"--")
               << L"\n  file: " << (file ? file : L"--") << L":" << line << L"\n";
    ExitProcess(3);
}

void RequireText(const wchar_t* name, const wchar_t* value)
{
    if (!value)
        Fail(name);
    volatile size_t length = wcslen(value);
    (void)length;
    std::wstring copy(value);
}

int DrawItem(IPluginItem* item, int iteration, bool dark)
{
    HDC screen = GetDC(nullptr);
    if (!screen)
        Fail(L"GetDC");
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 640, 64);
    if (!dc || !bitmap)
        Fail(L"CreateCompatibleDC/CreateCompatibleBitmap");
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    const int oldBkMode = GetBkMode(dc);
    const COLORREF oldTextColor = GetTextColor(dc);
    HGDIOBJ oldFont = GetCurrentObject(dc, OBJ_FONT);

    const int width = item->GetItemWidthEx(dc);
    if (width <= 0 || width > 4096)
        Fail(L"invalid item width");
    item->DrawItem(dc, 7, 5, width, 30 + (iteration % 3), dark);

    if (GetBkMode(dc) != oldBkMode || GetTextColor(dc) != oldTextColor ||
        GetCurrentObject(dc, OBJ_FONT) != oldFont)
        Fail(L"DrawItem leaked HDC state");

    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return width;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::wcerr << L"usage: probe_plugin_runtime.exe <PluginPing.dll> [seconds]\n";
        return 2;
    }

    _set_invalid_parameter_handler(&InvalidParameter);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

    const int seconds = argc >= 3 ? (std::max)(_wtoi(argv[2]), 1) : 30;
    HMODULE module = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
        Fail(L"LoadLibraryExW");

    auto getInstance = reinterpret_cast<GetInstanceProc>(
        GetProcAddress(module, "TMPluginGetInstance"));
    auto shutdown = reinterpret_cast<ShutdownProc>(
        GetProcAddress(module, "TMPluginShutdown"));
    if (!getInstance || !shutdown)
        Fail(L"required export missing");

    ITMPlugin* plugin = getInstance();
    if (!plugin || plugin->GetAPIVersion() != 7)
        Fail(L"invalid plugin instance/API version");

    MockTrafficMonitor host;
    plugin->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, L".\\plugins\\");
    plugin->OnInitialize(&host);

    // TrafficMonitor calls DataRequired and GetTooltipInfo even when no item is
    // enabled. This startup path must stay inert and return a stable empty string.
    plugin->DataRequired();
    const wchar_t* hiddenTooltip = plugin->GetTooltipInfo();
    if (!hiddenTooltip || hiddenTooltip[0] != L'\0')
        Fail(L"hidden plugin contributed tooltip text");

    IPluginItem* items[2] = {
        plugin->GetItem(0), plugin->GetItem(1)
    };
    if (!items[0] || !items[1] || plugin->GetItem(2))
        Fail(L"invalid item enumeration");

    const wchar_t* expectedIds[2] = {
        L"PluginPingLocal", L"PluginPingRemote"
    };
    const wchar_t* expectedNames[2] = {
        L"PluginPing DIRECT", L"PluginPing PROXY"
    };
    for (int i = 0; i < 2; ++i)
    {
        if (wcscmp(items[i]->GetItemId(), expectedIds[i]) != 0)
            Fail(L"invalid composite item ID/order");
        if (wcscmp(items[i]->GetItemName(), expectedNames[i]) != 0)
            Fail(L"invalid composite item name");
    }

    if (wcscmp(plugin->GetInfo(ITMPlugin::TMI_NAME), L"PluginPing") != 0)
        Fail(L"invalid plugin name");
    if (wcscmp(plugin->GetInfo(ITMPlugin::TMI_VERSION), L"1.0.14") != 0)
        Fail(L"invalid plugin version");

    for (int info = 0; info < ITMPlugin::TMI_MAX; ++info)
        RequireText(L"GetInfo", plugin->GetInfo(static_cast<ITMPlugin::PluginInfoIndex>(info)));

    // Activating DIRECT must activate its Direct-IP and TCP children only.
    // Give the workers two sampling ticks and reject any hidden PROXY/UDP text.
    DrawItem(items[0], 0, false);
    plugin->DataRequired();
    for (int check = 0; check < 20; ++check)
    {
        const wchar_t* directOnly = plugin->GetTooltipInfo();
        RequireText(L"GetTooltipInfo DIRECT-only", directOnly);
        if (wcsstr(directOnly, L"PROXY") || wcsstr(directOnly, L"UDP"))
            Fail(L"DIRECT row activated PROXY/UDP children");
        Sleep(100);
    }

    // Once both rows are active, their shared IP geometry must keep the RTT/JIT
    // columns on the same x axis.
    int directWidth = DrawItem(items[0], 1, false);
    int proxyWidth = DrawItem(items[1], 1, false);
    plugin->DataRequired();
    for (int attempt = 0; directWidth != proxyWidth && attempt < 20; ++attempt)
    {
        Sleep(100);
        directWidth = DrawItem(items[0], attempt + 2, false);
        proxyWidth = DrawItem(items[1], attempt + 2, false);
    }
    if (directWidth != proxyWidth)
    {
        std::wcerr << L"DIRECT width=" << directWidth << L", PROXY width=" << proxyWidth << L"\n";
        Fail(L"composite rows are not column-aligned");
    }

    for (int iteration = 0; iteration < seconds * 10; ++iteration)
    {
        if (iteration % 10 == 0)
            plugin->DataRequired();
        plugin->OnExtenedInfo(ITMPlugin::EI_DRAW_TASKBAR_WND,
                              (iteration & 1) ? L"1" : L"0");
        plugin->OnExtenedInfo(ITMPlugin::EI_LABEL_TEXT_COLOR, L"15790320");
        plugin->OnExtenedInfo(ITMPlugin::EI_VALUE_TEXT_COLOR, L"14737632");

        for (IPluginItem* item : items)
        {
            RequireText(L"GetItemName", item->GetItemName());
            RequireText(L"GetItemId", item->GetItemId());
            RequireText(L"GetItemLableText", item->GetItemLableText());
            RequireText(L"GetItemValueText", item->GetItemValueText());
            RequireText(L"GetItemValueSampleText", item->GetItemValueSampleText());
            DrawItem(item, iteration, (iteration & 1) != 0);
            const float graph = item->GetResourceUsageGraphValue();
            if (graph < 0.0f || graph > 1.0f)
                Fail(L"resource graph out of range");
        }
        RequireText(L"GetTooltipInfo", plugin->GetTooltipInfo());
        Sleep(100);
    }

    shutdown();
    shutdown();
    std::wcout << L"PluginPing runtime probe OK (" << seconds << L" s)\n";
    return 0;
}
