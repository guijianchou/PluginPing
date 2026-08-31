// PluginPing - immutable shared INI snapshot.
#pragma once

#include <memory>
#include <map>
#include <string>

namespace pluginping_shared {

class IniSnapshot
{
public:
    std::wstring String(const wchar_t* section, const wchar_t* key,
                       const std::wstring& fallback) const;
    int Integer(const wchar_t* section, const wchar_t* key, int fallback) const;
    bool Boolean(const wchar_t* section, const wchar_t* key, bool fallback) const;
    unsigned long long Generation() const noexcept { return m_generation; }

    // Used only by the file parser before the snapshot is published.
    void SetGeneration(unsigned long long generation) noexcept { m_generation = generation; }
    void SetValue(unsigned long long generation, const std::wstring& section,
                  const std::wstring& key, const std::wstring& value);

private:
    friend std::shared_ptr<const IniSnapshot> GetIniSnapshot(const std::wstring& path);

    unsigned long long m_generation = 0;
    std::map<std::wstring, std::map<std::wstring, std::wstring>> m_values;
};

// Reads a complete file into one immutable snapshot. A changed file is only
// published after its metadata is stable before and after the read.
std::shared_ptr<const IniSnapshot> GetIniSnapshot(const std::wstring& path);

} // namespace pluginping_shared
