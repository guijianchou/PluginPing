// PluginPing - merged TrafficMonitor host adapter.
#include "../network_src/src/Common.h"
#include "PluginPingMerge.h"
#include "ChildPluginAccess.h"

#include <string>

namespace pluginpingmerge {

namespace {

pluginping_shared::ICompositeSection* AsCompositeSection(IPluginItem* item)
{
    return dynamic_cast<pluginping_shared::ICompositeSection*>(item);
}

} // namespace

CPluginPingMerge& CPluginPingMerge::Instance()
{
    static CPluginPingMerge instance;
    return instance;
}

CPluginPingMerge::CPluginPingMerge()
    : m_ping(pluginping::GetPluginInstanceInternal()),
      m_network(pluginnetwork::GetPluginInstanceInternal()),
      m_directItem(CompositeRoute::Direct,
                   AsCompositeSection(m_ping ? m_ping->GetItem(0) : nullptr),
                   AsCompositeSection(m_network ? m_network->GetItem(0) : nullptr)),
      m_proxyItem(CompositeRoute::Proxy,
                  AsCompositeSection(m_ping ? m_ping->GetItem(1) : nullptr),
                  AsCompositeSection(m_network ? m_network->GetItem(1) : nullptr))
{
}

void CPluginPingMerge::Shutdown()
{
    // Stop the IP worker before the network workers so no child is still
    // publishing while the host is releasing the merged plugin.
    if (m_ping)
        pluginping::ShutdownPluginInternal();
    if (m_network)
        pluginnetwork::ShutdownPluginInternal();
}

IPluginItem* CPluginPingMerge::GetItem(int index)
{
    // Each host item is one complete route row: DIRECT owns the Direct IP and
    // TCP probe, while PROXY owns the Proxy IP and UDP probe.
    if (!IsReady())
        return nullptr;
    switch (index)
    {
    case 0: return &m_directItem;
    case 1: return &m_proxyItem;
    default: return nullptr;
    }
}

void CPluginPingMerge::DataRequired()
{
    try
    {
        if (m_ping)
            m_ping->DataRequired();
        if (m_network)
            m_network->DataRequired();
    }
    catch (...)
    {
        // The child adapters already seal their own ABI boundaries. Keep the
        // merged entry point equally strict for a host callback.
    }
}

const wchar_t* CPluginPingMerge::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:        return L"PluginPing";
    case TMI_DESCRIPTION: return L"DIRECT IP + TCP and PROXY IP + UDP route status.";
    case TMI_AUTHOR:      return L"PluginPing contributors";
    case TMI_COPYRIGHT:   return L"Copyright (C) 2026 PluginPing contributors";
    case TMI_VERSION:     return L"1.0.14";
    case TMI_URL:         return L"";
    default:              return L"";
    }
}

const wchar_t* CPluginPingMerge::GetTooltipInfo()
{
    thread_local std::wstring copy;
    try
    {
        copy.clear();
        const auto appendRoute = [](std::wstring& output, const CCompositeStatusItem& item)
        {
            const std::wstring text = item.CompositeTooltip();
            if (text.empty())
                return;
            if (!output.empty())
                output += L"\r\n";
            output += text;
        };
        appendRoute(copy, m_directItem);
        appendRoute(copy, m_proxyItem);
        return copy.c_str();
    }
    catch (...)
    {
        return L"";
    }
}

ITMPlugin::OptionReturn CPluginPingMerge::ShowOptionsDialog(void* hParent)
{
    try
    {
        // The merged plugin deliberately uses one plain INI surface. Opening it
        // through ShellExecute avoids loading CLR/WPF into the host process.
        return m_network ? m_network->ShowOptionsDialog(hParent) : OR_OPTION_NOT_PROVIDED;
    }
    catch (...)
    {
        return OR_OPTION_NOT_PROVIDED;
    }
}

void CPluginPingMerge::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    try
    {
        if (m_ping)
            m_ping->OnExtenedInfo(index, data);
        if (m_network)
            m_network->OnExtenedInfo(index, data);
    }
    catch (...)
    {
    }
}

} // namespace pluginpingmerge

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    try
    {
        auto& instance = pluginpingmerge::CPluginPingMerge::Instance();
        return instance.IsReady() ? &instance : nullptr;
    }
    catch (...)
    {
        return nullptr;
    }
}

extern "C" __declspec(dllexport) void TMPluginShutdown()
{
    try
    {
        pluginpingmerge::CPluginPingMerge::Instance().Shutdown();
    }
    catch (...)
    {
    }
}
