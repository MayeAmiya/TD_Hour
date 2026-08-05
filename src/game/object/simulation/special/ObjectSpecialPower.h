#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <limits>

#include "game/object/plan/special/ObjectSpecialPowerPlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class ObjectSpatialIndex;
class PlayerRegistry;
class SimulationRandom;
namespace navigation { class NavigationSystem; }
struct ObjectSimulationRules;
class ObjectSpyVisionSystem;
class ObjectCleanupHazardSystem;
class ObjectTacticalSystem;
struct ObjectSpecialAbilityEffectRequest;

struct ObjectSpecialPowerRuntime final {
    SpecialPowerContentId content = INVALID_SPECIAL_POWER_CONTENT_ID;
    uint64_t readyTick = 0;
    uint64_t activationSequence = 0;
    uint64_t pauseStartedTick = 0;
    uint32_t pausedCount = 0;
};

struct ObjectSpecialPowerComponent final {
    container::SharedPtr<const game::ObjectSpecialPowerPlan> plan;
    container::Vector<ObjectSpecialPowerRuntime> instances;
};

// DetectionTime is object-local rather than player diplomacy. While present,
// the defector sees every target as Neutral and other objects see it as an
// Ally, matching RefCode's asymmetric Object::getRelationship override.
struct ObjectUndetectedDefectorComponent final {
    uint64_t detectionEndTick = 0;
};

struct ObjectDefectionRequest final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    PlayerId newOwner = INVALID_PLAYER_ID;
    uint64_t detectionDurationTicks = 0;
    uint32_t authoredOrder = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

// Structural consequence of a capability reacting to SpecialPower activation.
// The simulation publishes only stable/value data; GameSession remains the
// sole object factory and resolves the template/team transaction.
enum class ObjectSpecialPowerSpawnCompletionKind : uint8_t {
    None,
    SpecialAbility,
    AirfieldCapabilityChild,
};

struct ObjectSpecialPowerSpawnRequest final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    container::String objectTemplate;
    container::String attachToBone;
    LogicFixedVec3 position;
    LogicFixedVec3 effectPosition;
    LogicFixedVec3 targetPosition;
    math::q32_32 yawRadians{};
    // RefCode SpecialPowerModule::createViewObject. A non-zero range marks
    // this request as the superweapon fog-reveal helper: GameSession applies
    // the shroud-clearing override on the spawned object and re-arms its
    // DeletionUpdate deadline to viewObjectLifetimeFrames. Both stay in the
    // deterministic representations the simulation already owns (Q32.32
    // distance, logic frames) so no float or wall clock enters the request.
    math::q32_32 shroudClearingRange{};
    uint32_t viewObjectLifetimeFrames = 0;
    SpecialPowerContentId specialPower =
        INVALID_SPECIAL_POWER_CONTENT_ID;
    ObjectId replacedObject = INVALID_OBJECT_ID;
    ObjectSpecialPowerSpawnCompletionKind completion =
        ObjectSpecialPowerSpawnCompletionKind::None;
    uint32_t specialAbilityRuleIndex = 0;
    uint32_t capabilityRuleIndex = 0;
    uint32_t containmentRuleIndex =
        std::numeric_limits<uint32_t>::max();
    uint32_t authoredOrder = 0;
    uint64_t activationSequence = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
    bool hasEffectPosition = false;
    bool attachStickyBomb = false;
    bool projectPreferredHeight = false;
    bool markAirborne = false;
    bool issueSpecialPowerOrder = false;
    bool attachToSourceContainment = false;
};

[[nodiscard]] PlayerRelationship relationshipBetweenObjects(
    const ecs::registry& registry,
    const PlayerRegistry& players,
    ecs::entity source,
    ecs::entity target) noexcept;

[[nodiscard]] PlayerRelationship relationshipBetweenPlayerAndObject(
    const ecs::registry& registry,
    const PlayerRegistry& players,
    PlayerId source,
    ecs::entity target) noexcept;

enum class ObjectSpecialPowerExecutionStatus : uint8_t {
    Activated,
    Approaching,
    NotReady,
    Disabled,
    MissingScience,
    MissingDefinition,
    UnsupportedEffect,
    MissingUpdate,
    ScriptOnly,
    InvalidObject,
};

