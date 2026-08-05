#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/object/ai/contracts/AIOrderIdentity.h"
#include "game/navigation/contracts/NavigationPathContracts.h"

namespace engine::ai
{

// Value mirrors of the ECS order protocol.  Keep the ordinals synchronized
// with ObjectOrderKind, ObjectOrderSource and ObjectOrderSystemPurpose.  This
// header deliberately does not retain an ECS component, entity or registry.
enum class ObjectAIOrderKind : uint8_t
{
    Move = 0,
    Stop,
    Attack,
    Build,
    CommandButton,
    SpecialPower,
    TacticalAttack,
    Count,
    Invalid = 0xff,
};

enum class ObjectAIOrderSource : uint8_t
{
    Player = 0,
    Script,
    System,
    Count,
    Invalid = 0xff,
};

enum class ObjectAIOrderSystemPurpose : uint8_t
{
    Generic = 0,
    CleanupHazard,
    AutoFindHealing,
    SupplyTruck,
    RailedTransport,
    AssaultTransport,
    PilotFindVehicle,
    DeliverPayload,
    Builder,
    SlaveReturn,
    TacticalAssist,
    SpecialAbility,
    CommandButtonHunt,
    Wander,
    ContainmentExit,
    ProductionExit,
    RepairDock,
    Retaliation,
    ContainmentEnter,
    ScenarioReinforcementDeliver,
    ScenarioReinforcementExit,
    MoveAside,
    IntentionalContact,
    ObjectCreationAttack,
    StrategicAI,
    ParachuteLanding,
    ConstructionEvacuation,
    CommandButtonFireWeapon,
    Count,
    Invalid = 0xff,
};

static_assert(static_cast<uint8_t>(ObjectAIOrderKind::Count) == 7);
static_assert(static_cast<uint8_t>(ObjectAIOrderSource::Count) == 3);
static_assert(static_cast<uint8_t>(ObjectAIOrderSystemPurpose::Count) == 28);

// Bind the protocol-header mirrors to the real enums so appending a value can
// never again leave AIAsyncOrderIdentity::isValid() rejecting it.
static_assert(static_cast<uint8_t>(ObjectAIOrderSource::Count) ==
              kAIAsyncOrderSourceCount);
static_assert(static_cast<uint8_t>(ObjectAIOrderSystemPurpose::Count) ==
              kAIAsyncOrderSystemPurposeCount);

enum class ObjectAIOrderCapability : uint8_t
{
    None = 0,
    MoveStop = uint8_t{1} << 0,
    Attack = uint8_t{1} << 1,
    // Reserved for a future typed special-order adapter.  The current owner
    // table intentionally does not transfer SpecialPower or CommandButton.
    Special = uint8_t{1} << 2,
};

enum class ObjectAITacticalAttackSubtype : uint8_t
{
    None = 0,
    Hunt,
    Guard,
    AttackSquad,
    AttackArea,
    GuardRetaliate,
    GuardTunnelNetwork,
    Count,
    Invalid = 0xff,
};

enum class ObjectAIMoveRouteSubtype : uint8_t
{
    Direct = 0,
    WaypointPathIndividuals,
    WaypointPathTeam,
    WaypointPathIndividualsExact,
    WaypointPathTeamExact,
    WanderWaypointPath,
    PanicWaypointPath,
    FollowPath,
    FollowExitProductionPath,
    Tighten,
    MoveAside,
    WanderInPlace,
    Count,
    Invalid = 0xff,
};

[[nodiscard]] constexpr bool isValidObjectAIMoveRouteSubtype(
    ObjectAIMoveRouteSubtype subtype) noexcept
{
    return subtype < ObjectAIMoveRouteSubtype::Count;
}

[[nodiscard]] constexpr bool isObjectAIWaypointRouteSubtype(
    ObjectAIMoveRouteSubtype subtype) noexcept
{
    return subtype == ObjectAIMoveRouteSubtype::WaypointPathIndividuals ||
           subtype == ObjectAIMoveRouteSubtype::WaypointPathTeam ||
           subtype == ObjectAIMoveRouteSubtype::WaypointPathIndividualsExact ||
           subtype == ObjectAIMoveRouteSubtype::WaypointPathTeamExact ||
           subtype == ObjectAIMoveRouteSubtype::WanderWaypointPath ||
           subtype == ObjectAIMoveRouteSubtype::PanicWaypointPath;
}

[[nodiscard]] constexpr bool objectAIWaypointRouteMovesAsTeam(
    ObjectAIMoveRouteSubtype subtype) noexcept
{
    return subtype == ObjectAIMoveRouteSubtype::WaypointPathTeam ||
           subtype == ObjectAIMoveRouteSubtype::WaypointPathTeamExact;
}

[[nodiscard]] constexpr bool isValidObjectAITacticalAttackSubtype(
    ObjectAITacticalAttackSubtype subtype) noexcept
{
    return subtype == ObjectAITacticalAttackSubtype::Hunt ||
           subtype == ObjectAITacticalAttackSubtype::Guard ||
           subtype == ObjectAITacticalAttackSubtype::AttackSquad ||
           subtype == ObjectAITacticalAttackSubtype::AttackArea ||
           subtype == ObjectAITacticalAttackSubtype::GuardRetaliate ||
           subtype == ObjectAITacticalAttackSubtype::GuardTunnelNetwork;
}

[[nodiscard]] constexpr ObjectAIOrderCapability operator|(
    ObjectAIOrderCapability left, ObjectAIOrderCapability right) noexcept
{
    return static_cast<ObjectAIOrderCapability>(
        static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

[[nodiscard]] constexpr ObjectAIOrderCapability operator&(
    ObjectAIOrderCapability left, ObjectAIOrderCapability right) noexcept
{
    return static_cast<ObjectAIOrderCapability>(
        static_cast<uint8_t>(left) & static_cast<uint8_t>(right));
}

constexpr ObjectAIOrderCapability& operator|=(
    ObjectAIOrderCapability& left, ObjectAIOrderCapability right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool hasObjectAIOrderCapability(
    ObjectAIOrderCapability mask, ObjectAIOrderCapability capability) noexcept
{
    return capability != ObjectAIOrderCapability::None &&
           (mask & capability) == capability;
}

[[nodiscard]] constexpr bool isValidObjectAIOrderCapabilityMask(
    ObjectAIOrderCapability mask) noexcept
{
    constexpr uint8_t known = static_cast<uint8_t>(ObjectAIOrderCapability::MoveStop) |
                              static_cast<uint8_t>(ObjectAIOrderCapability::Attack) |
                              static_cast<uint8_t>(ObjectAIOrderCapability::Special);
    return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(~known)) == 0;
}

[[nodiscard]] constexpr bool isValidObjectAIOrderKind(
    ObjectAIOrderKind kind) noexcept
{
    return kind < ObjectAIOrderKind::Count;
}

[[nodiscard]] constexpr bool isValidObjectAIOrderSource(
    ObjectAIOrderSource source) noexcept
{
    return source < ObjectAIOrderSource::Count;
}

[[nodiscard]] constexpr bool isValidObjectAIOrderSystemPurpose(
    ObjectAIOrderSystemPurpose purpose) noexcept
{
    return purpose < ObjectAIOrderSystemPurpose::Count;
}

// Order kind is intentionally outside the identity.  These fields are the
// complete correlation frozen at queue observation and used to reject late
// completion/cancellation feedback after replacement.
struct ObjectAIOrderIdentity final
{
    ObjectId subject = INVALID_OBJECT_ID;
    uint64_t queueRevision = 0;
    uint64_t externalRevision = 0;
    uint64_t issuedTick = 0;
    uint32_t sourceSequence = 0;
    uint32_t sourceScriptId = 0;
    ObjectAIOrderSource source = ObjectAIOrderSource::System;
    ObjectAIOrderSystemPurpose systemPurpose =
        ObjectAIOrderSystemPurpose::Generic;
    uint32_t systemPurposeInstance = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return subject.isValid() && isValidObjectAIOrderSource(source) &&
               isValidObjectAIOrderSystemPurpose(systemPurpose);
    }

    constexpr bool operator==(const ObjectAIOrderIdentity&) const noexcept = default;
};

[[nodiscard]] constexpr AIAsyncOrderIdentity toAIAsyncOrderIdentity(
    const ObjectAIOrderIdentity& value) noexcept
{
    return {
        .subject = value.subject,
        .queueRevision = value.queueRevision,
        .externalRevision = value.externalRevision,
        .issuedTick = value.issuedTick,
        .sourceSequence = value.sourceSequence,
        .sourceScriptId = value.sourceScriptId,
        .systemPurposeInstance = value.systemPurposeInstance,
        .source = static_cast<uint8_t>(value.source),
        .systemPurpose = static_cast<uint8_t>(value.systemPurpose),
    };
}

[[nodiscard]] constexpr bool matchesAIAsyncOrderIdentity(
    const AIAsyncOrderIdentity& asynchronous,
    const ObjectAIOrderIdentity& admitted) noexcept
{
    return asynchronous.isValid() &&
           asynchronous == toAIAsyncOrderIdentity(admitted);
}

struct ObjectAIOrderAdmissionRequest final
{
    ObjectAIOrderKind kind = ObjectAIOrderKind::Invalid;
    ObjectAIOrderIdentity identity;
    bool attackMove = false;
    ObjectAIMoveRouteSubtype moveRouteSubtype =
        ObjectAIMoveRouteSubtype::Direct;
    AIWaypointHandle waypointStart;
    uint64_t waypointGraphRevision = 0;
    AITeamHandle waypointTeam;
    ObjectAITacticalAttackSubtype tacticalAttackSubtype =
        ObjectAITacticalAttackSubtype::None;
    bool allArmyHunt = false;
    bool useTeamCommonTarget = false;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        const bool tactical = kind == ObjectAIOrderKind::TacticalAttack;
        const bool waypointRoute =
            isObjectAIWaypointRouteSubtype(moveRouteSubtype);
        const bool followPathRoute = moveRouteSubtype ==
            ObjectAIMoveRouteSubtype::FollowPath;
        const bool playerFollowPathRoute = followPathRoute &&
            identity.source == ObjectAIOrderSource::Player &&
            identity.systemPurpose == ObjectAIOrderSystemPurpose::Generic;
        const bool containmentExitRoute = followPathRoute &&
            identity.source == ObjectAIOrderSource::System &&
            identity.systemPurpose ==
                ObjectAIOrderSystemPurpose::ContainmentExit;
        const bool productionExitRoute = moveRouteSubtype ==
            ObjectAIMoveRouteSubtype::FollowExitProductionPath;
        const bool moveAsideRoute = moveRouteSubtype ==
            ObjectAIMoveRouteSubtype::MoveAside;
        const bool validRoute = isValidObjectAIMoveRouteSubtype(
                moveRouteSubtype) &&
            (waypointRoute
                 ? kind == ObjectAIOrderKind::Move &&
                       (!attackMove ||
                        (moveRouteSubtype ==
                             ObjectAIMoveRouteSubtype::WaypointPathIndividuals ||
                         moveRouteSubtype ==
                             ObjectAIMoveRouteSubtype::WaypointPathTeam)) &&
                       identity.source == ObjectAIOrderSource::Script &&
                       identity.systemPurpose ==
                           ObjectAIOrderSystemPurpose::Generic &&
                       waypointStart && waypointGraphRevision != 0 &&
                       (objectAIWaypointRouteMovesAsTeam(moveRouteSubtype)
                            ? static_cast<bool>(waypointTeam)
                            : !waypointTeam)
                 : playerFollowPathRoute || productionExitRoute ||
                           containmentExitRoute || moveAsideRoute
                     ? kind == ObjectAIOrderKind::Move && !attackMove &&
                           (playerFollowPathRoute ||
                            (identity.source == ObjectAIOrderSource::System &&
                             identity.systemPurpose ==
                                 (moveAsideRoute
                                      ? ObjectAIOrderSystemPurpose::MoveAside
                                  : productionExitRoute
                                      ? ObjectAIOrderSystemPurpose::ProductionExit
                                      : ObjectAIOrderSystemPurpose::ContainmentExit))) &&
                           !waypointStart && waypointGraphRevision == 0 &&
                           !waypointTeam
                     : !waypointStart && waypointGraphRevision == 0 &&
                           !waypointTeam);
        const bool validTacticalPayload =
            tactical && isValidObjectAITacticalAttackSubtype(
                            tacticalAttackSubtype) &&
            (tacticalAttackSubtype == ObjectAITacticalAttackSubtype::Hunt ||
             (!allArmyHunt && !useTeamCommonTarget));
        return isValidObjectAIOrderKind(kind) && identity.isValid() &&
               (!attackMove || kind == ObjectAIOrderKind::Move) && validRoute &&
               (tactical
                    ? validTacticalPayload
                    : tacticalAttackSubtype ==
                              ObjectAITacticalAttackSubtype::None &&
                          !allArmyHunt && !useTeamCommonTarget);
    }

    constexpr bool operator==(const ObjectAIOrderAdmissionRequest&) const noexcept = default;
};

enum class ObjectAIOrderOwner : uint8_t
{
    None,
    ObjectAIRuntime,
    CommandIngress,
    LegacyMovement,
    LegacyCombat,
    LegacyBuilderProduction,
    LegacySpecialized,
    Unsupported,
};

struct ObjectAIOrderAdmissionContext final
{
    bool containedPassenger = false;
    bool hostProjectedAttack = false;
    bool structuralChild = false;
};

// System provenance or a specialized purpose normally wins before
// kind/capability routing, preventing a broad capability bit from stealing
// autonomous orders from their existing owner. Explicit typed movement paths
// are exceptions: they preserve their producer/lifecycle identity while using
// ObjectAI for the shared pathfinding and arrival contract. R7 additionally
// keeps attack-move under legacy ownership until both capabilities are
// explicitly enabled.
[[nodiscard]] constexpr ObjectAIOrderOwner objectAIOrderOwner(
    ObjectAIOrderKind kind, ObjectAIOrderSource source,
    ObjectAIOrderSystemPurpose purpose,
    ObjectAIOrderCapability capabilities,
    bool attackMove = false,
    ObjectAIMoveRouteSubtype moveRouteSubtype =
        ObjectAIMoveRouteSubtype::Direct,
    ObjectAITacticalAttackSubtype tacticalAttackSubtype =
        ObjectAITacticalAttackSubtype::None) noexcept
{
    if (!isValidObjectAIOrderKind(kind) ||
        !isValidObjectAIOrderSource(source) ||
        !isValidObjectAIOrderSystemPurpose(purpose) ||
        !isValidObjectAIOrderCapabilityMask(capabilities) ||
        (attackMove && kind != ObjectAIOrderKind::Move) ||
        !isValidObjectAIMoveRouteSubtype(moveRouteSubtype))
    {
        return ObjectAIOrderOwner::Unsupported;
    }
    if (moveRouteSubtype == ObjectAIMoveRouteSubtype::FollowPath ||
        moveRouteSubtype ==
            ObjectAIMoveRouteSubtype::FollowExitProductionPath ||
        moveRouteSubtype == ObjectAIMoveRouteSubtype::MoveAside ||
        moveRouteSubtype == ObjectAIMoveRouteSubtype::WanderInPlace) {
        const bool playerWaypointFollow =
            moveRouteSubtype == ObjectAIMoveRouteSubtype::FollowPath &&
            source == ObjectAIOrderSource::Player &&
            purpose == ObjectAIOrderSystemPurpose::Generic;
        const bool scriptWanderInPlace =
            moveRouteSubtype == ObjectAIMoveRouteSubtype::WanderInPlace &&
            source == ObjectAIOrderSource::Script &&
            purpose == ObjectAIOrderSystemPurpose::Generic;
        const ObjectAIOrderSystemPurpose expectedPurpose =
            moveRouteSubtype == ObjectAIMoveRouteSubtype::MoveAside
                ? ObjectAIOrderSystemPurpose::MoveAside
            : moveRouteSubtype == ObjectAIMoveRouteSubtype::FollowPath
                ? ObjectAIOrderSystemPurpose::ContainmentExit
                : ObjectAIOrderSystemPurpose::ProductionExit;
        const bool systemRoute = source == ObjectAIOrderSource::System &&
            purpose == expectedPurpose;
        if (kind != ObjectAIOrderKind::Move || attackMove ||
            (!playerWaypointFollow && !scriptWanderInPlace && !systemRoute)) {
            return ObjectAIOrderOwner::Unsupported;
        }
        return hasObjectAIOrderCapability(
                   capabilities, ObjectAIOrderCapability::MoveStop)
            ? ObjectAIOrderOwner::ObjectAIRuntime
            : ObjectAIOrderOwner::LegacySpecialized;
    }
    const bool typedRetaliation =
        kind == ObjectAIOrderKind::TacticalAttack &&
        tacticalAttackSubtype ==
            ObjectAITacticalAttackSubtype::GuardRetaliate &&
        source == ObjectAIOrderSource::System &&
            purpose == ObjectAIOrderSystemPurpose::Retaliation;
    const bool typedCommandButtonHunt =
        kind == ObjectAIOrderKind::TacticalAttack &&
        tacticalAttackSubtype == ObjectAITacticalAttackSubtype::Hunt &&
        source == ObjectAIOrderSource::System &&
        (purpose == ObjectAIOrderSystemPurpose::CommandButtonHunt ||
         purpose == ObjectAIOrderSystemPurpose::ParachuteLanding);
    const bool typedSystemAttack =
        kind == ObjectAIOrderKind::Attack &&
        source == ObjectAIOrderSource::System &&
        (purpose == ObjectAIOrderSystemPurpose::CleanupHazard ||
         purpose == ObjectAIOrderSystemPurpose::AssaultTransport ||
         purpose == ObjectAIOrderSystemPurpose::DeliverPayload ||
         purpose == ObjectAIOrderSystemPurpose::SlaveReturn ||
         purpose == ObjectAIOrderSystemPurpose::TacticalAssist ||
         purpose == ObjectAIOrderSystemPurpose::SpecialAbility ||
         purpose == ObjectAIOrderSystemPurpose::ObjectCreationAttack ||
         purpose == ObjectAIOrderSystemPurpose::StrategicAI);
    const bool typedScriptFireWeapon =
        kind == ObjectAIOrderKind::Attack &&
        source == ObjectAIOrderSource::Script &&
        purpose == ObjectAIOrderSystemPurpose::CommandButtonFireWeapon;
    // AssaultTransportAIUpdate delegates aiAttackMoveToPosition to the base
    // AIUpdateInterface after passenger egress.  Strategic planners use the
    // same typed authority when they eventually choose a position attack.
    // Both halves must transfer together or the order would move without
    // running opportunity attacks.
    const bool typedSystemAttackMove =
        kind == ObjectAIOrderKind::Move && attackMove &&
        source == ObjectAIOrderSource::System &&
        (purpose == ObjectAIOrderSystemPurpose::AssaultTransport ||
         purpose == ObjectAIOrderSystemPurpose::StrategicAI);
    const bool typedContainmentEnter =
        kind == ObjectAIOrderKind::Move && !attackMove &&
        source == ObjectAIOrderSource::System &&
        purpose == ObjectAIOrderSystemPurpose::ContainmentEnter;
    // Specialized update modules own their protocol and terminal side effect,
    // but the original implementations issue aiMoveTo*/aiEnter through
    // AIUpdateInterface.  Keep that split here: the typed purpose retains the
    // producer identity while ObjectAIRuntime remains the sole shared
    // pathfinding/motion owner.  Scenario reinforcement and intentional
    // contact are deliberately absent because their off-map/contact terminal
    // contracts still have dedicated movement consumers.
    const bool typedInheritedMove =
        kind == ObjectAIOrderKind::Move && !attackMove &&
        source == ObjectAIOrderSource::System &&
        (purpose == ObjectAIOrderSystemPurpose::CleanupHazard ||
         purpose == ObjectAIOrderSystemPurpose::AutoFindHealing ||
         purpose == ObjectAIOrderSystemPurpose::SupplyTruck ||
         purpose == ObjectAIOrderSystemPurpose::RailedTransport ||
         purpose == ObjectAIOrderSystemPurpose::AssaultTransport ||
         purpose == ObjectAIOrderSystemPurpose::PilotFindVehicle ||
         purpose == ObjectAIOrderSystemPurpose::DeliverPayload ||
         purpose == ObjectAIOrderSystemPurpose::Builder ||
         purpose == ObjectAIOrderSystemPurpose::SlaveReturn ||
         purpose == ObjectAIOrderSystemPurpose::SpecialAbility ||
         purpose == ObjectAIOrderSystemPurpose::Wander ||
         purpose == ObjectAIOrderSystemPurpose::RepairDock ||
         purpose == ObjectAIOrderSystemPurpose::ContainmentEnter ||
         purpose == ObjectAIOrderSystemPurpose::StrategicAI ||
         purpose == ObjectAIOrderSystemPurpose::ConstructionEvacuation);
    if ((source == ObjectAIOrderSource::System && !typedRetaliation &&
         !typedCommandButtonHunt &&
         !typedSystemAttack && !typedSystemAttackMove &&
         !typedContainmentEnter &&
         !typedInheritedMove) ||
        (purpose != ObjectAIOrderSystemPurpose::Generic &&
         !typedScriptFireWeapon))
    {
        if (typedRetaliation)
            return hasObjectAIOrderCapability(
                       capabilities, ObjectAIOrderCapability::Attack) &&
                   hasObjectAIOrderCapability(
                       capabilities, ObjectAIOrderCapability::MoveStop)
                ? ObjectAIOrderOwner::ObjectAIRuntime
                : ObjectAIOrderOwner::LegacyCombat;
        if (typedCommandButtonHunt)
            return hasObjectAIOrderCapability(
                       capabilities, ObjectAIOrderCapability::Attack)
                ? ObjectAIOrderOwner::ObjectAIRuntime
                : ObjectAIOrderOwner::LegacyCombat;
        if (typedSystemAttack)
            return hasObjectAIOrderCapability(
                       capabilities, ObjectAIOrderCapability::Attack)
                ? ObjectAIOrderOwner::ObjectAIRuntime
                : ObjectAIOrderOwner::LegacyCombat;
        if (typedScriptFireWeapon)
            return ObjectAIOrderOwner::LegacyCombat;
        if (typedSystemAttackMove)
            return hasObjectAIOrderCapability(
                       capabilities, ObjectAIOrderCapability::MoveStop) &&
                    hasObjectAIOrderCapability(
                       capabilities, ObjectAIOrderCapability::Attack)
                ? ObjectAIOrderOwner::ObjectAIRuntime
                : ObjectAIOrderOwner::LegacySpecialized;
        if (typedContainmentEnter || typedInheritedMove)
            return hasObjectAIOrderCapability(
                       capabilities, ObjectAIOrderCapability::MoveStop)
                ? ObjectAIOrderOwner::ObjectAIRuntime
                : ObjectAIOrderOwner::LegacySpecialized;
        return ObjectAIOrderOwner::LegacySpecialized;
    }

    switch (kind)
    {
    case ObjectAIOrderKind::Move:
        if (attackMove)
        {
            return hasObjectAIOrderCapability(
                       capabilities, ObjectAIOrderCapability::MoveStop) &&
                    hasObjectAIOrderCapability(
                        capabilities, ObjectAIOrderCapability::Attack)
                ? ObjectAIOrderOwner::ObjectAIRuntime
                : ObjectAIOrderOwner::LegacyMovement;
        }
        return hasObjectAIOrderCapability(
                   capabilities, ObjectAIOrderCapability::MoveStop)
            ? ObjectAIOrderOwner::ObjectAIRuntime
            : ObjectAIOrderOwner::LegacyMovement;
    case ObjectAIOrderKind::Stop:
        return hasObjectAIOrderCapability(
                   capabilities, ObjectAIOrderCapability::MoveStop)
            ? ObjectAIOrderOwner::ObjectAIRuntime
            : ObjectAIOrderOwner::CommandIngress;
    case ObjectAIOrderKind::Attack:
        return hasObjectAIOrderCapability(
                   capabilities, ObjectAIOrderCapability::Attack)
            ? ObjectAIOrderOwner::ObjectAIRuntime
            : ObjectAIOrderOwner::LegacyCombat;
    case ObjectAIOrderKind::TacticalAttack:
        if ((source != ObjectAIOrderSource::Script &&
             !(source == ObjectAIOrderSource::Player &&
                tacticalAttackSubtype ==
                    ObjectAITacticalAttackSubtype::Guard) &&
             !typedCommandButtonHunt &&
             !typedRetaliation) ||
            !isValidObjectAITacticalAttackSubtype(tacticalAttackSubtype))
            return ObjectAIOrderOwner::Unsupported;
        return hasObjectAIOrderCapability(
                   capabilities, ObjectAIOrderCapability::Attack) &&
               (tacticalAttackSubtype !=
                    ObjectAITacticalAttackSubtype::Guard &&
                tacticalAttackSubtype !=
                    ObjectAITacticalAttackSubtype::GuardTunnelNetwork ||
                hasObjectAIOrderCapability(
                    capabilities, ObjectAIOrderCapability::MoveStop))
            ? ObjectAIOrderOwner::ObjectAIRuntime
            : ObjectAIOrderOwner::LegacyCombat;
    case ObjectAIOrderKind::Build:
        return ObjectAIOrderOwner::LegacyBuilderProduction;
    case ObjectAIOrderKind::SpecialPower:
        return ObjectAIOrderOwner::LegacySpecialized;
    case ObjectAIOrderKind::CommandButton:
    case ObjectAIOrderKind::Count:
    case ObjectAIOrderKind::Invalid:
        return ObjectAIOrderOwner::Unsupported;
    }
    return ObjectAIOrderOwner::Unsupported;
}

static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Attack, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::CleanupHazard,
    ObjectAIOrderCapability::Attack) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Attack, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::AssaultTransport,
    ObjectAIOrderCapability::Attack) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Attack, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::ObjectCreationAttack,
    ObjectAIOrderCapability::Attack) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Attack, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::StrategicAI,
    ObjectAIOrderCapability::Attack) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Move, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::AssaultTransport,
    ObjectAIOrderCapability::MoveStop |
        ObjectAIOrderCapability::Attack,
    true) == ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Move, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::Builder,
    ObjectAIOrderCapability::MoveStop) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Move, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::StrategicAI,
    ObjectAIOrderCapability::MoveStop) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Move, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::ContainmentEnter,
    ObjectAIOrderCapability::MoveStop) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Move, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::SupplyTruck,
    ObjectAIOrderCapability::MoveStop) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Move, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::SpecialAbility,
    ObjectAIOrderCapability::MoveStop) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Move, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::ConstructionEvacuation,
    ObjectAIOrderCapability::MoveStop) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::TacticalAttack, ObjectAIOrderSource::Player,
    ObjectAIOrderSystemPurpose::Generic,
    ObjectAIOrderCapability::MoveStop | ObjectAIOrderCapability::Attack,
    false, ObjectAIMoveRouteSubtype::Direct,
    ObjectAITacticalAttackSubtype::Guard) ==
    ObjectAIOrderOwner::ObjectAIRuntime);
