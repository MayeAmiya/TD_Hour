#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"

#include "debug/debug.h"
#include "core/container/string_utils.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/creation/ObjectOclCreateRandomSample.h"
#include "game/object/creation/ObjectOclSpreadPlacement.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>

namespace engine::detail {

void GameSessionWeaponEventDrain::queueOclPhysics(
    const WorkItem& item, ObjectId target, ObjectPhysicsRequestKind kind,
    LogicFixedVec3 linear, uint32_t sequence,
    ObjectPhysicsComponent::Scalar yaw,
    ObjectPhysicsComponent::Scalar pitch,
    ObjectPhysicsComponent::Scalar roll) {
    m_world.m_objectSimulation.queuePhysicsRequest({
        .target = target,
        .source = item.ocl.source,
        .sourceSequence = sequence,
        .linear = linear,
        .yawRate = yaw,
        .pitchRate = pitch,
        .rollRate = roll,
        .kind = kind,
        .confirmedTick = item.ocl.confirmedTick,
    });
}

LogicFixedVec3 GameSessionWeaponEventDrain::randomOclForce(
    ObjectPhysicsComponent::Scalar minimumMagnitude,
    ObjectPhysicsComponent::Scalar maximumMagnitude,
    ObjectPhysicsComponent::Scalar minimumPitch,
    ObjectPhysicsComponent::Scalar maximumPitch) {
    const math::q32_32 fullTurn =
        math::q32_32::from_raw(26'986'075'409ll);
    const math::q32_32 angle =
        m_content.m_simulationRandom.fixedInclusive(
            math::q32_32{}, fullTurn);
    const math::q32_32 pitch =
        m_content.m_simulationRandom.fixedInclusive(
            minimumPitch, maximumPitch);
    const math::q32_32 magnitude =
        m_content.m_simulationRandom.fixedInclusive(
            minimumMagnitude, maximumMagnitude);
    const math::q32_32_sincos angleDirection = math::fixed_sincos(angle);
    const math::q32_32_sincos pitchDirection = math::fixed_sincos(pitch);
    const math::q32_32 horizontal = pitchDirection.cosine * magnitude;
    return {
        angleDirection.cosine * horizontal,
        angleDirection.sine * horizontal,
        pitchDirection.sine * magnitude,
    };
}

void GameSessionWeaponEventDrain::processOclApplyRandomForce(
    const WorkItem& item,
    const game::ObjectCreationApplyRandomForceNugget& nugget) {
            if (!item.ocl.source) return;
            queueOclPhysics(item, item.ocl.source,
                         ObjectPhysicsRequestKind::ApplyForce,
                         randomOclForce(nugget.minimumMagnitude,
                                     nugget.maximumMagnitude,
                                     nugget.minimumPitchRadians,
                                     nugget.maximumPitchRadians),
                         nugget.authoredOrder);
            const auto randomRate = [&](ObjectPhysicsComponent::Scalar magnitude) {
                return m_content.m_simulationRandom.fixedInclusive(
                    -magnitude, magnitude);
            };
            queueOclPhysics(item, item.ocl.source,
                         ObjectPhysicsRequestKind::SetAngularRates, {},
                         nugget.authoredOrder,
                         randomRate(nugget.spinRate),
                         randomRate(nugget.spinRate),
                         randomRate(nugget.spinRate));
            return;
}

void GameSessionWeaponEventDrain::processOclFireWeapon(
    const WorkItem& item, const game::ObjectCreationListDefinition& definition,
    const game::ObjectCreationFireWeaponNugget& nugget) {
            if (!item.ocl.source || !item.ocl.hasSecondaryPosition ||
                nugget.weapon.empty()) {
                TD_LOG_ERROR(
                    "[GameSession] OCL '{}' FireWeapon rejected: source={} hasSecondary={} weapon='{}'",
                    definition.name, item.ocl.source.value,
                    item.ocl.hasSecondaryPosition, nugget.weapon);
                return;
            }
            const std::optional<ecs::entity> sourceEntity =
                m_world.m_objects.entityFromIdIncludingPending(item.ocl.source);
            const game::WeaponContentId weapon =
                m_content.m_contentSnapshot.findWeaponId(nugget.weapon);
            if (!sourceEntity || !weapon) {
                TD_LOG_ERROR(
                    "[GameSession] OCL '{}' FireWeapon unresolved: source={} sourceEntity={} weapon='{}' found={}",
                    definition.name, item.ocl.source.value,
                    sourceEntity.has_value(), nugget.weapon,
                    static_cast<bool>(weapon));
                return;
            }
            container::Vector<ObjectSystemWeaponFireCommand> nested;
            if (!queueObjectTransientWeaponFire(
                    weapon, m_world.m_registry, *sourceEntity, item.ocl.source,
                    m_content.m_contentSnapshot, m_content.m_simulationRandom,
                    nugget.authoredOrder + 1, nugget.authoredOrder,
                    item.ocl.emissionSequence, item.ocl.confirmedTick,
                    nested)) {
                TD_LOG_ERROR(
                    "[GameSession] OCL '{}' FireWeapon could not queue weapon='{}' for source={}",
                    definition.name, nugget.weapon, item.ocl.source.value);
                return;
            }
            for (ObjectSystemWeaponFireCommand& command : nested) {
                command.impactPosition = item.ocl.secondaryPosition;
            }
            pushCommands(std::move(nested));
            return;
}

void GameSessionWeaponEventDrain::processOclAttack(
    const WorkItem& item,
    const game::ObjectCreationListDefinition& definition,
    const game::ObjectCreationAttackNugget& nugget) {
            // RefCode's AttackNugget requires a primary object plus both
            // positions, then does exactly two things on the OCL source:
            //   primaryObject->setWeaponLock( m_weaponSlot, LOCKED_TEMPORARILY );
            //   ai->aiAttackPosition( secondary, m_numberOfShots, CMD_FROM_AI );
            // and finally hands the delivery decal to RadiusDecalUpdate with
            // killWhenNoLongerAttacking.  Both halves are load bearing: the GLA
            // Scud Storm authors `AutoChooseSources = PRIMARY NONE`, so the
            // salvo exists only while the temporary lock owns the slot.
            if (item.ocl.source && item.ocl.hasSecondaryPosition) {
                const std::optional<ecs::entity> sourceEntity =
                    m_world.m_objects
                        .entityFromIdIncludingPending(item.ocl.source);
                const std::optional<game::WeaponSlot> slot =
                    nugget.weaponSlot.empty()
                        ? std::optional<game::WeaponSlot>{
                              game::WeaponSlot::Primary}
                        : game::tryParseWeaponSlot(nugget.weaponSlot);
                if (!sourceEntity || !slot) {
                    TD_LOG_ERROR(
                        "[GameSession] OCL '{}' Attack unresolved: source={} sourceEntity={} weaponSlot='{}'",
                        definition.name, item.ocl.source.value,
                        sourceEntity.has_value(), nugget.weaponSlot);
                } else if (!setObjectWeaponLock(
                               m_world.m_registry, *sourceEntity, *slot,
                               ObjectWeaponLockType::Temporary)) {
                    // RefCode gates both halves on the AIUpdate/WeaponSet
                    // pair. A source without a materialized weapon in the
                    // authored slot cannot execute the attack, so publishing
                    // the order would only burn one WeaponUnavailable event.
                    TD_LOG_ERROR(
                        "[GameSession] OCL '{}' Attack could not lock weaponSlot='{}' on source={}",
                        definition.name, nugget.weaponSlot,
                        item.ocl.source.value);
                } else {
                    ObjectOrderQueueComponent* queue =
                        ecs::try_get<ObjectOrderQueueComponent>(
                            m_world.m_registry, *sourceEntity);
                    if (!queue) {
                        queue = &ecs::emplace<ObjectOrderQueueComponent>(
                            m_world.m_registry, *sourceEntity);
                    }
                    // aiAttackPosition replaces the current attack rather than
                    // stacking, so drop an unfinished salvo published by this
                    // same authored nugget before inserting the new head.
                    while (!queue->orders.empty() &&
                           queue->orders.front().kind ==
                               ObjectOrderKind::Attack &&
                           queue->orders.front().source ==
                               ObjectOrderSource::System &&
                           queue->orders.front().systemPurpose ==
                               ObjectOrderSystemPurpose::ObjectCreationAttack &&
                           queue->orders.front().systemPurposeInstance ==
                               nugget.authoredOrder) {
                        queue->orders.erase(queue->orders.begin());
                    }
                    queue->orders.insert(queue->orders.begin(),
                                         ObjectOrderIntent{
                        .kind = ObjectOrderKind::Attack,
                        .source = ObjectOrderSource::System,
                        .contextPlayer = item.ocl.owner,
                        .issuedTick = item.ocl.confirmedTick,
                        .sourceSequence = static_cast<uint32_t>(
                            std::min<uint64_t>(
                                item.ocl.emissionSequence,
                                std::numeric_limits<uint32_t>::max())),
                        .targetX = item.ocl.secondaryPosition.x,
                        .targetY = item.ocl.secondaryPosition.y,
                        .targetZ = item.ocl.secondaryPosition.z,
                        .hasTargetPosition = true,
                        // A negative authored NumberOfShots is meaningless;
                        // an explicit zero remains an authored zero-shot cap.
                        .maximumShots = static_cast<uint32_t>(
                            std::max<int32_t>(0, nugget.numberOfShots)),
                        .systemPurpose =
                            ObjectOrderSystemPurpose::ObjectCreationAttack,
                        .systemPurposeInstance = nugget.authoredOrder,
                    });
                    if (queue->orders.size() >=
                        ObjectOrderQueueComponent::MaximumQueuedOrders) {
                        queue->orders.pop_back();
                    }
                    ++queue->revision;
                }
            }
            {
                if (!nugget.deliveryDecal.empty() &&
                    item.ocl.source && item.ocl.hasSecondaryPosition) {
                    const uint64_t decalThrobTicks = std::max<uint64_t>(
                        1u,
                        (static_cast<uint64_t>(
                             nugget.deliveryDecalOpacityThrobMilliseconds) *
                             std::max<uint32_t>(
                                 1u, m_world.m_objectSimulation.rules()
                                         .logicFramesPerSecond) +
                         999u) /
                            1000u);
                    static_cast<void>(
                        m_world.m_objectSimulation.createRadiusDecal(
                            m_world.m_registry, m_world.m_objects, {
                                .object = item.ocl.source,
                                .texture = nugget.deliveryDecal,
                                .position =
                                    item.ocl.secondaryPosition,
                                .radius =
                                    nugget.deliveryDecalRadius,
                                .shadowTypeMask =
                                    nugget.deliveryDecalShadowTypeMask,
                                .minimumOpacity =
                                    nugget.deliveryDecalMinimumOpacity,
                                .maximumOpacity =
                                    nugget.deliveryDecalMaximumOpacity,
                                .opacityThrobTicks = decalThrobTicks,
                                .color = nugget.deliveryDecalColor,
                                .usesPlayerColor =
                                    nugget.deliveryDecalUsesPlayerColor,
                                .onlyVisibleToOwningPlayer =
                                    nugget.deliveryDecalOnlyVisibleToOwningPlayer,
                                .killWhenNoLongerAttacking = true,
                                .confirmedTick =
                                    item.ocl.confirmedTick,
                            }));
                }
            }
            return;
}

} // namespace engine::detail
