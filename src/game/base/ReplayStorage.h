#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"
#include "game/command/GameCommand.h"
#include "game/player/MatchSetup.h"

#include <cstdint>
namespace game {

struct ReplayEntry {
    container::String fileName;
    container::String displayName;
    uintmax_t size = 0;
};

struct ReplayCommandReadResult {
    bool ok = false;
    engine::GameStartInfo startInfo;
    engine::ResolvedMatchSetup resolvedMatchSetup;
    container::Vector<engine::GameCommand> commands;
    container::String error;
};

class ReplayStorage {
public:
    static ReplayStorage& instance();

    container::Vector<ReplayEntry> listReplays() const;
    container::Vector<uint8_t> readReplay(const container::String& fileName) const;
    bool writeReplay(const container::String& fileName, const container::Vector<uint8_t>& content) const;
    ReplayCommandReadResult readReplayCommandStream(const container::String& fileName) const;
    container::Vector<engine::GameCommand> readReplayCommands(const container::String& fileName) const;
    bool writeReplayData(const container::String& fileName,
                         const engine::ResolvedMatchSetup& resolvedMatchSetup,
                         const container::Vector<engine::GameCommand>& commands) const;
    bool deleteReplay(const container::String& fileName) const;

private:
    ReplayStorage() = default;
};

} // namespace game