static_assert(objectAIOrderOwner(
    ObjectAIOrderKind::Attack, ObjectAIOrderSource::System,
    ObjectAIOrderSystemPurpose::Generic,
    ObjectAIOrderCapability::Attack) ==
    ObjectAIOrderOwner::LegacySpecialized);

[[nodiscard]] constexpr ObjectAIOrderOwner objectAIOrderOwner(
    const ObjectAIOrderAdmissionRequest& request,
    ObjectAIOrderCapability capabilities,
    const ObjectAIOrderAdmissionContext& context) noexcept
{
    if (!request.isValid())
        return ObjectAIOrderOwner::Unsupported;
    if (context.containedPassenger || context.hostProjectedAttack ||
        context.structuralChild)
    {
        return ObjectAIOrderOwner::LegacySpecialized;
    }
    return objectAIOrderOwner(
        request.kind, request.identity.source,
        request.identity.systemPurpose, capabilities, request.attackMove,
        request.moveRouteSubtype, request.tacticalAttackSubtype);
}

[[nodiscard]] constexpr ObjectAIOrderOwner objectAIOrderOwner(
    const ObjectAIOrderAdmissionRequest& request,
    ObjectAIOrderCapability capabilities) noexcept
{
    return objectAIOrderOwner(
        request.kind, request.identity.source,
        request.identity.systemPurpose, capabilities, request.attackMove,
        request.moveRouteSubtype, request.tacticalAttackSubtype);
}

