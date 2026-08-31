// PluginPing - strict Cloudflare trace body parser.
#pragma once

#include <string>

namespace pluginping {

struct CloudflareTraceFields
{
    std::wstring ip;
    std::wstring loc;
};

// Returns false when the body has no valid IP. loc is optional; malformed or
// non-ISO loc values are deliberately normalized to empty.
bool ParseCloudflareTraceBody(const std::string& body, CloudflareTraceFields& fields);

} // namespace pluginping
