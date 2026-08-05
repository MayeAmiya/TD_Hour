#include "container/container_types.h"
#include "IniFile.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>

namespace data {

container::StringView IniFile::trim(container::StringView s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

container::String IniFile::toLower(container::StringView s)
{
    container::String result;
    result.reserve(s.size());
    for (char c : s)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return result;
}

bool IniFile::load(container::StringView content)
{
    clear();
    return overlay(content);
}

bool IniFile::overlay(container::StringView content)
{
    container::Vector<container::String> lines;
    size_t start = 0;
    while (start < content.size())
    {
        size_t end = content.find('\n', start);
        if (end == container::StringView::npos)
        {
            lines.emplace_back(content.substr(start));
            break;
        }
        container::StringView line = content.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.emplace_back(line);
        start = end + 1;
    }

    return loadFromLines(lines);
}

bool IniFile::loadFromLines(const container::Vector<container::String>& lines)
{
    EntryMap* currentSection = nullptr;
    container::String currentSectionName;

    for (const auto& rawLine : lines)
    {
        container::StringView line = trim(rawLine);
        if (line.empty())
            continue;

        if (line[0] == ';' || line[0] == '#')
            continue;

        if (line[0] == '[')
        {
            auto close = line.find(']');
            if (close == container::StringView::npos)
                continue;

            container::StringView name = trim(line.substr(1, close - 1));
            if (name.empty())
                continue;

            currentSectionName = toLower(name);
            currentSection = &m_sections[currentSectionName];
            continue;
        }

        if (!currentSection)
            continue;

        auto eq = line.find('=');
        if (eq == container::StringView::npos)
            continue;

        container::StringView entry = trim(line.substr(0, eq));
        if (entry.empty())
            continue;

        container::StringView value;
        if (eq + 1 < line.size())
            value = trim(line.substr(eq + 1));

        (*currentSection)[toLower(entry)] = container::String(value);
    }

    return true;
}

void IniFile::clear()
{
    m_sections.clear();
}

bool IniFile::hasSection(container::StringView section) const
{
    return m_sections.find(toLower(section)) != m_sections.end();
}

bool IniFile::hasEntry(container::StringView section, container::StringView entry) const
{
    auto secIt = m_sections.find(toLower(section));
    if (secIt == m_sections.end())
        return false;
    return secIt->second.find(toLower(entry)) != secIt->second.end();
}

size_t IniFile::entryCount(container::StringView section) const
{
    auto secIt = m_sections.find(toLower(section));
    if (secIt == m_sections.end())
        return 0;
    return secIt->second.size();
}

const container::String* IniFile::getString(container::StringView section, container::StringView entry) const
{
    auto secIt = m_sections.find(toLower(section));
    if (secIt == m_sections.end())
        return nullptr;
    auto entIt = secIt->second.find(toLower(entry));
    if (entIt == secIt->second.end())
        return nullptr;
    return &entIt->second;
}

container::String IniFile::getString(container::StringView section, container::StringView entry, container::StringView defaultVal) const
{
    auto* val = getString(section, entry);
    return val ? *val : container::String(defaultVal);
}

int IniFile::getInt(container::StringView section, container::StringView entry, int defaultVal) const
{
    auto* val = getString(section, entry);
    if (!val)
        return defaultVal;

    const char* start = val->data();
    char* end = nullptr;
    long result = std::strtol(start, &end, 0);
    if (end != start)
        return static_cast<int>(result);

    return defaultVal;
}

float IniFile::getFloat(container::StringView section, container::StringView entry, float defaultVal) const
{
    auto* val = getString(section, entry);
    if (!val)
        return defaultVal;

    float result = 0;
    auto [ptr, ec] = std::from_chars(val->data(), val->data() + val->size(), result);
    if (ec == std::errc())
        return result;

    return defaultVal;
}

double IniFile::getDouble(container::StringView section, container::StringView entry, double defaultVal) const
{
    auto* val = getString(section, entry);
    if (!val)
        return defaultVal;

    double result = 0;
    auto [ptr, ec] = std::from_chars(val->data(), val->data() + val->size(), result);
    if (ec == std::errc())
        return result;

    return defaultVal;
}

bool IniFile::getBool(container::StringView section, container::StringView entry, bool defaultVal) const
{
    auto* val = getString(section, entry);
    if (!val)
        return defaultVal;

    container::String lower = toLower(*val);
    if (lower == "yes" || lower == "true" || lower == "1")
        return true;
    if (lower == "no" || lower == "false" || lower == "0")
        return false;

    return defaultVal;
}

container::Vector<container::String> IniFile::sections() const
{
    container::Vector<container::String> result;
    result.reserve(m_sections.size());
    for (const auto& [name, _] : m_sections)
        result.push_back(name);
    std::sort(result.begin(), result.end());
    return result;
}

container::Vector<container::String> IniFile::entries(container::StringView section) const
{
    auto secIt = m_sections.find(toLower(section));
    if (secIt == m_sections.end())
        return {};

    container::Vector<container::String> result;
    result.reserve(secIt->second.size());
    for (const auto& [name, _] : secIt->second)
        result.push_back(name);
    std::sort(result.begin(), result.end());
    return result;
}

void IniFile::setString(container::StringView section, container::StringView entry, container::StringView value)
{
    m_sections[toLower(section)][toLower(entry)] = value;
}

void IniFile::setInt(container::StringView section, container::StringView entry, int value)
{
    setString(section, entry, std::to_string(value));
}

void IniFile::setFloat(container::StringView section, container::StringView entry, float value)
{
    setString(section, entry, std::to_string(value));
}

void IniFile::setBool(container::StringView section, container::StringView entry, bool value)
{
    setString(section, entry, value ? "yes" : "no");
}

void IniFile::removeSection(container::StringView section)
{
    m_sections.erase(toLower(section));
}

void IniFile::removeEntry(container::StringView section, container::StringView entry)
{
    auto secIt = m_sections.find(toLower(section));
    if (secIt != m_sections.end())
        secIt->second.erase(toLower(entry));
}

container::String IniFile::save() const
{
    container::String result;
    for (const container::String& secName : sections())
    {
        const auto section = m_sections.find(secName);
        if (section == m_sections.end())
            continue;

        result += '[';
        result += secName;
        result += "]\n";
        for (const container::String& entName : entries(secName))
        {
            const auto entry = section->second.find(entName);
            if (entry == section->second.end())
                continue;

            result += entName;
            result += '=';
            result += entry->second;
            result += '\n';
        }
        result += '\n';
    }
    return result;
}

} // namespace data
