#include "container/container_types.h"
#include "OptionPreferences.h"
#include "data/ini/IniFile.h"
#include "debug/debug.h"
#include "io/VFS.h"

#include <sstream>
#include <algorithm>
#include <cctype>

namespace config {

bool OptionPreferences::load(const container::String& filePath) {
    const container::String content = io::VFS::instance().readAll(filePath);
    if (content.empty()) {
        TD_LOG_WARN("Failed to open OptionPreferences file: {}", filePath);
        return false;
    }

    return loadFromString(content);
}

bool OptionPreferences::loadFromString(const container::String& content) {
    data::IniFile ini;
    if (!ini.load(content)) {
        TD_LOG_ERROR("Failed to parse OptionPreferences INI content");
        return false;
    }

    // Clear existing preferences
    m_preferences.clear();

    // Load all sections and entries
    auto sections = ini.sections();
    for (const auto& section : sections) {
        auto entries = ini.entries(section);
        for (const auto& entry : entries) {
            auto* value = ini.getString(section, entry);
            if (value) {
                // Store with section::entry key for uniqueness
                container::String fullKey = container::String(section) + "::" + container::String(entry);
                m_preferences[fullKey] = *value;
            }
        }
    }

    return true;
}

bool OptionPreferences::save(const container::String& filePath) const {
    if (!io::VFS::instance().writeAll(filePath, saveToString())) {
        TD_LOG_ERROR("Failed to open OptionPreferences file for writing: {}", filePath);
        return false;
    }

    return true;
}

container::String OptionPreferences::saveToString() const {
    data::IniFile ini;

    // Group preferences by section
    for (const auto& [key, value] : m_preferences) {
        size_t sep = key.find("::");
        if (sep != container::String::npos) {
            container::String section = key.substr(0, sep);
            container::String entry = key.substr(sep + 2);
            ini.setString(section, entry, value);
        }
    }

    return ini.save();
}

void OptionPreferences::clear() {
    m_preferences.clear();
}

bool OptionPreferences::getBool(const container::String& key, bool defaultValue) const {
    auto it = m_preferences.find(key);
    if (it == m_preferences.end()) {
        return defaultValue;
    }
    return parseBool(it->second, defaultValue);
}

int OptionPreferences::getInt(const container::String& key, int defaultValue) const {
    auto it = m_preferences.find(key);
    if (it == m_preferences.end()) {
        return defaultValue;
    }
    return parseInt(it->second, defaultValue);
}

float OptionPreferences::getFloat(const container::String& key, float defaultValue) const {
    auto it = m_preferences.find(key);
    if (it == m_preferences.end()) {
        return defaultValue;
    }
    return parseFloat(it->second, defaultValue);
}

double OptionPreferences::getDouble(const container::String& key, double defaultValue) const {
    auto it = m_preferences.find(key);
    if (it == m_preferences.end()) {
        return defaultValue;
    }
    return parseDouble(it->second, defaultValue);
}

container::String OptionPreferences::getString(const container::String& key, const container::String& defaultValue) const {
    auto it = m_preferences.find(key);
    if (it == m_preferences.end()) {
        return defaultValue;
    }
    return it->second;
}

void OptionPreferences::setBool(const container::String& key, bool value) {
    m_preferences[key] = boolToString(value);
}

void OptionPreferences::setInt(const container::String& key, int value) {
    m_preferences[key] = intToString(value);
}

void OptionPreferences::setFloat(const container::String& key, float value) {
    m_preferences[key] = floatToString(value);
}

void OptionPreferences::setDouble(const container::String& key, double value) {
    m_preferences[key] = doubleToString(value);
}

void OptionPreferences::setString(const container::String& key, const container::String& value) {
    m_preferences[key] = value;
}

bool OptionPreferences::hasKey(const container::String& key) const {
    return m_preferences.find(key) != m_preferences.end();
}

void OptionPreferences::remove(const container::String& key) {
    m_preferences.erase(key);
}

container::Vector<container::String> OptionPreferences::keys() const {
    container::Vector<container::String> result;
    result.reserve(m_preferences.size());
    for (const auto& [key, value] : m_preferences) {
        result.push_back(key);
    }
    return result;
}

container::String OptionPreferences::boolToString(bool value) {
    return value ? "yes" : "no";
}

container::String OptionPreferences::intToString(int value) {
    return std::to_string(value);
}

container::String OptionPreferences::floatToString(float value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%f", value);
    return buf;
}

container::String OptionPreferences::doubleToString(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%f", value);
    return buf;
}

bool OptionPreferences::parseBool(const container::String& value, bool defaultValue) {
    container::String lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "yes" || lower == "true" || lower == "1") {
        return true;
    }
    if (lower == "no" || lower == "false" || lower == "0") {
        return false;
    }
    return defaultValue;
}

int OptionPreferences::parseInt(const container::String& value, int defaultValue) {
    try {
        return std::stoi(value);
    } catch (...) {
        return defaultValue;
    }
}

float OptionPreferences::parseFloat(const container::String& value, float defaultValue) {
    try {
        return std::stof(value);
    } catch (...) {
        return defaultValue;
    }
}

double OptionPreferences::parseDouble(const container::String& value, double defaultValue) {
    try {
        return std::stod(value);
    } catch (...) {
        return defaultValue;
    }
}

} // namespace config