struct ObjectAIOrderSlotHandle final
{
    static constexpr uint32_t InvalidSlot =
        std::numeric_limits<uint32_t>::max();

    uint32_t slot = InvalidSlot;
    uint32_t generation = 0;
    ObjectId subject = INVALID_OBJECT_ID;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return slot != InvalidSlot && generation != 0 && subject.isValid();
    }

    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr bool operator==(const ObjectAIOrderSlotHandle&) const noexcept = default;
};

enum class ObjectAIOrderAdmissionStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidCapacity,
    InvalidSlot,
    InvalidSubject,
    DuplicateSubject,
    SlotAlreadyBound,
    SlotNotBound,
    StaleGeneration,
    SubjectMismatch,
    InvalidCapabilityMask,
    InvalidOrderKind,
    InvalidOrderSource,
    InvalidSystemPurpose,
    NotOwnedByObjectAI,
    UnsupportedOrder,
    ActiveOrderExists,
    NoActiveOrder,
    StaleIdentity,
    StaleQueueRevision,
    StaleExternalRevision,
    ActiveOrderWouldLoseOwnership,
    CompletionOutputCapacityExceeded,
    InvalidSnapshotSchema,
    SnapshotSizeMismatch,
    InvalidSnapshot,
};

enum class ObjectAIOrderAdmissionAction : uint8_t
{
    None,
    Bound,
    Released,
    CapabilitiesChanged,
    Admitted,
    Replaced,
    Cancelled,
    ExternalRevisionSynchronized,
    SynchronousStop,
    CompletedSuccess,
    CompletedCancelled,
    CompletedFailed,
    Restored,
};

