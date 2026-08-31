// PluginPing - immutable IP display data.
#pragma once

#include <string>

namespace pluginping {

struct EndpointStatus
{
    std::wstring label;
    std::wstring countryCode;
    std::wstring ipAddress;
    // A valid PROXY loc=CN result is rendered as one N/A token for the
    // combined ASN/IP group.
    bool displayAsnOnly = false;
    std::wstring ipDiagnostic;
};

struct LocalRemoteSnapshot
{
    EndpointStatus local;
    EndpointStatus remote;
};

} // namespace pluginping
