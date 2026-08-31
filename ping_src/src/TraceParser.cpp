// PluginPing - strict Cloudflare trace body parser.
#include "TraceParser.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace pluginping {

namespace {

bool ExtractField(const std::string& body, const char* field, std::string& value)
{
    if (!field || !*field)
        return false;
    const std::string prefix = std::string(field) + "=";
    size_t lineStart = 0;
    while (lineStart <= body.size())
    {
        const size_t newline = body.find('\n', lineStart);
        size_t lineEnd = newline == std::string::npos ? body.size() : newline;
        if (lineEnd > lineStart && body[lineEnd - 1] == '\r')
            --lineEnd;
        if (lineEnd - lineStart >= prefix.size() &&
            body.compare(lineStart, prefix.size(), prefix) == 0)
        {
            value.assign(body, lineStart + prefix.size(), lineEnd - lineStart - prefix.size());
            return true;
        }
        if (newline == std::string::npos)
            break;
        lineStart = newline + 1;
    }
    return false;
}

std::wstring Token(const std::string& value)
{
    size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t' ||
                                    value[first] == '"'))
        ++first;
    size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' ||
                            value[last - 1] == '"'))
        --last;
    std::wstring result;
    result.reserve(last - first);
    for (size_t i = first; i < last; ++i)
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(value[i])));
    return result;
}

bool ValidIp(const std::wstring& value)
{
    IN_ADDR ipv4{};
    if (InetPtonW(AF_INET, value.c_str(), &ipv4) == 1)
        return true;
    IN6_ADDR ipv6{};
    return InetPtonW(AF_INET6, value.c_str(), &ipv6) == 1;
}

std::wstring NormalizeLoc(const std::string& value)
{
    if (value.size() != 2)
        return {};
    std::wstring result(2, L'\0');
    for (size_t i = 0; i < 2; ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch >= 'a' && ch <= 'z')
            result[i] = static_cast<wchar_t>(ch - 'a' + 'A');
        else if (ch >= 'A' && ch <= 'Z')
            result[i] = static_cast<wchar_t>(ch);
        else
            return {};
    }
    return result;
}

} // namespace

bool ParseCloudflareTraceBody(const std::string& body, CloudflareTraceFields& fields)
{
    fields = {};
    std::string ipValue;
    if (!ExtractField(body, "ip", ipValue))
        return false;
    fields.ip = Token(ipValue);
    if (!ValidIp(fields.ip))
    {
        fields = {};
        return false;
    }
    std::string locValue;
    if (ExtractField(body, "loc", locValue))
        fields.loc = NormalizeLoc(locValue);
    return true;
}

} // namespace pluginping
