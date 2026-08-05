#pragma once

#include "core/container/container_types.h"
#include "game/session/command/OrderContracts.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <optional>

namespace engine {

struct ScriptCommandButtonSelectionResult final {
    container::Vector<ObjectId> actors;
    ObjectId sourceActor = INVALID_OBJECT_ID;
    ObjectId targetObject = INVALID_OBJECT_ID;
};

struct ScriptMultiplayerVictoryState final {
    bool singleAllianceRemaining = false;
    uint64_t endTick = 0;
    container::Vector<PlayerId> defeatedPlayers;
    container::Vector<PlayerId> victoriousPlayers;
};

struct ScriptOrderExecutionRecord final {
    uint64_t confirmedTick = 0;
    uint32_t sourceScriptId = 0;
    uint32_t sourceEffectOrdinal = 0;
    bool accepted = false;
    OrderRejectionReason rejection = OrderRejectionReason::None;
    size_t actorCount = 0;
};

enum class ObjectSightConcealment : uint8_t {
    IncludeHiddenStealth,
    RejectHiddenStealth,
};

struct ObjectSightQuery final {
    PlayerId targetPlayer = INVALID_PLAYER_ID;
    std::optional<PlayerRelationship> relationship;
    container::Span<const container::String> exactObjectTypes;
    ObjectSightConcealment concealment =
        ObjectSightConcealment::RejectHiddenStealth;
    bool requireAliveSource = false;
};

} // namespace engine
