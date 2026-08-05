#pragma once

#include "container/hash_containers.h"

#include <cstdint>
namespace data {

class IniFile
{
public:
    IniFile() = default;

    bool load(container::StringView content);
    bool overlay(container::StringView content);
    bool loadFromLines(const container::Vector<container::String>& lines);
    void clear();

    [[nodiscard]] bool hasSection(container::StringView section) const;
    [[nodiscard]] bool hasEntry(container::StringView section, container::StringView entry) const;
    [[nodiscard]] size_t sectionCount() const { return m_sections.size(); }
    [[nodiscard]] size_t entryCount(container::StringView section) const;

    [[nodiscard]] const container::String* getString(container::StringView section, container::StringView entry) const;
    [[nodiscard]] container::String getString(container::StringView section, container::StringView entry, container::StringView defaultVal) const;
    [[nodiscard]] int getInt(container::StringView section, container::StringView entry, int defaultVal = 0) const;
    [[nodiscard]] float getFloat(container::StringView section, container::StringView entry, float defaultVal = 0.0f) const;
    [[nodiscard]] double getDouble(container::StringView section, container::StringView entry, double defaultVal = 0.0) const;
    [[nodiscard]] bool getBool(container::StringView section, container::StringView entry, bool defaultVal = false) const;

    [[nodiscard]] container::Vector<container::String> sections() const;
    [[nodiscard]] container::Vector<container::String> entries(container::StringView section) const;

    void setString(container::StringView section, container::StringView entry, container::StringView value);
    void setInt(container::StringView section, container::StringView entry, int value);
    void setFloat(container::StringView section, container::StringView entry, float value);
    void setBool(container::StringView section, container::StringView entry, bool value);
    void removeSection(container::StringView section);
    void removeEntry(container::StringView section, container::StringView entry);

    [[nodiscard]] container::String save() const;

private:
    struct EntryMapHash
    {
        using hash_type = std::hash<container::StringView>;
        using is_transparent = void;

        size_t operator()(const char* s) const { return hash_type{}(s); }
        size_t operator()(container::StringView s) const { return hash_type{}(s); }
        size_t operator()(const container::String& s) const { return hash_type{}(s); }
    };

    using EntryMap = container::HashMap<container::String, container::String>;

    struct SectionMapHash
    {
        using hash_type = std::hash<container::StringView>;
        using is_transparent = void;

        size_t operator()(const char* s) const { return hash_type{}(s); }
        size_t operator()(container::StringView s) const { return hash_type{}(s); }
        size_t operator()(const container::String& s) const { return hash_type{}(s); }
    };

    using SectionMap = container::HashMap<container::String, EntryMap>;

    SectionMap m_sections;

    static container::StringView trim(container::StringView s);
    static container::String toLower(container::StringView s);
};

} // namespace data
