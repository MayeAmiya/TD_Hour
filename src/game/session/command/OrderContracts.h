#pragma once

#include "core/container/container_types.h"

#include "game/command/GameCommand.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include <cstdint>
#include <limits>
#include <optional>
namespace engine {

struct PlayerGroupPathMember final {
    ObjectId object = INVALID_OBJECT_ID;
    math::q32_32 offsetX{};
    math::q32_32 offsetY{};
};

// Transaction-produced only; never serialized as player/network input.
// The canonical member list assigns one Navigation-owned centerline and a
// stable column offset to each eligible ground unit.
struct PlayerGroupPathPlan final {
    uint64_t id = 0;
    math::q32_32 startX{};
    math::q32_32 startY{};
    math::q32_32 startZ{};
    container::Vector<PlayerGroupPathMember> members;
};

struct PlayerOrder final {
    PlayerId player = INVALID_PLAYER_ID;
    GameTick tick = 0;
    uint32_t sequence = 0;
    ObjectOrderKind kind = ObjectOrderKind::Move;
    ObjectTacticalAttackSubtype tacticalAttackSubtype =
        ObjectTacticalAttackSubtype::None;
    container::Vector<ObjectId> actors;
    ObjectId targetObject = INVALID_OBJECT_ID;
    CommandPosition targetPosition;
    math::q32_32 placementYawRadians{};
    CommandPosition placementEndPosition;
    container::String contentName;
    bool forceAttack = false;
    bool attackMove = false;
    bool combatDrop = false;
    bool guardWithoutPursuit = false;
    bool guardFlyingOnly = false;
    bool queued = false;
    std::optional<PlayerGroupPathPlan> groupPath;
};

// The command ingress determines authorization.  `contextPlayer` remains
// useful provenance for diagnostics and future resource/economy semantics,
// but it must never be mistaken for a permission to control every actor.
enum class OrderAuthority : uint8_t {
    PlayerCommand,
    ScenarioScript,
    StrategicAI,
};

// Script actions either address a fixed named object set or the live members
// of one Scenario Team.  The latter is validated against ObjectTeamRegistry
// at admission so a stale effect cannot quietly direct an object that has
// left the team.
enum class ScriptOrderAuthority : uint8_t {
    NamedObjects,
    ScenarioTeam,
};

// Internal script intent deliberately has no CommandSource/GameTick envelope.
// A script runs during a confirmed tick already; it uses the same object-order
// admission path as a player order without becoming a replay/network command
// that another machine could inject.
struct ScriptOrderIntent final {
    PlayerId contextPlayer = INVALID_PLAYER_ID;
    ScriptOrderAuthority authority = ScriptOrderAuthority::NamedObjects;
    ObjectTeamId scenarioTeam = INVALID_OBJECT_TEAM_ID;
    uint64_t confirmedTick = 0;
    // Diagnostic source identity. It is deliberately not the queue ordering
    // key: one script can emit several ordered effects in one confirmed tick.
    uint32_t sourceScriptId = 0;
    uint32_t sourceEffectOrdinal = 0;
    ObjectOrderKind kind = ObjectOrderKind::Move;
    ObjectOrderSystemPurpose systemPurpose =
        ObjectOrderSystemPurpose::Generic;
    // Scenario scripts can currently submit only the typed Hunt tactical
    // wrapper. PlayerOrder intentionally has no corresponding payload and
    // GameCommand has no TacticalAttack wire type.
    ObjectTacticalAttackSubtype tacticalAttackSubtype =
        ObjectTacticalAttackSubtype::None;
    ObjectMoveRouteSubtype moveRouteSubtype = ObjectMoveRouteSubtype::Direct;
    container::Vector<ObjectId> actors;
    ObjectId targetObject = INVALID_OBJECT_ID;
    CommandPosition targetPosition;
    container::String contentName;
    // RefCode distinguishes NAMED_ATTACK_NAMED (force target) from
    // TEAM_ATTACK_NAMED (ordinary group attack). Keep the policy internal to
    // script/ECS order admission; player network commands retain false.
    bool forceAttack = false;
    // FIRE_WEAPON/ATTACK_MOVE CommandButtons preserve the authored shot cap
    // on the resulting Attack or attack-move intent. Empty means ordinary
    // open-ended combat behavior.
    std::optional<uint32_t> maximumShots;
    bool attackMove = false;
    uint32_t waypointStartId = std::numeric_limits<uint32_t>::max();
    uint64_t waypointGraphRevision = 0;
    bool allArmyHunt = false;
    bool useTeamCommonTarget = false;
    ObjectTeamId tacticalTargetTeam = INVALID_OBJECT_TEAM_ID;
    uint32_t tacticalTargetAreaId = std::numeric_limits<uint32_t>::max();
    uint64_t tacticalTargetRevision = 0;
    bool queued = false;
};

enum class OrderRejectionReason : uint8_t {
    None,
    UnsupportedCommand,
    MalformedOrder,
    InvalidPlayer,
    InvalidTarget,
    MissingActor,
    PendingDestroy,
    OwnershipMismatch,
    TeamMembershipMismatch,
    QueueCapacity,
};

struct OrderExecutionResult final {
    bool accepted = false;
    OrderRejectionReason rejection = OrderRejectionReason::None;
    size_t actorCount = 0;
    container::String message;
};

} // namespace engine