enum class ObjectAIOrderCompletion : uint8_t
{
    Success,
    Cancelled,
    Failed,
};

struct ObjectAIOrderAdmissionResult final
{
    ObjectAIOrderAdmissionStatus status =
        ObjectAIOrderAdmissionStatus::Success;
    ObjectAIOrderAdmissionAction action =
        ObjectAIOrderAdmissionAction::None;
    ObjectAIOrderOwner owner = ObjectAIOrderOwner::None;
    ObjectAIOrderSlotHandle handle;
    ObjectAIOrderAdmissionRequest previousOrder;
    ObjectAIOrderAdmissionRequest currentOrder;
    bool hadPreviousOrder = false;
    bool hasCurrentOrder = false;

    [[nodiscard]] constexpr bool succeeded() const noexcept
    {
        return status == ObjectAIOrderAdmissionStatus::Success;
    }
};

struct ObjectAIOrderAdmissionSlotView final
{
    ObjectAIOrderSlotHandle handle;
    ObjectAIOrderCapability capabilities = ObjectAIOrderCapability::None;
    uint64_t observedQueueRevision = 0;
    uint64_t observedExternalRevision = 0;
    ObjectAIOrderAdmissionRequest historicalOrder;
    bool bound = false;
    bool active = false;
    bool hasHistory = false;
};

