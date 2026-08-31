// PluginPing - immutable shared INI snapshot implementation.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ConfigFile.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>
#include <vector>

namespace pluginping_shared {

namespace {

struct FileStamp
{
    unsigned long long writeTime = 0;
    unsigned long long size = 0;
    bool exists = false;
};

std::wstring Trim(const std::wstring& value)
{
    size_t first = 0;
    while (first < value.size() && iswspace(value[first]))
        ++first;
    size_t last = value.size();
    while (last > first && iswspace(value[last - 1]))
        --last;
    return value.substr(first, last - first);
}

std::wstring Lower(std::wstring value)
{
    for (wchar_t& ch : value)
        ch = static_cast<wchar_t>(towlower(ch));
    return value;
}

FileStamp Stamp(const std::wstring& path)
{
    FileStamp result;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return result;

    ULARGE_INTEGER time{};
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    result.writeTime = time.QuadPart;
    result.size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
                  data.nFileSizeLow;
    result.exists = true;
    return result;
}

bool SameStamp(const FileStamp& left, const FileStamp& right)
{
    return left.exists == right.exists && left.writeTime == right.writeTime &&
           left.size == right.size;
}

bool ReadBytes(const std::wstring& path, std::vector<unsigned char>& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER length{};
    const bool sized = GetFileSizeEx(file, &length) && length.QuadPart >= 0 &&
                       length.QuadPart <= static_cast<LONGLONG>(32 * 1024 * 1024);
    if (!sized)
    {
        CloseHandle(file);
        return false;
    }

    bytes.assign(static_cast<size_t>(length.QuadPart), 0);
    size_t offset = 0;
    while (offset < bytes.size())
    {
        const DWORD request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset, static_cast<size_t>(1 << 20)));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + offset, request, &read, nullptr) || read == 0)
        {
            CloseHandle(file);
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

std::wstring Decode(const std::vector<unsigned char>& bytes)
{
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
    {
        std::wstring value;
        value.reserve((bytes.size() - 2) / 2);
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            value.push_back(static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8)));
        return value;
    }

    size_t offset = 0;
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        offset = 3;

    const char* data = reinterpret_cast<const char*>(bytes.data() + offset);
    const int size = static_cast<int>(bytes.size() - offset);
    int chars = size > 0 ? MultiByteToWideChar(codePage, flags, data, size, nullptr, 0) : 0;
    if (chars <= 0 && size > 0)
    {
        codePage = CP_ACP;
        flags = 0;
        chars = MultiByteToWideChar(codePage, flags, data, size, nullptr, 0);
    }
    if (chars <= 0)
        return std::wstring();

    std::wstring value(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(codePage, flags, data, size, value.data(), chars);
    return value;
}

std::shared_ptr<IniSnapshot> Parse(const std::vector<unsigned char>& bytes,
                                   unsigned long long generation)
{
    auto snapshot = std::make_shared<IniSnapshot>();
    snapshot->SetGeneration(generation);

    const std::wstring text = Decode(bytes);
    std::wstring section;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t newline = text.find(L'\n', start);
        const size_t end = newline == std::wstring::npos ? text.size() : newline;
        std::wstring line = Trim(text.substr(start, end - start));
        if (!line.empty() && line.front() != L';' && line.front() != L'#')
        {
            if (line.front() == L'[' && line.back() == L']')
            {
                section = Lower(Trim(line.substr(1, line.size() - 2)));
            }
            else
            {
                const size_t equals = line.find(L'=');
                if (equals != std::wstring::npos && !section.empty())
                {
                    const std::wstring key = Lower(Trim(line.substr(0, equals)));
                    if (!key.empty())
                        snapshot->SetValue(generation, section, key, Trim(line.substr(equals + 1)));
                }
            }
        }

        if (newline == std::wstring::npos)
            break;
        start = newline + 1;
    }
    return snapshot;
}

struct CacheEntry
{
    FileStamp stamp;
    std::shared_ptr<const IniSnapshot> snapshot;
};

struct Cache
{
    std::mutex mutex;
    std::map<std::wstring, CacheEntry> entries;
};

Cache* GetCache() noexcept
{
    static Cache* cache = new Cache();
    return cache;
}

} // namespace

void IniSnapshot::SetValue(unsigned long long generation, const std::wstring& section,
                            const std::wstring& key, const std::wstring& value)
{
    m_generation = generation;
    m_values[section][key] = value;
}

std::wstring IniSnapshot::String(const wchar_t* section, const wchar_t* key,
                                 const std::wstring& fallback) const
{
    if (!section || !key)
        return fallback;
    const auto sectionIt = m_values.find(Lower(section));
    if (sectionIt == m_values.end())
        return fallback;
    const auto keyIt = sectionIt->second.find(Lower(key));
    return keyIt == sectionIt->second.end() ? fallback : keyIt->second;
}

int IniSnapshot::Integer(const wchar_t* section, const wchar_t* key, int fallback) const
{
    const std::wstring value = String(section, key, std::wstring());
    if (value.empty())
        return fallback;
    wchar_t* end = nullptr;
    const long parsed = wcstol(value.c_str(), &end, 10);
    return end == value.c_str() ? 0 : static_cast<int>(parsed);
}

bool IniSnapshot::Boolean(const wchar_t* section, const wchar_t* key, bool fallback) const
{
    const std::wstring value = String(section, key, fallback ? L"1" : L"0");
    if (_wcsicmp(value.c_str(), L"true") == 0 || _wcsicmp(value.c_str(), L"yes") == 0 ||
        _wcsicmp(value.c_str(), L"on") == 0)
        return true;
    if (_wcsicmp(value.c_str(), L"false") == 0 || _wcsicmp(value.c_str(), L"no") == 0 ||
        _wcsicmp(value.c_str(), L"off") == 0)
        return false;
    const wchar_t first = value.empty() ? L'\0' : value[0];
    if (first == L'-' || first == L'+' || iswdigit(first))
        return _wtoi(value.c_str()) != 0;
    return fallback;
}

std::shared_ptr<const IniSnapshot> GetIniSnapshot(const std::wstring& path)
{
    Cache* cache = GetCache();
    std::lock_guard<std::mutex> lock(cache->mutex);
    CacheEntry& entry = cache->entries[path];
    const FileStamp current = Stamp(path);
    if (entry.snapshot && SameStamp(current, entry.stamp))
        return entry.snapshot;
    if (!current.exists)
        return entry.snapshot ? entry.snapshot : std::make_shared<IniSnapshot>();

    std::vector<unsigned char> bytes;
    FileStamp after;
    bool readStable = false;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const FileStamp before = Stamp(path);
        if (!before.exists || !ReadBytes(path, bytes))
            break;
        after = Stamp(path);
        if (SameStamp(before, after))
        {
            readStable = true;
            break;
        }
        Sleep(10);
    }

    if (!readStable)
        return entry.snapshot ? entry.snapshot : std::make_shared<IniSnapshot>();

    const unsigned long long generation = entry.snapshot
        ? entry.snapshot->Generation() + 1
        : 1;
    entry.snapshot = Parse(bytes, generation);
    entry.stamp = after;
    return entry.snapshot;
}

} // namespace pluginping_shared
