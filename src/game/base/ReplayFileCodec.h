#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"
#include "game/command/GameCommand.h"
#include "game/player/MatchSetup.h"

#include <cstddef>
#include <cstdint>
namespace game {

// Every replay is self-describing through a canonical resolved setup.  The
// displayed GameStartInfo is rebuilt from that setup for GameLogic; local
// control is deliberately absent so a replay never inherits the recorder's
// player slot or lobby-only legacy fields.
struct ReplayData {
    engine::GameStartInfo startInfo;
    engine::ResolvedMatchSetup resolvedMatchSetup;
    container::Vector<engine::GameCommand> commands;
};

struct ReplayFileResult {
    bool ok = false;
    ReplayData replay;
    container::String error;
};

class ReplayFileCodec {
public:
    static constexpr uint32_t Magic = 0x50525447; // "GTRP" little-endian
    // This format deliberately has no legacy reader.  A replay contains a
    // versioned MatchSetupCodec payload followed by its command stream.
    // Version 12 carries CommandStream v3 / CommandCodec v10 ControlBar
    // activation metadata. Version 13 carries CommandStream v4 /
    // CommandCodec v15 PurchaseScience. Version 14 carries CommandStream v5 /
    // CommandCodec v16 fixed command coordinates. CommandCodec has no legacy payload
    // reader; replay compatibility is intentionally explicit rather than
    // silently interpreting old commands.
    static constexpr uint16_t Version = 14;
    static constexpr uint16_t RulesVersion = 1;

    static container::Vector<uint8_t> encode(const engine::ResolvedMatchSetup& resolvedMatchSetup,
                                       const container::Vector<engine::GameCommand>& commands);
    static ReplayFileResult decode(const uint8_t* data, size_t size);
    static ReplayFileResult decode(const container::Vector<uint8_t>& data) {
        return decode(data.data(), data.size());
    }
};

} // namespace game
