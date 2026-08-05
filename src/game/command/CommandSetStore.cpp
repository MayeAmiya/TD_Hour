#include "core/container/container_types.h"
#include "CommandSetStore.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "debug/debug.h"

#include <cstdlib>

namespace game {

CommandSetStore& CommandSetStore::instance() {
    static CommandSetStore s_instance;
    return s_instance;
}

void CommandSetStore::clear() {
    m_sets.clear();
}

bool CommandSetStore::loadFromIni(
    container::StringView filePath, ini::LegacyIniLoadType loadType) {
    GeneralsIniParser parser;
    const container::String path{filePath};
    if (!parser.parseFile(path)) {
        return false;
    }

    bool valid = true;
    for (const auto& block : parser.blocks()) {
        if (block.type != "CommandSet") continue;
        if (block.name.empty()) continue;

        CommandSetTemplate set;
        const auto existing = m_sets.find(block.name);
        if (existing != m_sets.end()) {
            if (!ini::createsOverrides(loadType)) {
                // Unlike CommandButton, ZH rejects an ordinary duplicate
                // CommandSet instead of replacing or partially patching it.
                TD_LOG_WARN(
                    "[CommandSetStore] Ignored duplicate CommandSet '{}' without CreateOverrides",
                    block.name);
                valid = false;
                continue;
            }
            set = existing->second;
        }
        set.name = block.name;

        for (const auto& [key, value] : block.values) {
            const int oneBasedSlot = static_cast<int>(std::strtol(key.c_str(), nullptr, 10));
            if (oneBasedSlot <= 0 || oneBasedSlot > COMMAND_SET_SLOT_COUNT) continue;
            set.commands[oneBasedSlot - 1] = value;
        }

        m_sets.insert_or_assign(set.name, std::move(set));
    }

    TD_LOG_INFO("[CommandSetStore] Loaded {} command sets from {}", m_sets.size(), path);
    return valid;
}

const CommandSetTemplate* CommandSetStore::find(container::StringView name) const {
    auto it = m_sets.find(container::String{name});
    return it != m_sets.end() ? &it->second : nullptr;
}

} // namespace game
