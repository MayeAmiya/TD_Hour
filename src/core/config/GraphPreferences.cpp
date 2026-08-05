#include "container/container_types.h"
#include "GraphPreferences.h"
#include "debug/debug.h"
#include "io/VFS.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace config {
namespace {

[[nodiscard]] container::StringView trimPreferenceToken(
    container::StringView value) noexcept {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' ||
            value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' ||
            value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] container::StringView preferenceEntryName(
    container::StringView key) noexcept {
    key = trimPreferenceToken(key);
    const size_t separator = key.rfind("::");
    return trimPreferenceToken(separator == container::StringView::npos
            ? key : key.substr(separator + 2u));
}

[[nodiscard]] container::String canonicalPreferenceKey(
    container::StringView key) {
    key = preferenceEntryName(key);
    container::String result;
    result.reserve(key.size());
    for (const unsigned char value : key) {
        result.push_back(static_cast<char>(std::tolower(value)));
    }
    return result;
}

} // namespace

bool GraphPreferences::load(const container::String& filePath) {
    const container::String content = io::VFS::instance().readAll(filePath);
    if (content.empty()) {
        TD_LOG_WARN("Options file not found: {}, using defaults", filePath);
        return false;
    }

    return loadFromString(content);
}

bool GraphPreferences::loadFromString(const container::String& content) {
    container::HashMap<container::String, container::String> parsed;
    container::HashMap<container::String, container::String> spellings;
    std::istringstream input(content);
    container::String line;
    bool firstLine = true;
    while (std::getline(input, line)) {
        container::StringView view = trimPreferenceToken(line);
        if (firstLine && view.size() >= 3 &&
            static_cast<unsigned char>(view[0]) == 0xEF &&
            static_cast<unsigned char>(view[1]) == 0xBB &&
            static_cast<unsigned char>(view[2]) == 0xBF) {
            view.remove_prefix(3);
            view = trimPreferenceToken(view);
        }
        firstLine = false;
        if (view.empty() || view.front() == ';' || view.front() == '#' ||
            view.front() == '[') {
            // Original Options.ini is flat. Accept sectioned files as a
            // compatibility input by ignoring the heading and flattening
            // their entries into the same case-insensitive key space.
            continue;
        }
        const size_t equals = view.find('=');
        if (equals == container::StringView::npos) continue;
        const container::StringView spelling =
            preferenceEntryName(view.substr(0, equals));
        const container::String canonical = canonicalPreferenceKey(spelling);
        if (canonical.empty()) continue;
        parsed[canonical] = container::String(
            trimPreferenceToken(view.substr(equals + 1u)));
        spellings[canonical] = container::String(spelling);
    }
    m_preferences = std::move(parsed);
    m_keySpelling = std::move(spellings);
    return true;
}

bool GraphPreferences::save(const container::String& filePath) const {
    if (!io::VFS::instance().writeAll(filePath, saveToString())) {
        TD_LOG_ERROR("Failed to open Options file for writing: {}", filePath);
        return false;
    }

    return true;
}

container::String GraphPreferences::saveToString() const {
    container::Vector<container::String> keys;
    keys.reserve(m_preferences.size());
    for (const auto& [key, _] : m_preferences) keys.push_back(key);
    std::sort(keys.begin(), keys.end());

    container::String output;
    for (const container::String& key : keys) {
        const auto value = m_preferences.find(key);
        if (value == m_preferences.end()) continue;
        const auto spelling = m_keySpelling.find(key);
        output += spelling != m_keySpelling.end() ? spelling->second : key;
        output += " = ";
        output += value->second;
        output.push_back('\n');
    }
    return output;
}

void GraphPreferences::clear() {
    m_preferences.clear();
    m_keySpelling.clear();
}

bool GraphPreferences::getBool(const container::String& key, bool defaultValue) const {
    auto it = m_preferences.find(canonicalPreferenceKey(key));
    if (it == m_preferences.end()) {
        return defaultValue;
    }
    return parseBool(it->second, defaultValue);
}

int GraphPreferences::getInt(const container::String& key, int defaultValue) const {
    auto it = m_preferences.find(canonicalPreferenceKey(key));
    if (it == m_preferences.end()) {
        return defaultValue;
    }
    return parseInt(it->second, defaultValue);
}

float GraphPreferences::getFloat(const container::String& key, float defaultValue) const {
    auto it = m_preferences.find(canonicalPreferenceKey(key));
    if (it == m_preferences.end()) {
        return defaultValue;
    }
    return parseFloat(it->second, defaultValue);
}

container::String GraphPreferences::getString(const container::String& key, const container::String& defaultValue) const {
    auto it = m_preferences.find(canonicalPreferenceKey(key));
    if (it == m_preferences.end()) {
        return defaultValue;
    }
    return it->second;
}

void GraphPreferences::setBool(const container::String& key, bool value) {
    setString(key, boolToString(value));
}

void GraphPreferences::setInt(const container::String& key, int value) {
    setString(key, intToString(value));
}

void GraphPreferences::setFloat(const container::String& key, float value) {
    setString(key, floatToString(value));
}

void GraphPreferences::setString(const container::String& key, const container::String& value) {
    const container::String canonical = canonicalPreferenceKey(key);
    if (canonical.empty()) return;
    m_preferences[canonical] = value;
    m_keySpelling[canonical] = container::String(preferenceEntryName(key));
}

bool GraphPreferences::hasKey(const container::String& key) const {
    return m_preferences.find(canonicalPreferenceKey(key)) !=
        m_preferences.end();
}

void GraphPreferences::remove(const container::String& key) {
    const container::String canonical = canonicalPreferenceKey(key);
    m_preferences.erase(canonical);
    m_keySpelling.erase(canonical);
}

container::String GraphPreferences::boolToString(bool value) {
    return value ? "yes" : "no";
}

container::String GraphPreferences::intToString(int value) {
    return std::to_string(value);
}

container::String GraphPreferences::floatToString(float value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%f", value);
    return buf;
}

bool GraphPreferences::parseBool(const container::String& value, bool defaultValue) {
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

int GraphPreferences::parseInt(const container::String& value, int defaultValue) {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || errno == ERANGE) return defaultValue;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0' || parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return defaultValue;
    }
    return static_cast<int>(parsed);
}

float GraphPreferences::parseFloat(const container::String& value, float defaultValue) {
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || errno == ERANGE || !std::isfinite(parsed))
        return defaultValue;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return *end == '\0' ? parsed : defaultValue;
}

} // namespace config
