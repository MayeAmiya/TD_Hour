#pragma once

#include "container/hash_containers.h"
#include <cstdint>
#include <optional>
#include <utility>

namespace engine {

// Two-phase command line processor
// Phase 1: Parse raw command line into key=value pairs
// Phase 2: Apply parsed options to engine/game systems
class CommandLine {
public:
    CommandLine() = default;
    ~CommandLine() = default;

    // Phase 1: Parse command line from main(int argc, char* argv[])
    void parse(int argc, const char* const* argv);

    // Phase 1: Parse from raw string
    void parseFromString(const container::String& cmdline);

    // Phase 2: Apply parsed options (called after systems init)
    void apply();

    // Getters
    bool hasParam(const container::String& key) const;
    container::String getParam(const container::String& key, const container::String& defaultVal = "") const;
    int getIntParam(const container::String& key, int defaultVal = 0) const;
    bool getBoolParam(const container::String& key, bool defaultVal = false) const;
    float getFloatParam(const container::String& key, float defaultVal = 0.0f) const;
    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>>
    getResolutionParam() const;

    // Get all parameters
    const container::HashMap<container::String, container::String>& getParams() const { return m_params; }

    // Get raw args (for things like mod path that need full args)
    const container::Vector<container::String>& getArgs() const { return m_args; }

    // Setters for programmatic use
    void setParam(const container::String& key, const container::String& value);
    void clearParam(const container::String& key);

    // Singleton
    static CommandLine& instance();

private:
    container::HashMap<container::String, container::String> m_params;
    container::Vector<container::String> m_args;
    bool m_applied = false;

    static container::String toLower(const container::String& s);

    // Common parameter keys
    static constexpr const char* KEY_MOD_PATH = "mod";
    static constexpr const char* KEY_RESOLUTION = "resolution";
    static constexpr const char* KEY_WINDOWED = "windowed";
    static constexpr const char* KEY_FULLSCREEN = "fullscreen";
    static constexpr const char* KEY_NO_SOUND = "nosound";
    static constexpr const char* KEY_NO_MUSIC = "nomusic";
    static constexpr const char* KEY_DEVMODE = "dev";
    static constexpr const char* KEY_LOG_LEVEL = "loglevel";
    static constexpr const char* KEY_SAVE_DIR = "savedir";
};

} // namespace engine
