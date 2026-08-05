#include "container/container_types.h"
#include "CommandLine.h"
#include <algorithm>
#include <charconv>
#include <sstream>
#include <cctype>

namespace engine {

CommandLine& CommandLine::instance() {
    static CommandLine s_instance;
    return s_instance;
}

void CommandLine::parse(int argc, const char* const* argv) {
    m_params.clear();
    m_args.clear();

    for (int i = 1; i < argc; ++i) {
        container::String arg = argv[i];
        m_args.push_back(arg);

        // Check for --key=value format
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') {
            size_t eqPos = arg.find('=', 2);
            if (eqPos != container::String::npos) {
                container::String key = arg.substr(2, eqPos - 2);
                container::String value = arg.substr(eqPos + 1);
                m_params[toLower(key)] = value;
            } else {
                container::String key = arg.substr(2);
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    m_params[toLower(key)] = argv[++i];
                } else {
                    m_params[toLower(key)] = "true";
                }
            }
        }
        // Check for -key value format
        else if (arg.size() > 1 && arg[0] == '-') {
            container::String key = arg.substr(1);
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                m_params[toLower(key)] = argv[++i];
            } else {
                m_params[toLower(key)] = "true";
            }
        }
        // Bare argument
        else {
            m_params[toLower(arg)] = "true";
        }
    }
}

void CommandLine::parseFromString(const container::String& cmdline) {
    container::Vector<container::String> args;
    std::istringstream iss(cmdline);
    container::String token;
    bool inQuote = false;

    while (iss >> std::skipws) {
        char c = iss.peek();
        if (c == '"') {
            iss.get();
            inQuote = true;
        }

        token.clear();
        if (inQuote) {
            std::getline(iss, token, '"');
            inQuote = false;
        } else {
            iss >> token;
        }

        if (!token.empty()) {
            args.push_back(token);
        }
    }

    // Convert to argc/argv format
    container::Vector<const char*> argv;
    argv.push_back("engine"); // placeholder for program name
    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }

    parse(static_cast<int>(argv.size()), argv.data());
}

void CommandLine::apply() {
    if (m_applied) return;
    m_applied = true;

    // Apply could set GlobalData fields, configure systems, etc.
    // This is called after GlobalData is loaded but before game systems init
}

bool CommandLine::hasParam(const container::String& key) const {
    return m_params.find(toLower(key)) != m_params.end();
}

container::String CommandLine::getParam(const container::String& key, const container::String& defaultVal) const {
    auto it = m_params.find(toLower(key));
    if (it != m_params.end()) {
        return it->second;
    }
    return defaultVal;
}

int CommandLine::getIntParam(const container::String& key, int defaultVal) const {
    container::String val = getParam(key);
    if (!val.empty()) {
        try {
            return std::stoi(val);
        } catch (...) {
            return defaultVal;
        }
    }
    return defaultVal;
}

bool CommandLine::getBoolParam(const container::String& key, bool defaultVal) const {
    container::String val = getParam(key);
    if (val.empty()) return defaultVal;

    container::String lower = toLower(val);
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

float CommandLine::getFloatParam(const container::String& key, float defaultVal) const {
    container::String val = getParam(key);
    if (!val.empty()) {
        try {
            return std::stof(val);
        } catch (...) {
            return defaultVal;
        }
    }
    return defaultVal;
}

std::optional<std::pair<uint32_t, uint32_t>>
CommandLine::getResolutionParam() const {
    const container::String value = getParam(KEY_RESOLUTION);
    const size_t separator = value.find_first_of("xX");
    if (separator == container::String::npos || separator == 0u ||
        separator + 1u >= value.size()) {
        return std::nullopt;
    }
    const auto parseDimension = [](container::StringView text)
        -> std::optional<uint32_t> {
        uint32_t result = 0;
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() || result == 0u) {
            return std::nullopt;
        }
        return result;
    };
    const std::optional<uint32_t> width = parseDimension(
        container::StringView{value}.substr(0u, separator));
    const std::optional<uint32_t> height = parseDimension(
        container::StringView{value}.substr(separator + 1u));
    if (!width || !height) return std::nullopt;
    return std::pair{*width, *height};
}

void CommandLine::setParam(const container::String& key, const container::String& value) {
    m_params[toLower(key)] = value;
}

void CommandLine::clearParam(const container::String& key) {
    m_params.erase(toLower(key));
}

container::String CommandLine::toLower(const container::String& s) {
    container::String result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

} // namespace engine