struct ObjectSpecialPowerExecutionEvent final {
    ObjectId source = INVALID_OBJECT_ID;
    PlayerId player = INVALID_PLAYER_ID;
    SpecialPowerContentId content = INVALID_SPECIAL_POWER_CONTENT_ID;
    game::ObjectSpecialPowerKind kind = game::ObjectSpecialPowerKind::Unsupported;
    ObjectSpecialPowerExecutionStatus status =
        ObjectSpecialPowerExecutionStatus::InvalidObject;
    uint64_t confirmedTick = 0;
    uint32_t sourceSequence = 0;
    ObjectOrderSource commandSource = ObjectOrderSource::System;
    uint64_t readyTick = 0;
    uint64_t durationTicks = 0;
    int64_t moneyAmount = 0;
    LogicFixedVec3 targetPosition{};
    bool hasTargetPosition = false;
    // Selected same-template Update module. Spectre deployment may author
    // level 3/2/1 modules in priority order; the Airfield capability must emit
    // from this exact science-admitted rule rather than repeat a name lookup.
    uint32_t updateAuthoredOrder = UINT32_MAX;
    // Command acceptance and the RefCode markSpecialPowerTriggered boundary
    // differ when UpdateModuleStartsAttack defers recharge to Preparation.
    bool scriptTriggered = true;
};

// Sparse, value-only replacement for the legacy SpecialPowerModule base.
// Effect-specific systems remain separate consumers; this system owns only
// template identity, science admission, recharge and shared-sync semantics.
class ObjectSpecialPowerSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot& content,
                          const ObjectSimulationRules& rules,
                          uint64_t confirmedTick) const;

    [[nodiscard]] bool restartAllRecharge(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const GameContentSnapshot& content,
        const ObjectSimulationRules& rules,
        uint64_t confirmedTick) const;

    // Prefer ContentId on hot paths. The StringView overload resolves the
    // catalog once and forwards so call sites that still hold only a name
    // (diagnostics / script bridges) keep working.
    [[nodiscard]] bool restartRecharge(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, SpecialPowerContentId specialPower,
        const GameContentSnapshot& content,
        const ObjectSimulationRules& rules,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool restartRecharge(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, container::StringView specialPowerTemplate,
        const GameContentSnapshot& content,
        const ObjectSimulationRules& rules,
        uint64_t confirmedTick) const;

    void onBuildCompleted(ecs::registry& registry,
                          const ObjectLifecycle& lifecycle,
                          ObjectId object,
                          const GameContentSnapshot& content,
                          const ObjectSimulationRules& rules,
                          uint64_t confirmedTick) const;

    // Projects passive SpecialPower providers into sticky per-player state.
    // This sparse scan also covers science grants and ownership transfer
    // without coupling PlayerRegistry to object module storage.
    void updatePassivePlayerEffects(
        const ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        PlayerRegistry& players,
        const GameContentSnapshot& content) const;

    void updateDefectionDetection(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        uint64_t confirmedTick) const;

    // Consumes only SpecialPower queue heads, in stable ObjectId order. Every
    // consumed intent produces a value event, including deterministic
    // rejection, so malformed/unsupported content cannot wedge later orders.
    void consumeOrders(ecs::registry& registry,
                       const ObjectLifecycle& lifecycle,
                       PlayerRegistry& players,
                       const GameContentSnapshot& content,
                       const ObjectSimulationRules& rules,
                       ObjectSpyVisionSystem& spyVision,
                       ObjectCleanupHazardSystem& cleanupHazard,
                       ObjectTacticalSystem& tactical,
                       const game::terrain::TerrainLogic& terrain,
                       SimulationRandom* random,
                       const ObjectSpatialIndex* spatialIndex,
                       const navigation::NavigationSystem* navigation,
                       const game::terrain::MapVisibilitySnapshot* visibility,
                       uint64_t confirmedTick,
                       uint64_t& nextEmissionSequence,
                       container::Vector<ObjectCreationListInvocation>&
                           objectCreationListInvocations,
                       container::Vector<ObjectDefectionRequest>&
                           defectionRequests,
                       container::Vector<ObjectSpecialPowerSpawnRequest>&
                           objectSpawnRequests,
                       container::Vector<ObjectSpecialAbilityEffectRequest>&
                           specialAbilityEffectRequests,
                       container::Vector<ObjectSpecialPowerExecutionEvent>& events) const;

private:
    void synchronizeSharedReadyTick(
        ecs::registry& registry, PlayerId owner,
        SpecialPowerContentId content, uint64_t readyTick) const;
};

} // namespace engine
