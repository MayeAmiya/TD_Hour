#pragma once

#include "core/container/hash_containers.h"
#include "game/data/base/LegacyIniLoadType.h"
namespace game {

static constexpr int COMMAND_SET_SLOT_COUNT = 18;

struct CommandSetTemplate {
    container::String name;
    container::Array<container::String, COMMAND_SET_SLOT_COUNT> commands{};
};

class CommandSetStore {
public:
    static CommandSetStore& instance();

    void clear();
    bool loadFromIni(
        container::StringView filePath,
        ini::LegacyIniLoadType loadType = ini::LegacyIniLoadType::Overwrite);
    const CommandSetTemplate* find(container::StringView name) const;
    const container::HashMap<container::String, CommandSetTemplate>& all() const { return m_sets; }

private:
    container::HashMap<container::String, CommandSetTemplate> m_sets;
};

} // namespace game