struct ObjectAIOrderAdmissionSlotSnapshot final
{
    ObjectId subject = INVALID_OBJECT_ID;
    uint32_t generation = 1;
    ObjectAIOrderCapability capabilities = ObjectAIOrderCapability::None;
    uint64_t observedQueueRevision = 0;
    uint64_t observedExternalRevision = 0;
    ObjectAIOrderAdmissionRequest historicalOrder;
    bool bound = false;
    bool active = false;
    bool hasHistory = false;
};

struct ObjectAIOrderAdmissionSnapshot final
{
    static constexpr uint32_t SchemaVersion = 8;

    uint32_t schemaVersion = SchemaVersion;
    container::Vector<ObjectAIOrderAdmissionSlotSnapshot> slots;
};

// Fixed-capacity, slot-aligned SoA admission state.  initialize/restore and
// snapshot capture are structural operations; all bind/admit/replace/cancel
// and observation calls are allocation-free and never mutate an ECS queue or
// any gameplay component.
class ObjectAIOrderAdmissionStorage final
{
public:
    [[nodiscard]] ObjectAIOrderAdmissionStatus initialize(size_t capacity);

    [[nodiscard]] ObjectAIOrderAdmissionResult bind(
        uint32_t slot, ObjectId subject,
        ObjectAIOrderCapability capabilities = ObjectAIOrderCapability::None,
        uint64_t observedQueueRevision = 0,
        uint64_t observedExternalRevision = 0) noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult release(
        ObjectAIOrderSlotHandle handle) noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult setCapabilities(
        ObjectAIOrderSlotHandle handle,
        ObjectAIOrderCapability capabilities) noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult admit(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderAdmissionRequest& request) noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult replace(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderIdentity& expectedIdentity,
        const ObjectAIOrderAdmissionRequest& replacement) noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult cancel(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderIdentity& expectedIdentity) noexcept;

