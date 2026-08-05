#pragma once

#include "core/container/container_types.h"

#include <cstdint>
#include <limits>
#include "core/ecs/registry.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include "game/object/plan/economy/ObjectUpgradePlanTypes.h"
namespace engine
{

struct ObjectSimulationRules;
class ObjectLifecycle;
class ObjectOwnershipIndex;
class PlayerRegistry;
class ScienceCatalog;
class GameContentSnapshot;
class SimulationRandom;
class ObjectSpatialIndex;
namespace navigation { class NavigationSystem; }
struct ObjectReplacementInvocation;
struct ObjectUpgradeFxInvocation;

// Cold-path value sink used only when an UpgradeMux edge materializes a
// gameplay consequence. It avoids giving ObjectUpgradeSystem ownership of a
// GameSession while preserving one central OCL queue and monotonic sequence.
class ObjectUpgradeEffectSink {
public:
    virtual ~ObjectUpgradeEffectSink() = default;
    virtual void queueObjectCreationListInvocation(
        ObjectCreationListInvocation invocation) = 0;
    [[nodiscard]] virtual uint64_t
    reserveGameplaySubmissionOrdinal() noexcept = 0;
    virtual void queueObjectReplacementInvocation(
        ObjectReplacementInvocation invocation) = 0;
    virtual void queueObjectUpgradeFxInvocation(
        ObjectUpgradeFxInvocation invocation) = 0;
};

// Detached structural request emitted by ReplaceObjectUpgrade. The source
// entity remains owned by ObjectLifecycle until GameSession consumes this
// value, so UpgradeMux never mutates EnTT storage while walking its runtimes.
struct ObjectReplacementInvocation final {
    container::String replacementTemplate;
    ObjectId source = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    LogicFixedVec3 position;
    ObjectPhysicsComponent::Scalar orientationRadians{};
    ObjectPhysicsComponent::Scalar pitchRadians{};
    ObjectPhysicsComponent::Scalar rollRadians{};
    bool sourceOwnsFullAttitude = false;
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectUpgradeFxInvocation final {
    container::String fxList;
    ObjectId source = INVALID_OBJECT_ID;
    LogicFixedVec3 position;
    ObjectPhysicsComponent::Scalar rollRadians{};
    ObjectPhysicsComponent::Scalar pitchRadians{};
    ObjectPhysicsComponent::Scalar yawRadians{};
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

// Sparse read-only query supplied by the composition root for target policies
// whose authored catalog is session-owned. ObjectSimulation retains neither
// the opaque context nor the callback beyond the synchronous confirmed phase.
struct ObjectAITargetPriorityQuery final {
    const void* context = nullptr;
    int32_t (*resolve)(const void*, ObjectId, ecs::entity) noexcept = nullptr;

    [[nodiscard]] int32_t operator()(
        ObjectId subject, ecs::entity target) const noexcept {
        return context && resolve ? resolve(context, subject, target) : 1;
    }
};

// Capabilities available to one confirmed UpgradeMux execution. Keeping the
// mutable player authority and immutable content catalog explicit avoids a
// hidden GameSession/global-store dependency inside the value-only upgrade
// system. Future player-scoped upgrade operations can extend this context
// without restoring the legacy Object/Player virtual-module graph.
struct ObjectUpgradeExecutionContext final
{
    PlayerRegistry* players = nullptr;
    const ScienceCatalog* scienceCatalog = nullptr;
    const GameContentSnapshot* content = nullptr;
    ObjectAITargetPriorityQuery aiTargetPriority;
    int32_t rankLevelLimit = std::numeric_limits<int32_t>::max();
    SimulationRandom* random = nullptr;
    const game::terrain::TerrainLogic* terrain = nullptr;
    // Completed previous structural boundary, used only as a deterministic
    // broad phase. Consumers still perform fixed-point exact tests against
    // current authoritative component positions.
    const ObjectSpatialIndex* spatialIndex = nullptr;
    navigation::NavigationSystem* navigation = nullptr;
    const game::terrain::MapVisibilitySnapshot* mapVisibility = nullptr;
    // Mutable authority is explicit and used only by synchronous authored
    // transactions whose ZH order precedes damage (for example Particle
    // Uplink scorch reveal pulses). Read-only systems continue using the
    // completed snapshot above.
    game::terrain::MapVisibilityAuthority* mapVisibilityAuthority = nullptr;
    ObjectUpgradeEffectSink* effects = nullptr;
};

// Authoritative equivalent of Object::getCommandSetString(). A non-empty
// per-object override wins; an empty override deliberately falls back to the
// immutable ThingTemplate CommandSet, matching the source getter.
[[nodiscard]] container::StringView effectiveObjectCommandSetName(
    const ecs::registry& registry, ecs::entity entity) noexcept;

struct ObjectUpgradeRuntime final
{
    bool activated = false;
};

// Sparse active-shroud source. The radius is gameplay-authoritative Q32.32;
// MapVisibilityAuthority receives only a float projection when rasterizing the
// confirmed frame into legacy terrain cells.
struct ObjectActiveShroudComponent final {
    math::q32_32 radius{};
    uint64_t activatedTick = 0;
};

struct ObjectRadarProviderComponent final {
    // RefCode Player::addRadar counts every activated RadarUpgrade module,
    // not merely every host object. Keep both totals so multiple authored
    // providers on one object survive capture/disable/delete reprojection.
    uint32_t providerCount = 0;
    uint32_t disableProofProviderCount = 0;
    uint64_t activatedTick = 0;
};

struct ObjectRadarUpdateComponent final {
    container::SharedPtr<const game::ObjectRadarUpdatePlan> plan;
    uint64_t extensionCompleteTick = 0;
    bool active = false;
    bool extensionComplete = false;
};

// Object-scoped UpgradeTemplate completion is not player research, but ZH
// assigns player and object upgrades from the same sealed 512-bit identity
// space. Keeping the exact mask on the object makes every confirmed lookup a
// bit test; authored names are resolved only at content/input boundaries.
struct ObjectUpgradeInventoryComponent final
{
    UpgradeMask completed;
    uint64_t revision = 0;
};

struct ObjectUpgradeComponent final
{
    container::SharedPtr<const game::ObjectUpgradePlan> plan;
    container::Vector<ObjectUpgradeRuntime> instances;
};

enum class ObjectPowerPlantRodState : uint8_t
{
    Retracted,
    Extending,
    Extended,
};

// A source mask avoids baking today's single PowerPlantUpgrade owner into the
// visual state. OverchargeBehavior can later keep rods extended concurrently
// by setting its own bit, without undoing a researched plant upgrade.
enum class ObjectPowerPlantExtensionSource : uint8_t
{
    PowerPlantUpgrade,
    Overcharge,
    Count,
};

using ObjectPowerPlantExtensionSourceMask = uint8_t;

[[nodiscard]] constexpr ObjectPowerPlantExtensionSourceMask objectPowerPlantExtensionSourceBit(
    ObjectPowerPlantExtensionSource source) noexcept
{
    return source >= ObjectPowerPlantExtensionSource::Count
               ? ObjectPowerPlantExtensionSourceMask{}
               : static_cast<ObjectPowerPlantExtensionSourceMask>(ObjectPowerPlantExtensionSourceMask{1}
                                                                  << static_cast<uint8_t>(source));
}

struct ObjectPowerPlantComponent final
{
    uint32_t rodsExtendMilliseconds = 0;
    uint64_t extensionCompleteTick = 0;
    ObjectPowerPlantExtensionSourceMask extensionSources = 0;
    ObjectPowerPlantRodState state = ObjectPowerPlantRodState::Retracted;
};

// Value-only replacement for the legacy UpgradeModule / UpgradeMux dispatch.
// It is invoked only at object assembly and actual upgrade-completion events;
// there is no world-wide per-frame polling of player technology revisions.
class ObjectUpgradeSystem final
{
public:
    // Structural assembly only. The source engine runs its first general
    // UpgradeMux evaluation after CreateModule::onCreate, so materialization
    // must not activate owner facts prematurely.
    void materializeObject(ecs::registry& registry,
                           ecs::entity entity) const;

    void onPlayerUpgradeCompleted(ecs::registry& registry,
                                  ObjectLifecycle& lifecycle,
                                  const ObjectOwnershipIndex& ownership,
                                  PlayerId player,
                                  const UpgradeMask& completedUpgrades,
                                  const ObjectSimulationRules& rules,
                                  uint64_t confirmedTick,
                                  ObjectUpgradeExecutionContext context = {}) const;

    // RefCode re-runs UpgradeMux after an ownership change. Existing
    // activations stay sticky; this only lets a previously dormant rule see
    // the new owner's already-completed player upgrades.
    void onObjectOwnerChanged(ecs::registry& registry,
                              ObjectLifecycle& lifecycle,
                              ObjectId object,
                              const UpgradeMask& newOwnerCompletedUpgrades,
                              const ObjectSimulationRules& rules,
                              uint64_t confirmedTick) const;

    // `affectedByUpgrade` equivalent used by OBJECT_UPGRADE queue admission.
    // It tests the prospective local completion against the current combined
    // player/object facts without mutating either inventory.  Only typed ECS
    // rules are considered, so partially migrated content is rejected rather
    // than accepting payment for a no-op.
    [[nodiscard]] bool canReceiveObjectUpgrade(const ecs::registry& registry,
                                               ecs::entity entity,
                                               const UpgradeMask& ownerCompletedUpgrades,
                                               UpgradeContentId prospectiveUpgrade) const noexcept;

    [[nodiscard]] bool hasObjectUpgrade(const ecs::registry& registry,
                                        ecs::entity entity,
                                        UpgradeContentId upgrade) const noexcept;

    // Commits one OBJECT UpgradeTemplate to an individual receiver then runs
    // one author-order UpgradeMux pass.  It is deliberately separate from
    // PlayerRegistry: OBJECT completion never creates a global in-progress or
    // completed technology entry.
    [[nodiscard]] bool completeObjectUpgrade(ecs::registry& registry,
                                             ObjectLifecycle& lifecycle,
                                             ObjectId object,
                                             UpgradeContentId upgrade,
                                             const UpgradeMask& ownerCompletedUpgrades,
                                             const ObjectSimulationRules& rules,
                                             uint64_t confirmedTick,
                                             ObjectUpgradeExecutionContext context = {}) const;

    // Object::removeUpgrade clears only the local OBJECT bit and resets muxes
    // triggered by that bit; it never rolls back implementation side effects
    // and never touches PlayerRegistry technology. Pending-destroy producers
    // remain valid until lifecycle flush, matching the deferred source object.
    [[nodiscard]] bool removeObjectUpgrade(ecs::registry& registry,
                                           const ObjectLifecycle& lifecycle,
                                           ObjectId object,
                                           UpgradeContentId upgrade) const noexcept;

    // Narrow Object::updateUpgradeModules equivalent. Veterancy uses this
    // once before granting its synthetic level upgrade and giveUpgrade runs
    // it again afterwards, even when that level bit was already accumulated.
    void reevaluateObjectUpgrades(ecs::registry& registry,
                                  ObjectLifecycle& lifecycle,
                                  ObjectId object,
                                  const UpgradeMask& ownerCompletedUpgrades,
                                  const ObjectSimulationRules& rules,
                                  uint64_t confirmedTick,
                                  ObjectUpgradeExecutionContext context = {}) const;

    // The source rechecks upgrade modules immediately after each path that
    // clears UNDER_CONSTRUCTION.  Construction systems call this narrow hook
    // after their status transition; it does not poll all objects every tick.
    void onConstructionCompleted(ecs::registry& registry,
                                 ObjectLifecycle& lifecycle,
                                 ObjectId object,
                                 const UpgradeMask& ownerCompletedUpgrades,
                                 const ObjectSimulationRules& rules,
                                 uint64_t confirmedTick,
                                 ObjectUpgradeExecutionContext context = {}) const;

    // Advances only pending PowerPlantUpdate rod transitions in stable
    // ObjectId order. The source update is a sleep/wake callback, so this
    // pass is normally empty and never examines player upgrade lists.
    void update(ecs::registry& registry,
                ObjectLifecycle& lifecycle,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick) const;
    void updateRadarProviders(
        const ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        PlayerRegistry& players,
        uint64_t confirmedTick) const;

private:
    static void activateEligible(ecs::registry& registry,
                                 ecs::entity entity,
                                 const UpgradeMask& ownerCompletedUpgrades,
                                 const ObjectSimulationRules& rules,
                                 uint64_t confirmedTick,
                                 ObjectUpgradeExecutionContext context);
};

} // namespace engine
