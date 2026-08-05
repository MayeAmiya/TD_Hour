#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <utility>
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationDamageDetail.h"

namespace engine {

using namespace object_simulation_detail;

void ObjectSimulation::queueDamage(ObjectDamageRequest request) {
    if (!request.target) return;
    const HealthScalar amount = request.amount;
    uint64_t ordinal = request.submissionOrdinal;
    if (ordinal == 0) {
        ordinal = object_simulation_detail::state(*this)
                      .m_nextGameplaySubmissionOrdinal++;
        if (object_simulation_detail::state(*this)
                .m_nextGameplaySubmissionOrdinal == 0) {
            ++object_simulation_detail::state(*this)
                  .m_nextGameplaySubmissionOrdinal;
        }
    }
    object_simulation_detail::state(*this).m_damageRequests.push_back({.request = std::move(request),
                                .amount = amount,
                                .submissionOrdinal = ordinal});
}

void ObjectSimulation::queueCheckpointNavigationChange(
    ObjectId object, uint32_t authoredOrder, uint64_t confirmedTick) {
    if (!object) return;
    object_simulation_detail::state(*this)
        .m_checkpointNavigationEvents.push_back({
            .object = object,
            .authoredOrder = authoredOrder,
            .confirmedTick = confirmedTick,
            .submissionOrdinal =
                object_simulation_detail::state(*this)
                    .m_nextGameplaySubmissionOrdinal++,
        });
    if (object_simulation_detail::state(*this)
            .m_nextGameplaySubmissionOrdinal == 0) {
        ++object_simulation_detail::state(*this)
              .m_nextGameplaySubmissionOrdinal;
    }
}

bool ObjectSimulation::applyBodyStateProjection(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectBodyStateProjection& projection) {
    if (!projection.object ||
        projection.state == ObjectBodyDamageState::Rubble) {
        return false;
    }
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(projection.object);
    ObjectHealthComponent* health = entity
        ? ecs::try_get<ObjectHealthComponent>(registry, *entity)
        : nullptr;
    if (!health || health->effectivelyDead ||
        health->maximumFixed <= ObjectHealthComponent::Scalar{} ||
        health->damageState == projection.state) {
        return false;
    }

    ObjectHealthComponent::Scalar desired = health->maximumFixed;
    if (projection.state == ObjectBodyDamageState::Damaged) {
        desired = health->maximumFixed *
            object_simulation_detail::state(*this)
                .m_rules.unitDamagedThresholdFixed;
    } else if (projection.state ==
               ObjectBodyDamageState::ReallyDamaged) {
        desired = health->maximumFixed *
            object_simulation_detail::state(*this)
                .m_rules.unitReallyDamagedThresholdFixed;
    }
    // ActiveBody::setDamageState selects one health point below the threshold
    // because calcDamageState uses strict comparisons. Preserve that exact
    // low-level Body value mutation without entering the Damage transaction.
    desired -= ObjectHealthComponent::Scalar{int32_t{1}};
    desired = ObjectHealthComponent::Scalar::max(
        ObjectHealthComponent::Scalar{},
        ObjectHealthComponent::Scalar::min(health->maximumFixed, desired));
    health->previousFixed = health->currentFixed;
    health->currentFixed = desired;
    health->damageState = projection.state;
    health->effectivelyDead = desired <= ObjectHealthComponent::Scalar{};
    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, *entity)) {
        projectObjectBodyDamageVisual(
            registry, *entity, health->damageState, *visual);
    }
    markObjectDirty(
        registry, *entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    return true;
}

bool ObjectSimulation::applyBodyHealthProjection(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectBodyHealthProjection& projection) {
    if (!projection.object) return false;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(projection.object);
    ObjectHealthComponent* health = entity
        ? ecs::try_get<ObjectHealthComponent>(registry, *entity)
        : nullptr;
    if (!health) return false;

    const ObjectHealthComponent::Scalar desired =
        ObjectHealthComponent::Scalar::max(
            ObjectHealthComponent::Scalar{},
            ObjectHealthComponent::Scalar::min(
                health->maximumFixed, projection.desiredHealth));
    if (desired == health->currentFixed) return true;

    const ObjectBodyDamageState previousState = health->damageState;
    const bool wasEffectivelyDead = health->effectivelyDead;
    health->previousFixed = health->currentFixed;
    health->currentFixed = desired;
    health->damageState = objectBodyDamageStateFor(
        desired, health->maximumFixed,
        object_simulation_detail::state(*this).m_rules);
    health->effectivelyDead = desired <= ObjectHealthComponent::Scalar{};

    if (health->damageState == ObjectBodyDamageState::Rubble &&
        previousState != ObjectBodyDamageState::Rubble) {
        applyStructureRubbleGameplayState(
            registry, *entity,
            object_simulation_detail::state(*this).m_rules,
            projection.confirmedTick);
        if (const ObjectCheckpointComponent* checkpoint =
                ecs::try_get<ObjectCheckpointComponent>(registry, *entity)) {
            const uint32_t authoredOrder =
                checkpoint->plan && !checkpoint->plan->rules.empty()
                    ? checkpoint->plan->rules.front().authoredOrder : 0;
            queueCheckpointNavigationChange(
                projection.object, authoredOrder, projection.confirmedTick);
        }
    }

    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, *entity)) {
        projectObjectBodyDamageVisual(
            registry, *entity, health->damageState, *visual);
    }
    uint8_t dirty =
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
        objectDirtyBit(ObjectDirtyDomain::RenderExtraction);
    if (wasEffectivelyDead != health->effectivelyDead)
        dirty |= objectDirtyBit(ObjectDirtyDomain::Spatial);
    markObjectDirty(registry, *entity, dirty);
    return true;
}

} // namespace engine