    // Completion is a two-phase transaction boundary. The caller preflights
    // any completion command/event sink and passes outputCapacityAvailable;
    // failure leaves the admitted identity active and cannot pop an ECS order
    // or commit a state transition.
    [[nodiscard]] ObjectAIOrderAdmissionResult complete(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAIOrderCompletion completion,
        bool outputCapacityAvailable) noexcept;

    // Stop never needs to survive as a queue head.  Command ingress clears
    // the ECS queue and increments externalRevision; this method observes that
    // value, invalidates the admitted identity and leaves gameplay untouched.
    [[nodiscard]] ObjectAIOrderAdmissionResult synchronizeExternalRevision(
        ObjectAIOrderSlotHandle handle,
        uint64_t externalRevision) noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionStatus activeOrder(
        ObjectAIOrderSlotHandle handle,
        ObjectAIOrderAdmissionRequest& output) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionStatus readSlot(
        uint32_t slot, ObjectAIOrderAdmissionSlotView& output) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionStatus captureSnapshot(
        ObjectAIOrderAdmissionSnapshot& output) const;

    [[nodiscard]] ObjectAIOrderAdmissionResult restoreSnapshot(
        const ObjectAIOrderAdmissionSnapshot& snapshot);

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] size_t capacity() const noexcept;
    [[nodiscard]] size_t boundCount() const noexcept;

    [[nodiscard]] bool bound(uint32_t slot) const noexcept;

    [[nodiscard]] bool active(uint32_t slot) const noexcept;

    [[nodiscard]] uint32_t generation(uint32_t slot) const noexcept;

    [[nodiscard]] ObjectAIOrderSlotHandle handle(uint32_t slot) const noexcept;

    [[nodiscard]] container::Span<const ObjectId> subjects() const noexcept;

    [[nodiscard]] container::Span<const uint8_t> boundMask() const noexcept;

    [[nodiscard]] container::Span<const uint8_t> activeMask() const noexcept;

