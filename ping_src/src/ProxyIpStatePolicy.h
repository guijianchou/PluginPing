// PluginPing - pure policy for retaining a confirmed PROXY identity.
#pragma once

#include <string>

namespace pluginping {

enum class ProxyFailureKind
{
    Transient,
    RejectedRoute
};

inline void ApplyProxyIdentityFailure(std::wstring& ip, std::wstring& country,
                                      bool& displayAsnOnly, ProxyFailureKind kind)
{
    if (kind == ProxyFailureKind::Transient && (!ip.empty() || displayAsnOnly))
        return;

    ip.clear();
    country = L"--";
    displayAsnOnly = false;
}

} // namespace pluginping
