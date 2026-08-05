#pragma once

#include "core/container/container_types.h"

#include "GameCommand.h"

#include <cstddef>
#include <cstdint>
namespace engine {

struct CommandCodecResult {
    bool ok = false;
    GameCommand command;
    container::String error;
    size_t bytesRead = 0;
};

class CommandCodec {
public:
    // v5 adds distinct PLAYER-upgrade factory transactions. v4 added
    // unit-production transactions and their producer-local job ID; v3 added
    // the explicit actor-facing Stop command; v2 replaced the
    // legacy single-subject + opaque-byte payload shape with a canonical actor
    // set and explicit queued flag. Old replay/recording compatibility is
    // intentionally not retained by this modern engine.
    // Version 6 carries authored Build yaw and the optional second LINEBUILD
    // anchor. Version 7 adds the bounded SetBeaconText command family.
    // Version 8 adds the actor-group + object-target Repair transaction.
    // Version 9 adds Sell and CancelConstruction. Version 10 adds the fully
    // serialized pointer-free ControlBar activation/post-accept context.
    // Version 11 added dedicated ExitContainer and Evacuate containment
    // transactions. Version 12 adds ExecuteRailedTransport. Version 13 adds
    // contextual EnterContainer. Version 14 adds typed CombatDrop. Version 15
    // adds the actorless PurchaseScience transaction. Version 16 replaces all
    // command positions and placement yaw with canonical Q32.32 wire values.
    // Version 17 adds the explicit player ForceAttack bit and the typed
    // CreateFormation command. Version 18 adds the three typed player Guard
    // modes and the immediate ToggleOvercharge transaction. Version 19 adds
    // deterministic cancellation of one actor-local order waypoint.
    // There is deliberately
    // no legacy reader.
    static constexpr uint16_t Version = 19;
    static constexpr size_t MaximumActors = 512;
    static constexpr size_t MaximumCommandNameBytes = 256;

    static container::Vector<uint8_t> encode(const GameCommand& command);
    static CommandCodecResult decode(const uint8_t* data, size_t size);
    static CommandCodecResult decode(const container::Vector<uint8_t>& data) {
        return decode(data.data(), data.size());
    }
};

} // namespace engine
