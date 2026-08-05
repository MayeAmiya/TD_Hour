#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "core/ecs/ObjectId.h"
#include "game/object/contracts/ObjectFixedGeometryTypes.h"
#include "math/fixed/q32_32.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "game/object/plan/economy/ObjectBuilderPlanTypes.h"
namespace engine {

class PlayerRegistry;
class GameContentSnapshot;
namespace navigation {
class NavigationSystem;
}

class ObjectLifecycle;
struct ObjectDamageRequest;
struct ObjectSimulationRules;

enum class ObjectBuilderTaskKind : uint8_t {
    None,
    Build,
    Repair,
    Fortify,
};

enum class ObjectBuilderPhase : uint8_t {
    Idle,
    Approaching,
    Building,
    Repairing,
    Leaving,
};

struct ObjectBuilderTask final {
    ObjectBuilderTaskKind kind = ObjectBuilderTaskKind::None;
    ObjectId target = INVALID_OBJECT_ID;
    uint64_t issuedTick = 0;
    uint32_t sourceSequence = 0;
    // ActionManager's visibility admission belongs to the repair command,
    // not to whichever task happens to be active when the command is read.
    // Keeping it on the slot lets a newer Build task temporarily pre-empt a
    // Repair task without losing the original player-view contract.
    bool requireClearRepairTarget = false;
};

// Shared fixed-point construction approach rule. Local route orchestration
// and confirmed Build authority must target and accept the same annulus;
// otherwise a dozer is first driven into the future footprint and authority
// immediately sends it back out before construction can begin.
struct ObjectBuilderApproachResult final {
    LogicFixedVec3 target{};
    math::q32_32 minimumDistance{};
    math::q32_32 maximumDistance{};
    bool arrived = false;
};

[[nodiscard]] ObjectBuilderApproachResult objectBuilderApproach(
    const LogicFixedVec3& current,
    const LogicFixedVec3& constructionCenter,
    math::q32_32 builderRadius,
    math::q32_32 productRadius,
    math::q32_32 closeEnough) noexcept;

struct ObjectBuilderRuntime final {
    static constexpr size_t TaskSlotCount = 3;

    // RefCode stores one independent pending command for Build, Repair and
    // Fortify, then selects the most recently issued slot. `current` and
    // `previous` remain as compatibility projections for existing callers
    // and focused probes; taskSlots is the pending-command authority.
    std::array<ObjectBuilderTask, TaskSlotCount> taskSlots{};
    ObjectBuilderTask current;
    ObjectBuilderTask previous;
    // A direct repair retarget can replace the current task before the next
    // confirmed update. Preserve the old bridge edge as a value so the
    // terrain-owned scaffold generation is still torn down exactly once.
    ObjectId pendingScaffoldRemovalBridge = INVALID_OBJECT_ID;
    ObjectId pendingScaffoldRemovalTower = INVALID_OBJECT_ID;
    uint32_t pendingScaffoldRemovalSequence = 0;
    uint64_t observedExternalOrderRevision = 0;
    uint64_t idleSinceTick = 0;
    uint64_t nextBoredScanTick = 0;
    uint64_t revision = 0;
    uint32_t nextCommandSequence = 1;
    ObjectBuilderPhase phase = ObjectBuilderPhase::Idle;
    // Player-issued repair keeps applying ActionManager's shroud admission
    // while the task is live. System-owned dormant bridge repair and bored
    // same-owner maintenance do not opt into this player-view contract.
    bool requireClearRepairTarget = false;
    // Object::onDisabledEdge cancels the active Dozer task so another worker
    // may claim it, then resumes that one task when the object is enabled.
    // Keep this explicit: `previous` is also ordinary retarget bookkeeping.
    bool suspendedByDisable = false;
};

struct ObjectBuilderComponent final {
    container::SharedPtr<const game::ObjectBuilderPlan> plan;
    container::Vector<ObjectBuilderRuntime> runtimes;
};

struct ObjectConstructionSiteComponent final {
    ObjectId builder = INVALID_OBJECT_ID;
    uint32_t requiredFrames = 1;
    uint32_t completedFrames = 0;
    uint64_t lastProgressTick = 0;
    uint64_t revision = 0;
    // Optional Scenario BuildList identity. A script-prioritized build keeps
    // this value correlation until completion so GameSession can publish the
    // authored object-script hook without retaining a BuildList pointer.
    uint32_t sourceSideOrdinal = UINT32_MAX;
    uint32_t sourceBuildListOrdinal = UINT32_MAX;
    bool rebuild = false;
};

struct ObjectRepairBenefactorLeaseComponent final {
    ObjectId builder = INVALID_OBJECT_ID;
    uint64_t expiresTick = 0;
    uint64_t revision = 0;
};

enum class ObjectBridgeRepairScaffoldIntentKind : uint8_t {
    EnsureCreated,
    Remove,
};

// Builder/Worker AI owns the repair decision, but GameSession owns terrain
// bridge geometry and central object spawning.  This value-only intent keeps
// that boundary explicit instead of giving an ECS system a GameSession
// pointer or recreating scaffold layout logic in the builder module.
struct ObjectBridgeRepairScaffoldIntent final {
    ObjectBridgeRepairScaffoldIntentKind kind =
        ObjectBridgeRepairScaffoldIntentKind::EnsureCreated;
    ObjectId bridge = INVALID_OBJECT_ID;
    ObjectId tower = INVALID_OBJECT_ID;
    ObjectId builder = INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectConstructionCompletionIntent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId builder = INVALID_OBJECT_ID;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

class ObjectBuilderSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          uint64_t confirmedTick) const;