private:
    [[nodiscard]] static ObjectAIOrderAdmissionResult& fail(
        ObjectAIOrderAdmissionResult& result,
        ObjectAIOrderAdmissionStatus status) noexcept;

    [[nodiscard]] bool validSlot(uint32_t slot) const noexcept;

    [[nodiscard]] ObjectAIOrderSlotHandle makeHandle(uint32_t slot) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionStatus validateHandle(
        ObjectAIOrderSlotHandle handle) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionStatus validateMutation(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderAdmissionRequest& request,
        ObjectAIOrderAdmissionResult& result) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionStatus validateNewerIdentity(
        uint32_t slot, const ObjectAIOrderIdentity& identity,
        bool synchronousStop) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionResult applySynchronousStop(
        ObjectAIOrderAdmissionResult& result, uint32_t slot,
        const ObjectAIOrderAdmissionRequest& request) noexcept;

    void storeOrder(
        uint32_t slot, const ObjectAIOrderAdmissionRequest& request) noexcept;

    void clearOrder(uint32_t slot) noexcept;

    [[nodiscard]] ObjectAIOrderIdentity loadIdentity(uint32_t slot) const noexcept;

    [[nodiscard]] ObjectAIOrderAdmissionRequest loadOrder(
        uint32_t slot) const noexcept;

    void setPrevious(
        ObjectAIOrderAdmissionResult& result, uint32_t slot) const noexcept;

    void setCurrent(
        ObjectAIOrderAdmissionResult& result, uint32_t slot) const noexcept;

    [[nodiscard]] static bool canonicalUnboundSnapshot(
        const ObjectAIOrderAdmissionSlotSnapshot& value) noexcept;

    [[nodiscard]] static bool validateSnapshot(
        const ObjectAIOrderAdmissionSnapshot& snapshot) noexcept;

    container::Vector<ObjectId> m_subjects;
    container::Vector<uint32_t> m_generations;
    container::Vector<ObjectAIOrderCapability> m_capabilityMasks;
    container::Vector<uint64_t> m_observedQueueRevisions;
    container::Vector<uint64_t> m_observedExternalRevisions;
    container::Vector<uint64_t> m_orderQueueRevisions;
    container::Vector<uint64_t> m_orderExternalRevisions;
    container::Vector<uint64_t> m_orderIssuedTicks;
    container::Vector<uint32_t> m_orderSourceSequences;
    container::Vector<uint32_t> m_orderSourceScriptIds;
    container::Vector<uint32_t> m_orderPurposeInstances;
    container::Vector<ObjectAIOrderKind> m_orderKinds;
    container::Vector<uint8_t> m_orderAttackMoveMasks;
    container::Vector<ObjectAIMoveRouteSubtype> m_orderMoveRouteSubtypes;
    container::Vector<AIWaypointHandle> m_orderWaypointStarts;
    container::Vector<uint64_t> m_orderWaypointGraphRevisions;
    container::Vector<AITeamHandle> m_orderWaypointTeams;
    container::Vector<ObjectAITacticalAttackSubtype>
        m_orderTacticalAttackSubtypes;
    container::Vector<uint8_t> m_orderAllArmyHuntMasks;
    container::Vector<uint8_t> m_orderUseTeamCommonTargetMasks;
    container::Vector<ObjectAIOrderSource> m_orderSources;
    container::Vector<ObjectAIOrderSystemPurpose> m_orderPurposes;
    container::Vector<uint8_t> m_boundMasks;
    container::Vector<uint8_t> m_activeMasks;
    container::Vector<uint8_t> m_historyMasks;
    size_t m_boundCount = 0;
    bool m_initialized = false;
};

} // namespace engine::ai
