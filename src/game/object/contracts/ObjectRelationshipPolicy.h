#pragma once

#include "core/container/container_types.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

namespace engine {

struct ObjectTeamRelationshipOverride final {
    ObjectTeamId target = INVALID_OBJECT_TEAM_ID;
    PlayerRelationship relationship = PlayerRelationship::Neutral;

    constexpr bool operator==(const ObjectTeamRelationshipOverride&) const
        noexcept = default;
};

struct ObjectPlayerRelationshipOverride final {
    PlayerId target = INVALID_PLAYER_ID;
    PlayerRelationship relationship = PlayerRelationship::Neutral;

    constexpr bool operator==(const ObjectPlayerRelationshipOverride&) const
        noexcept = default;
};

// Immutable-address value shared by one live Team record and all of its
// current Object members. A script mutation copy-on-writes the small sorted
// vectors, then GameSession swaps the shared handle on current members.
// Existing relationship consumers therefore remain pointer-free and read no
// ObjectTeamRegistry service from ECS hot paths.
struct ObjectRelationshipOverridePolicy final {
    container::Vector<ObjectTeamRelationshipOverride> teams;
    container::Vector<ObjectPlayerRelationshipOverride> players;
    uint64_t revision = 1;
};

struct ObjectRelationshipOverrideComponent final {
    container::SharedPtr<const ObjectRelationshipOverridePolicy> policy;
};

} // namespace engine