    [[nodiscard]] bool beginConstruction(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId site, ObjectId builder, uint32_t requiredFrames,
        bool rebuild, uint64_t confirmedTick) const;

    [[nodiscard]] bool requestRepair(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, ObjectId target, uint64_t confirmedTick,
        uint32_t sourceSequence = 0) const;
    // Player/script repair uses RefCode ActionManager diplomacy (allied and
    // neutral structures are legal; enemies are not) and may replace the
    // builder's external queue just like DozerAIUpdate::newTask().
    [[nodiscard]] bool canRepair(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ObjectId builder, ObjectId target) const;
    [[nodiscard]] bool requestRepair(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ObjectId builder, ObjectId target,
        uint64_t confirmedTick, uint32_t sourceSequence,
        bool replaceExternalOrders,
        bool requireClearTarget) const;
    // RefCode ActionManager::canResumeConstructionOf is distinct from
    // ordinary repair: an unfinished site keeps its paid progress and may be
    // claimed only when its previous builder is no longer actively building
    // it. A resumed task uses the original Build state machine, not Repair.
    [[nodiscard]] bool canResumeConstruction(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ObjectId builder,
        ObjectId site) const;
    [[nodiscard]] bool resumeConstruction(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ObjectId builder, ObjectId site,
        uint64_t confirmedTick, uint32_t sourceSequence,
        bool replaceExternalOrders) const;
    [[nodiscard]] bool assignConstruction(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, ObjectId site, uint64_t confirmedTick,
        uint32_t sourceSequence = 0) const;

    // Value-only equivalents of the original DozerAIInterface task queries.
    // `moduleIndex` is authored builder-module order and defaults to the
    // ordinary single DozerAIUpdate/WorkerAIUpdate occurrence.
    [[nodiscard]] ObjectBuilderTask task(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, ObjectBuilderTaskKind kind,
        size_t moduleIndex = 0) const;
    [[nodiscard]] ObjectBuilderTask mostRecentTask(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, size_t moduleIndex = 0) const;
    [[nodiscard]] ObjectBuilderTask currentTask(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, size_t moduleIndex = 0) const;
    [[nodiscard]] bool isTaskPending(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, ObjectBuilderTaskKind kind,
        size_t moduleIndex = 0) const;
    [[nodiscard]] bool isAnyTaskPending(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, size_t moduleIndex = 0) const;

    // Ownership transfer (notably ConvertToHijackedVehicleCrateCollide) must
    // sever every task that belonged to the previous owner.  The Builder
    // system remains the sole owner of construction-site claims, repair
    // benefactor leases, and bridge-scaffold teardown sequencing; callers
    // only consume the resulting value intents at the terrain boundary.
    // This operation is idempotent and preserves nextCommandSequence so a
    // later task can never reuse a pre-cancellation command identity.
    [[nodiscard]] bool cancelAllTasks(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, uint64_t confirmedTick,
        container::Vector<ObjectBridgeRepairScaffoldIntent>&
            outBridgeScaffoldIntents,
        std::optional<uint32_t> authoredOrder = std::nullopt) const;

    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry* players,
        const game::terrain::MapVisibilitySnapshot* visibility,
        const GameContentSnapshot* content,
        const navigation::NavigationSystem* navigation,
        const ObjectSimulationRules& rules,
        uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& outDamage,
        container::Vector<ObjectConstructionCompletionIntent>&
            outCompletedConstruction,
        container::Vector<ObjectBridgeRepairScaffoldIntent>&
            outBridgeScaffoldIntents) const;
    // Compatibility surface for focused/system probes and callers whose
    // repair tasks are already pre-authorized. Production GameSession passes
    // PlayerRegistry through the overload above for live diplomacy checks.
    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& outDamage,
        container::Vector<ObjectConstructionCompletionIntent>&
            outCompletedConstruction,
        container::Vector<ObjectBridgeRepairScaffoldIntent>&
            outBridgeScaffoldIntents) const {
        update(registry, lifecycle, nullptr, nullptr, nullptr, nullptr, rules,
               confirmedTick,
               outDamage, outCompletedConstruction,
               outBridgeScaffoldIntents);
    }
    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& outDamage,
        container::Vector<ObjectConstructionCompletionIntent>&
            outCompletedConstruction) const {
        container::Vector<ObjectBridgeRepairScaffoldIntent> ignored;
        update(registry, lifecycle, nullptr, nullptr, nullptr, nullptr, rules,
               confirmedTick,
               outDamage, outCompletedConstruction, ignored);
    }
};

} // namespace engine
