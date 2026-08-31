// PluginPing - one TrafficMonitor plugin containing both probe suites.
#pragma once

#include "CompositeStatusItem.h"
#include "../ping_src/include/PluginInterface.h"

namespace pluginpingmerge {

class CPluginPingMerge final : public ITMPlugin
{
public:
    static CPluginPingMerge& Instance();
    bool IsReady() const
    {
        return m_ping != nullptr && m_network != nullptr &&
               m_directItem.IsReady() && m_proxyItem.IsReady();
    }
    void Shutdown();

    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    OptionReturn ShowOptionsDialog(void* hParent) override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;

private:
    CPluginPingMerge();
    CPluginPingMerge(const CPluginPingMerge&) = delete;
    CPluginPingMerge& operator=(const CPluginPingMerge&) = delete;

    ITMPlugin* m_ping = nullptr;
    ITMPlugin* m_network = nullptr;
    CCompositeStatusItem m_directItem;
    CCompositeStatusItem m_proxyItem;
};

} // namespace pluginpingmerge

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance();
extern "C" __declspec(dllexport) void TMPluginShutdown();
