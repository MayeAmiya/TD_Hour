#include "game/object/simulation/combat/ObjectTactical.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "core/math/fixed/q32_32_trig.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/contracts/ObjectToppleMath.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/navigation/runtime/NavigationSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>

#include "game/object/simulation/combat/ObjectTacticalDetail.h"

namespace engine {
using namespace object_tactical_detail;

namespace {

[[nodiscard]] constexpr bool isLegacyMutuallyExclusiveSpecialAbility(
    game::SpecialPowerType type) noexcept {
    switch (type) {
    case game::SpecialPowerType::BlackLotusDisableVehicleHack:
    case game::SpecialPowerType::BlackLotusStealCashHack:
    case game::SpecialPowerType::BlackLotusCaptureBuilding:
    case game::SpecialPowerType::RemoteCharges:
    case game::SpecialPowerType::TimedCharges:
    case game::SpecialPowerType::InfantryCaptureBuilding:
    case game::SpecialPowerType::BoobyTrap:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] uint64_t claimGameplaySubmissionOrdinal(
    uint64_t& nextGameplaySubmissionOrdinal) noexcept {
    const uint64_t ordinal = nextGameplaySubmissionOrdinal++;
    if (nextGameplaySubmissionOrdinal == 0) {
        ++nextGameplaySubmissionOrdinal;
    }
    return ordinal;
}

} // namespace

void queueObjectToppleRequest(ecs::registry& registry,
                              ObjectToppleRequest request) {
    if (!request.object) return;
    ObjectToppleJournal* journal =
        registry.ctx().find<ObjectToppleJournal>();
    if (!journal) {
        journal = &registry.ctx().emplace<ObjectToppleJournal>();
    }
    const uint64_t ordinal = journal->nextSubmissionOrdinal++;
    if (journal->nextSubmissionOrdinal == 0) {
        journal->nextSubmissionOrdinal = 1;
    }
    journal->pending.push_back({
        .request = std::move(request),
        .submissionOrdinal = ordinal,
    });
}

void ObjectTacticalSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    const auto* type = ecs::try_get<ThingTemplateComponent>(registry, entity);
    const auto plan = type && type->archetype ? type->archetype->tacticalPlan : nullptr;
    if (!plan) return;
    ObjectTacticalComponent component;
    component.plan = plan;
    component.propagandaTowers.resize(plan->propagandaTowers.size());
    for (size_t index = 0; index < component.propagandaTowers.size(); ++index) {
        const uint64_t delay = std::max<uint64_t>(
            1, millisecondsToTicks(
                   plan->propagandaTowers[index].scanDelayMilliseconds,
                   rules.logicFramesPerSecond));
        // RefCode initializes lastScanFrame to zero. A tower present near
        // match start waits for the first delay; a tower created after that
        // global frame threshold is eligible to scan immediately.
        component.propagandaTowers[index].nextScanTick =
            confirmedTick >= delay ? confirmedTick : delay;
    }
    component.deployStyles.resize(plan->deployStyles.size());
    component.topple.resize(plan->topple.size());
    component.battlePlans.resize(plan->battlePlans.size());
    for (size_t index = 0; index < component.battlePlans.size(); ++index) {
        if (const SpecialPowerDefinition* definition =
                content.findSpecialPower(
                    plan->battlePlans[index].specialPowerTemplate)) {
            component.battlePlans[index].specialPower = definition->id;
        }
    }
    component.specialAbilities.resize(plan->specialAbilities.size());
    for (size_t index = 0; index < component.specialAbilities.size(); ++index) {
        if (const SpecialPowerDefinition* definition =
                content.findSpecialPower(
                    plan->specialAbilities[index].specialPowerTemplate)) {
            component.specialAbilities[index].specialPower = definition->id;
        }
    }
    component.commandButtonHunts.resize(plan->commandButtonHunts.size());
    component.wander.resize(plan->wanderAuthoredOrders.size());
    if (!component.battlePlans.empty()) {
        if (ObjectWeaponComponent* weapons =
                ecs::try_get<ObjectWeaponComponent>(registry, entity)) {
            // BattlePlanUpdate::onObjectCreated disables its turret. Only an
            // active Bombardment plan enables it again.
            setObjectTurretsEnabled(*weapons, false);
        }
    }
    if (!component.deployStyles.empty()) {
        static_cast<void>(ObjectStatusSystem::apply(registry, entity, {
            .setMask = statusBit(game::ObjectStatusFlag::NoAttack),
            .confirmedTick = confirmedTick,
        }));
    }
    if (auto* existing = ecs::try_get<ObjectTacticalComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectTacticalComponent>(registry, entity, std::move(component));
    }
}

bool ObjectTacticalSystem::activateSpecialAbility(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const SpecialPowerDefinition& definition,
    const ObjectOrderIntent& order, const GameContentSnapshot& content,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectSpecialAbilityEffectRequest>& effectRequests,
    PlayerRegistry* players, SimulationRandom* random,
    bool deferRechargeUntilPreparation) const {
    const auto entity = lifecycle.entityFromId(object);
    if (!entity) return false;
    auto* component = ecs::try_get<ObjectTacticalComponent>(registry, *entity);
    if (!component || !component->plan) return false;

    for (size_t index = 0; index < component->plan->battlePlans.size(); ++index) {
        auto& runtime = component->battlePlans[index];
        if (!runtime.specialPower || runtime.specialPower != definition.id)
            continue;
        game::ObjectBattlePlanStatus desired = game::ObjectBattlePlanStatus::None;
        if (const auto* button = content.findCommandButton(order.contentName)) {
            if (containsToken(button->options, "OPTION_ONE")) desired = game::ObjectBattlePlanStatus::Bombardment;
            else if (containsToken(button->options, "OPTION_TWO")) desired = game::ObjectBattlePlanStatus::HoldTheLine;
            else if (containsToken(button->options, "OPTION_THREE")) desired = game::ObjectBattlePlanStatus::SearchAndDestroy;
        }
        if (desired == game::ObjectBattlePlanStatus::None) return false;
        runtime.desired = desired;
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
        if (owner && players) {
            static_cast<void>(players->recordAcademyEvent(
                owner->player,
                PlayerAcademyEvent::BattlePlanSelected));
        }
        return true;
    }

    for (size_t index = 0; index < component->plan->specialAbilities.size(); ++index) {
        const auto& rule = component->plan->specialAbilities[index];
        auto& runtime = component->specialAbilities[index];
        if (!runtime.specialPower || runtime.specialPower != definition.id)
            continue;

        // SpecialAbilityUpdate::initiateIntentToDoSpecialPower synchronously
        // calls onExit(false) on these seven legacy modules before the newly
        // selected ability begins its unpack/preparation state.  Do the same
        // in one deterministic transaction: old presentation/status state is
        // retired first, persistent placed objects survive, and every
        // non-persistent object is handed to the existing gameplay effect
        // drain rather than being leaked or deleted through a side channel.
        const size_t abilityCount = std::min(
            component->specialAbilities.size(),
            component->plan->specialAbilities.size());
        for (size_t otherIndex = 0; otherIndex < abilityCount; ++otherIndex) {
            if (otherIndex == index) continue;
            auto& otherRuntime = component->specialAbilities[otherIndex];
            const auto& otherRule =
                component->plan->specialAbilities[otherIndex];
            const SpecialPowerDefinition* otherDefinition =
                content.findSpecialPower(otherRuntime.specialPower);
            if (!otherDefinition ||
                !isLegacyMutuallyExclusiveSpecialAbility(
                    otherDefinition->specialPowerType)) {
                continue;
            }

            if (!otherRule.specialObjectsPersistent &&
                !otherRuntime.specialObjects.empty()) {
                ObjectSpecialAbilityEffectRequest request{
                    .kind = ObjectSpecialAbilityEffectKind::
                        DestroySpecialObjects,
                    .source = object,
                    .specialPower = otherRuntime.specialPower,
                    .specialPowerTemplate =
                        otherRule.specialPowerTemplate,
                    .specialPowerType = otherDefinition->specialPowerType,
                    .ruleIndex = static_cast<uint32_t>(otherIndex),
                    .authoredOrder = otherRule.authoredOrder,
                    .activationSequence =
                        otherRuntime.activationSequence,
                    .submissionOrdinal = claimGameplaySubmissionOrdinal(
                        nextGameplaySubmissionOrdinal),
                    .confirmedTick = confirmedTick,
                };
                request.objects.reserve(otherRuntime.specialObjects.size());
                for (const ObjectSpecialAbilityObject& special :
                     otherRuntime.specialObjects) {
                    request.objects.push_back(special.object);
                }
                effectRequests.push_back(std::move(request));
                otherRuntime.specialObjects.clear();
            }

            otherRuntime.active = false;
            otherRuntime.effectTriggered = false;
            otherRuntime.deferredRechargePending = false;
            otherRuntime.phase = ObjectSpecialAbilityPhase::Inactive;
            otherRuntime.phaseEndTick = 0;
            otherRuntime.triggerTick = 0;
            otherRuntime.finishTick = confirmedTick;
        }

        // initiateIntentToDoSpecialPower clears the shared owner pose before
        // any new unpack/preparation animation is selected.  This also makes
        // cancellation and reactivation within one confirmed tick ordered:
        // an old ability can never clear the new ability on the next update.
        setModelCondition(registry, *entity,
                          game::ModelConditionFlag::Unpacking, false,
                          confirmedTick);
        setModelCondition(registry, *entity,
                          game::ModelConditionFlag::Packing, false,
                          confirmedTick);
        clearSpecialAbilityPreparationConditions(
            registry, *entity, confirmedTick);
        static_cast<void>(ObjectStatusSystem::apply(registry, *entity, {
            .clearMask = statusBit(
                game::ObjectStatusFlag::IsUsingAbility),
            .confirmedTick = confirmedTick,
        }));

        runtime.active = true;
        runtime.effectTriggered = false;
        runtime.captureFlashPhase = {};
        runtime.deferredRechargePending =
            deferRechargeUntilPreparation;
        runtime.preTriggerRevealApplied = false;
        runtime.preparationObjectAttempted = false;
        runtime.boobyTrapTriggered = false;
        runtime.fleeAfterPacking = false;
        runtime.facingRequestQueued = false;
        runtime.facingStateActive = false;
        runtime.facingComplete = false;
        runtime.facingFailed = false;
        runtime.facingRequestIssuedTick = 0;
        runtime.facingRequestSequence = 0;
        runtime.target = order.targetObject;
        runtime.hasTargetPosition = order.hasTargetPosition;
        runtime.noTargetCommand = !order.targetObject && !order.hasTargetPosition;
        runtime.targetPosition = {
            order.targetX, order.targetY, order.targetZ};
        if (const auto targetEntity = lifecycle.entityFromId(order.targetObject)) {
            if (const auto* targetTransform =
                    ecs::try_get<TransformComponent>(registry, *targetEntity)) {
                runtime.targetPosition = readAuthoritativeObjectPosition(
                    registry, *targetEntity, *targetTransform);
                runtime.hasTargetPosition = true;
            }
        }
        runtime.specialPowerType = definition.specialPowerType;
        const bool skipUnpack = runtime.noTargetCommand &&
            rule.skipPackingWithNoTarget;
        if (rule.needToFaceTarget && runtime.hasTargetPosition) {
            // RefCode enters AIFaceObject/AIFacePosition and waits for AI to
            // return idle before unpacking. The tactical update emits the
            // typed facing request; authoritative yaw is never snapped here.
            runtime.phase = ObjectSpecialAbilityPhase::Facing;
            runtime.phaseEndTick = 0;
        } else if (!skipUnpack && rule.unpackMilliseconds != 0) {
            runtime.phase = ObjectSpecialAbilityPhase::Unpacking;
            runtime.phaseEndTick = saturatingAdd(
                confirmedTick, millisecondsToTicks(
                    variedMilliseconds(rule.unpackMilliseconds,
                        rule.packUnpackVariationFactor, random),
                    rules.logicFramesPerSecond));
        } else {
            runtime.phase = ObjectSpecialAbilityPhase::Preparing;
            runtime.phaseEndTick = saturatingAdd(
                confirmedTick, millisecondsToTicks(
                    rule.preparationMilliseconds,
                    rules.logicFramesPerSecond));
        }
        runtime.triggerTick = runtime.phase ==
                ObjectSpecialAbilityPhase::Facing
            ? 0 : runtime.phaseEndTick;
        runtime.finishTick = 0;
        runtime.activationTick = confirmedTick;
        ++runtime.activationSequence;
        if (runtime.activationSequence == 0) ++runtime.activationSequence;
        if (const ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(registry, *entity)) {
            runtime.observedExternalRevision = queue->externalRevision;
        } else {
            runtime.observedExternalRevision = 0;
        }
        static_cast<void>(ObjectStatusSystem::apply(registry, *entity, {
            .setMask = statusBit(game::ObjectStatusFlag::IsUsingAbility),
            .confirmedTick = confirmedTick,
        }));
        if (runtime.phase == ObjectSpecialAbilityPhase::Unpacking) {
            setModelCondition(registry, *entity, game::ModelConditionFlag::Unpacking, true,
                              confirmedTick);
        } else if (const std::optional<game::ModelConditionFlag> pose =
                       specialAbilityPreparationCondition(
                           runtime.specialPowerType)) {
            // No unpack phase means RefCode's update() reaches
            // startPreparation() on this same activation, so the authored
            // preparation pose is selected here rather than at the
            // Unpacking -> Preparing transition in ObjectTacticalUpdate.
            setModelCondition(registry, *entity, *pose, true, confirmedTick);
        }
        return true;
    }
    return false;
}

ObjectSpecialAbilityAdmission ObjectTacticalSystem::admitSpecialAbility(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const SpecialPowerDefinition& definition,
    const GameContentSnapshot& content, const ObjectOrderIntent& order,
    const game::terrain::MapVisibilitySnapshot* visibility,
    const navigation::NavigationSystem* navigation,
    bool attackUsesLineOfSight,
    container::Span<const uint64_t> seeThroughObstacles) const {
    ObjectSpecialAbilityAdmission result;
    const auto entity = lifecycle.entityFromId(object);
    const auto* component = entity
        ? ecs::try_get<ObjectTacticalComponent>(registry, *entity) : nullptr;
    const auto* sourceTransform = entity
        ? ecs::try_get<TransformComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan || !sourceTransform) return result;

    // BattlePlanUpdate is another SpecialPowerUpdateInterface consumer.  It
    // does not need approach/facing admission, but must be allowed through to
    // activateSpecialAbility(), which validates the authored command option.
    for (const ObjectBattlePlanRuntime& runtime : component->battlePlans) {
        if (runtime.specialPower &&
            runtime.specialPower == definition.id) {
            result.status = ObjectSpecialAbilityAdmissionStatus::Ready;
            return result;
        }
    }

    for (size_t index = 0; index < component->plan->specialAbilities.size();
         ++index) {
        const auto& rule = component->plan->specialAbilities[index];
        const auto& runtime = component->specialAbilities[index];
        if (!runtime.specialPower ||
            runtime.specialPower != definition.id) {
            continue;
        }
        result.ruleIndex = static_cast<uint32_t>(index);
        if (runtime.active) return result;
        result.supportsDeferredRecharge = true;
        const bool remoteDetonation =
            isRemoteCharges(definition.specialPowerType) &&
            !order.targetObject && !order.hasTargetPosition;
        size_t liveSpecialObjects = 0;
        for (const ObjectSpecialAbilityObject& special :
             runtime.specialObjects) {
            const auto specialEntity = lifecycle.entityFromId(special.object);
            if (!specialEntity ||
                !alive(registry, lifecycle, special.object, *specialEntity))
                continue;
            ++liveSpecialObjects;
            if (rule.uniqueSpecialObjectTargets && order.targetObject &&
                special.target == order.targetObject) {
                return result;
            }
        }
        if (!remoteDetonation && rule.specialObjectsPersistent &&
            liveSpecialObjects >= rule.maximumSpecialObjects &&
            !rule.specialObject.empty()) {
            return result;
        }
        if (order.targetObject &&
            (definition.specialPowerType ==
                 game::SpecialPowerType::RemoteCharges ||
             definition.specialPowerType ==
                 game::SpecialPowerType::TimedCharges)) {
            const game::SpecialPowerType opposite =
                definition.specialPowerType ==
                        game::SpecialPowerType::RemoteCharges
                    ? game::SpecialPowerType::TimedCharges
                    : game::SpecialPowerType::RemoteCharges;
            const size_t otherCount = std::min(
                component->specialAbilities.size(),
                component->plan->specialAbilities.size());
            for (size_t otherIndex = 0; otherIndex < otherCount;
                 ++otherIndex) {
                const ObjectSpecialAbilityRuntime& otherRuntime =
                    component->specialAbilities[otherIndex];
                const SpecialPowerDefinition* otherDefinition =
                    content.findSpecialPower(otherRuntime.specialPower);
                if (!otherDefinition ||
                    otherDefinition->specialPowerType != opposite) {
                    continue;
                }
                for (const ObjectSpecialAbilityObject& special :
                     otherRuntime.specialObjects) {
                    const std::optional<ecs::entity> specialEntity =
                        lifecycle.entityFromId(special.object);
                    if (special.target == order.targetObject &&
                        specialEntity && alive(
                            registry, lifecycle, special.object,
                            *specialEntity)) {
                        return result;
                    }
                }
            }
        }

        LogicFixedVec3 target{};
        std::optional<ecs::entity> targetEntity;
        const TransformComponent* targetTransform = nullptr;
        bool hasTarget = false;
        if (order.targetObject) {
            targetEntity = lifecycle.entityFromId(order.targetObject);
            targetTransform = targetEntity
                ? ecs::try_get<TransformComponent>(registry, *targetEntity)
                : nullptr;
            if (!targetTransform) return result;
            target = readAuthoritativeObjectPosition(
                registry, *targetEntity, *targetTransform);
            hasTarget = true;
        } else if (order.hasTargetPosition) {
            target = {order.targetX, order.targetY, order.targetZ};
            hasTarget = true;
        }
        if (!hasTarget) {
            result.status = ObjectSpecialAbilityAdmissionStatus::Ready;
            return result;
        }

        const LogicFixedVec3 source = readAuthoritativeObjectPosition(
            registry, *entity, *sourceTransform);
        const math::q32_32 dx{target.x - source.x};
        const math::q32_32 dy{target.y - source.y};
        const math::q32_32 distanceSquared = dx * dx + dy * dy;
        const auto* sourceGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, *entity);
        const auto* targetGeometry = targetEntity
            ? ecs::try_get<ObjectGeometryComponent>(registry, *targetEntity)
            : nullptr;
        const math::q32_32 sourceRadius = sourceGeometry
            ? math::q32_32::max(math::q32_32{},
                                sourceGeometry->boundingCircleRadiusFixed)
            : math::q32_32{};
        const math::q32_32 targetRadius = targetGeometry
            ? math::q32_32::max(math::q32_32{},
                                targetGeometry->boundingCircleRadiusFixed)
            : math::q32_32{};
        // PartitionManager::FROM_BOUNDINGSPHERE_2D subtracts the source
        // radius even when the destination is a position rather than an
        // Object.  Omitting it made every position-targeted ability stop one
        // actor radius too far away from its authored StartAbilityRange.
        const math::q32_32 objectContactDistance = sourceRadius +
            (order.targetObject ? targetRadius : math::q32_32{});
        const math::q32_32 centerDistance =
            math::q32_32::sqrt(distanceSquared);
        const math::q32_32 surfaceDistance = math::q32_32::max(
            math::q32_32{}, centerDistance - objectContactDistance);
        const math::q32_32 range = math::q32_32::max(
            math::q32_32{}, rule.startAbilityRange);
        const int64_t pathfindCellSizeRaw = navigation &&
                navigation->grid().transform().cellSizeRaw > 0
            ? navigation->grid().transform().cellSizeRaw
            : (int64_t{10} << 32u);
        const math::q32_32 undersizedRange = math::q32_32::max(
            math::q32_32{},
            range - math::q32_32::from_raw(pathfindCellSizeRaw) /
                math::q32_32{int32_t{4}});
        const bool requiresObjectContact = order.targetObject &&
            undersizedRange == math::q32_32{};
        bool objectContact = !requiresObjectContact;
        if (requiresObjectContact && targetEntity && targetTransform &&
            sourceGeometry && targetGeometry) {
            ObjectCollisionContact contact;
            objectContact = computeObjectCollisionContact(
                source,
                readAuthoritativeObjectYaw(
                    registry, *entity, *sourceTransform),
                *sourceGeometry, target,
                readAuthoritativeObjectYaw(
                    registry, *targetEntity, *targetTransform),
                *targetGeometry, contact);
        }
        bool lineOfSight = true;
        if (rule.approachRequiresLineOfSight && visibility) {
            const auto* sourceOwner =
                ecs::try_get<OwnerComponent>(registry, *entity);
            math::q32_32 radius{};
            if (order.targetObject) {
                if (targetGeometry) radius = targetRadius;
            }
            // Stand-in for isClearLineOfSightTerrain(): this project has no
            // terrain-height ray, so shroud coverage of the target footprint
            // is retained as the visibility half of the filter.
            lineOfSight = sourceOwner &&
                visibility->footprintHasClearCellRaw(
                    sourceOwner->player, target.x.raw(),
                    target.y.raw(), radius.raw());
        }
        if (rule.approachRequiresLineOfSight && lineOfSight) {
            // The obstacle half of PartitionFilterLineOfSight. It routes to
            // isAttackViewBlockedByObstacle, so it applies only to sources
            // authored KINDOF_ATTACK_NEEDS_LINE_OF_SIGHT and only while
            // AIData.AttackUsesLineOfSight is enabled; the shared predicate
            // owns both gates.
            lineOfSight = !objectAttackViewBlockedByObstacle(
                registry, lifecycle, navigation, attackUsesLineOfSight,
                seeThroughObstacles, *entity, object, source,
                order.targetObject, target, false);
        }
        const bool unlimitedRange =
            range >= math::q32_32{10'000'000};
        // isWithinStartAbilityRange first checks the authored range, then
        // uses StartAbilityRange - PATHFIND_CELL_SIZE * 0.25 for the LOS
        // candidate query.  A resulting zero range is contact-class and must
        // observe a real geometry collision before the ability may begin.
        const bool insideLosCandidateRange =
            !rule.approachRequiresLineOfSight ||
            surfaceDistance <= undersizedRange;
        if (lineOfSight && insideLosCandidateRange && objectContact &&
            (unlimitedRange || surfaceDistance <= range)) {
            result.status = ObjectSpecialAbilityAdmissionStatus::Ready;
            return result;
        }

        if (!requiresObjectContact &&
            centerDistance <= objectContactDistance) {
            // There is no closer approach position for a non-contact-class
            // ability. Contact-class abilities deliberately remain in the
            // approach loop until the exact geometry test above succeeds.
            result.status = ObjectSpecialAbilityAdmissionStatus::Ready;
            return result;
        }
        // Stop just inside the authored radius. Re-evaluating the original
        // order after movement handles moving targets without consuming the
        // recharge or retaining an ECS entity/pointer.
        const math::q32_32 approachRange =
            rule.approachRequiresLineOfSight ? undersizedRange : range;
        const math::q32_32 retainedDistance = lineOfSight &&
            approachRange > math::q32_32{1}
            ? objectContactDistance + approachRange - math::q32_32{1}
            : objectContactDistance;
        const math::q32_32 travel = math::q32_32::max(
            math::q32_32{}, centerDistance - retainedDistance);
        result.approachPosition = {
            source.x + dx * travel / centerDistance,
            source.y + dy * travel / centerDistance,
            target.z,
        };
        result.status = ObjectSpecialAbilityAdmissionStatus::Approach;
        return result;
    }
    return result;
}

bool ObjectTacticalSystem::acknowledgeSpecialObjectSpawn(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId owner, uint32_t ruleIndex, uint64_t activationSequence,
    ObjectId target, uint64_t spawnSequence, ObjectId spawned,
    bool accepted) const {
    const auto entity = lifecycle.entityFromId(owner);
    auto* component = entity
        ? ecs::try_get<ObjectTacticalComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan ||
        ruleIndex >= component->specialAbilities.size() ||
        ruleIndex >= component->plan->specialAbilities.size()) {
        return false;
    }
    auto& runtime = component->specialAbilities[ruleIndex];
    const auto& rule = component->plan->specialAbilities[ruleIndex];
    if (!accepted || !spawned ||
        runtime.activationSequence != activationSequence ||
        (!runtime.active && !rule.specialObjectsPersistent)) {
        return false;
    }
    for (const ObjectSpecialAbilityObject& existing :
         runtime.specialObjects) {
        if (existing.spawnSequence == spawnSequence ||
            existing.object == spawned) return false;
    }
    runtime.specialObjects.push_back({
        .object = spawned,
        .target = target,
        .spawnSequence = spawnSequence,
    });
    std::sort(runtime.specialObjects.begin(), runtime.specialObjects.end(),
        [](const ObjectSpecialAbilityObject& left,
           const ObjectSpecialAbilityObject& right) {
            if (left.spawnSequence != right.spawnSequence)
                return left.spawnSequence < right.spawnSequence;
            return left.object < right.object;
        });
    return true;
}

bool ObjectTacticalSystem::acceptsSpecialAbilityFacingRequest(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpecialAbilityFacingRequest& request) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(request.source);
    const ObjectTacticalComponent* component = entity
        ? ecs::try_get<ObjectTacticalComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan ||
        request.ruleIndex >= component->specialAbilities.size() ||
        request.ruleIndex >= component->plan->specialAbilities.size()) {
        return false;
    }
    const ObjectSpecialAbilityRuntime& runtime =
        component->specialAbilities[request.ruleIndex];
    return runtime.active &&
        runtime.phase == ObjectSpecialAbilityPhase::Facing &&
        runtime.activationSequence == request.activationSequence &&
        runtime.facingRequestQueued && !runtime.facingStateActive &&
        runtime.target == request.target;
}

bool ObjectTacticalSystem::acknowledgeSpecialAbilityFacingRequest(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpecialAbilityFacingRequest& request, bool accepted,
    bool terminalFailure, uint64_t requestIssuedTick,
    uint32_t requestSequence) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(request.source);
    ObjectTacticalComponent* component = entity
        ? ecs::try_get<ObjectTacticalComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan ||
        request.ruleIndex >= component->specialAbilities.size() ||
        request.ruleIndex >= component->plan->specialAbilities.size()) {
        return false;
    }
    ObjectSpecialAbilityRuntime& runtime =
        component->specialAbilities[request.ruleIndex];
    if (!runtime.active ||
        runtime.phase != ObjectSpecialAbilityPhase::Facing ||
        runtime.activationSequence != request.activationSequence ||
        !runtime.facingRequestQueued || runtime.target != request.target) {
        return false;
    }
    runtime.facingRequestQueued = false;
    if (accepted && requestSequence != 0) {
        runtime.facingStateActive = true;
        runtime.facingRequestIssuedTick = requestIssuedTick;
        runtime.facingRequestSequence = requestSequence;
    } else if (terminalFailure) {
        runtime.facingFailed = true;
    }
    return true;
}

bool ObjectTacticalSystem::acknowledgeSpecialAbilityFacingFeedback(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId source, uint64_t requestIssuedTick,
    uint32_t requestSequence, bool completed) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(source);
    ObjectTacticalComponent* component = entity
        ? ecs::try_get<ObjectTacticalComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan || requestSequence == 0)
        return false;
    const size_t count = std::min(
        component->specialAbilities.size(),
        component->plan->specialAbilities.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectSpecialAbilityRuntime& runtime =
            component->specialAbilities[index];
        if (!runtime.active ||
            runtime.phase != ObjectSpecialAbilityPhase::Facing ||
            !runtime.facingStateActive ||
            runtime.facingRequestIssuedTick != requestIssuedTick ||
            runtime.facingRequestSequence != requestSequence) {
            continue;
        }
        runtime.facingStateActive = false;
        runtime.facingComplete = completed;
        runtime.facingFailed = !completed;
        return true;
    }
    return false;
}

bool ObjectTacticalSystem::setCommandButtonHunt(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, container::String commandButton,
    uint64_t confirmedTick) const {
    const auto entity = lifecycle.entityFromId(object);
    auto* component = entity ? ecs::try_get<ObjectTacticalComponent>(registry, *entity) : nullptr;
    if (!component || component->commandButtonHunts.empty()) return false;
    auto* queue = ecs::try_get<ObjectOrderQueueComponent>(registry, *entity);
    if (!queue) {
        queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, *entity);
    }
    // RefCode setCommandButton() calls aiIdle(CMD_FROM_AI) before waking the
    // module. Clearing the deterministic queue and advancing its internal
    // revision is the modern equivalent. It must not advance
    // externalRevision: no Player/Script AI command was admitted here.
    queue->orders.clear();
    ++queue->revision;
    if (queue->revision == 0) ++queue->revision;
    for (auto& runtime : component->commandButtonHunts) {
        runtime.commandButton = commandButton;
        runtime.nextScanTick = confirmedTick;
        runtime.observedExternalRevision = queue->externalRevision;
    }
    return true;
}

bool ObjectTacticalSystem::setWanderInPlace(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick) const {
    const auto entity = lifecycle.entityFromId(object);
    if (!entity ||
        !ecs::try_get<ObjectLocomotionComponent>(registry, *entity)) {
        return false;
    }
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, *entity);
    if (!transform) return false;
    const LogicFixedVec3 position = readAuthoritativeObjectPosition(
        registry, *entity, *transform);
    auto* queue = ecs::try_get<ObjectOrderQueueComponent>(registry, *entity);
    if (!queue) {
        queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, *entity);
    }
    queue->orders.clear();
    ++queue->revision;
    if (queue->revision == 0) ++queue->revision;
    ++queue->externalRevision;
    if (queue->externalRevision == 0) ++queue->externalRevision;
    queue->replacementExternalRevision = queue->externalRevision;
    queue->replacementExternalSource = ObjectOrderSource::Script;
    queue->replacementExternalKind = ObjectOrderKind::Move;
    queue->orders.push_back({
        .kind = ObjectOrderKind::Move,
        .source = ObjectOrderSource::Script,
        .issuedTick = confirmedTick,
        .targetX = position.x,
        .targetY = position.y,
        .targetZ = position.z,
        .hasTargetPosition = true,
        .moveRouteSubtype = ObjectMoveRouteSubtype::WanderInPlace,
    });
    return true;
}

bool ObjectTacticalSystem::applyTopplingForce(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const LogicFixedVec3& direction,
    math::q32_32 speed, uint64_t confirmedTick,
    uint64_t& nextGameplaySubmissionOrdinal,
    bool noBounce, bool noFx) const {
    const auto entity = lifecycle.entityFromId(object);
    auto* component = entity ? ecs::try_get<ObjectTacticalComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan || component->topple.empty()) return false;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, *entity);
    if (health && health->effectivelyDead) return false;
    LogicFixedVec3 effectiveDirection = direction;
    if (const ObjectScriptToppleDirectionComponent* overrideDirection =
            ecs::try_get<ObjectScriptToppleDirectionComponent>(registry,
                                                                *entity)) {
        effectiveDirection = overrideDirection->direction;
    }
    const math::q32_32 lengthSquared =
        effectiveDirection.x * effectiveDirection.x +
        effectiveDirection.y * effectiveDirection.y +
        effectiveDirection.z * effectiveDirection.z;
    if (lengthSquared <= math::q32_32{}) return false;
    const math::q32_32 length = math::q32_32::sqrt(lengthSquared);
    effectiveDirection.x /= length;
    effectiveDirection.y /= length;
    effectiveDirection.z /= length;

    const ObjectPhysicsComponent* existingPhysics =
        ecs::try_get<ObjectPhysicsComponent>(registry, *entity);
    const ObjectFixedTransformComponent* fixedTransform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, *entity);
    const math::q32_32 initialYaw = game::normalizeToppleAngle(
        existingPhysics && existingPhysics->ownsAttitude
            ? existingPhysics->yaw
            : fixedTransform ? fixedTransform->yawRadians : math::q32_32{});
    const LogicFixedVec3 initialPosition = fixedTransform
        ? fixedTransform->position : LogicFixedVec3{};
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, *entity);
    const bool burned = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Burned));

    bool applied = false;
    bool ownsVisualTopple = false;
    const size_t count = std::min(component->topple.size(),
                                  component->plan->topple.size());
    for (size_t index = 0; index < count; ++index) {
        auto& runtime = component->topple[index];
        const auto& rule = component->plan->topple[index];
        // Object::topple() calls ToppleUpdate only while its state is
        // TOPPLE_UPRIGHT. Later nuclear/flood waves must not restart a tree
        // which is already falling or has reached TOPPLE_DOWN.
        if (runtime.active || runtime.finished) continue;
        runtime.active = true;
        runtime.finished = false;
        runtime.startKillIssued = false;
        runtime.noBounce = noBounce;
        runtime.noFx = noFx;
        runtime.direction = effectiveDirection;
        runtime.angularVelocity = speed * rule.initialVelocityPercent;
        runtime.angularAcceleration = speed * rule.initialAccelerationPercent;
        runtime.angularAccumulation = {};
        runtime.yawDelta = {};
        runtime.yawStepsRemaining = 0;
        applied = true;
        if (rule.killWhenStartToppling) continue;

        LogicFixedVec3 moduleDirection = effectiveDirection;
        math::q32_32 toppleAngle = game::normalizeToppleAngle(
            math::fixed_atan2(moduleDirection.y, moduleDirection.x));
        if (rule.toppleLeftOrRightOnly) {
            // Fences and streetlights can fall only along their authored
            // local left/right axis.  Select the nearer world-space normal,
            // exactly as ToppleUpdate::angleClosestTo does in ZH.
            toppleAngle = game::angleClosestTo(
                initialYaw + game::kToppleHalfPi,
                initialYaw - game::kToppleHalfPi, toppleAngle);
            moduleDirection.x = math::fixed_cos(toppleAngle);
            moduleDirection.y = math::fixed_sin(toppleAngle);
            moduleDirection.z = {};
            runtime.direction = moduleDirection;
            m_topplePathfindRemovalRequests.push_back({
                .object = object,
                .emissionSequence = nextGameplaySubmissionOrdinal++,
                .confirmedTick = confirmedTick,
            });
        }

        const math::q32_32 desiredYaw = game::angleClosestTo(
            toppleAngle + game::kToppleHalfPi,
            toppleAngle - game::kToppleHalfPi, initialYaw);
        const int64_t twiceVelocityRaw = std::max<int64_t>(
            1, math::q32_32::abs(runtime.angularVelocity).raw() * 2);
        const uint64_t rawSteps = static_cast<uint64_t>(
            game::kToppleHalfPi.raw() / twiceVelocityRaw);
        runtime.yawStepsRemaining = static_cast<uint32_t>(
            std::clamp<uint64_t>(rawSteps, 1,
                                 std::numeric_limits<uint32_t>::max()));
        runtime.yawDelta = game::shortestToppleAngleDelta(
            initialYaw, desiredYaw) /
            math::q32_32{static_cast<int32_t>(
                std::min<uint32_t>(runtime.yawStepsRemaining,
                                   static_cast<uint32_t>(INT32_MAX)))};

        // ZH selects TOPPLED and emits ToppleFX at force admission, before
        // the first angular update. TOPPLE_OPTIONS_NO_FX suppresses only the
        // later BounceFX, not this initial effect.
        setModelCondition(registry, *entity, game::ModelConditionFlag::Toppled, true,
                          confirmedTick);
        if (!rule.toppleFx.empty()) {
            m_toppleFxEvents.push_back({
                .object = object,
                .position = initialPosition,
                .yawRadians = initialYaw,
                .fxList = rule.toppleFx,
                .authoredOrder = rule.authoredOrder,
                .confirmedTick = confirmedTick,
            });
        }
        if (!rule.stumpName.empty()) {
            m_toppleStumpSpawnRequests.push_back({
                .source = object,
                .objectTemplate = rule.stumpName,
                .position = initialPosition,
                .yawRadians = initialYaw,
                .ruleIndex = static_cast<uint32_t>(index),
                .authoredOrder = rule.authoredOrder,
                .emissionSequence = nextGameplaySubmissionOrdinal++,
                .confirmedTick = confirmedTick,
                .burned = burned,
            });
        }
        ownsVisualTopple = true;
    }
    if (!applied) return false;

    // KillWhenStartToppling returns before every visual/physics side effect
    // in the original module.  Do not fabricate a falling attitude when all
    // admitted ToppleUpdate modules selected that path.
    if (!ownsVisualTopple) return true;

    ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, *entity);
    if (!physics) {
        // ToppleUpdate is valid on stock props such as StreetLamp which have
        // no authored PhysicsBehavior. Materialize only the sparse attitude
        // owner when the first force arrives; otherwise those objects can
        // enter TOPPLED state but have nowhere to retain pitch/roll.
        ObjectPhysicsComponent created;
        if (const ObjectFixedTransformComponent* fixed =
                ecs::try_get<ObjectFixedTransformComponent>(registry,
                                                              *entity)) {
            created.position = fixed->position;
            created.lastPublishedPosition = fixed->position;
            created.collisionStartPosition = fixed->position;
            created.yaw = fixed->yawRadians;
            created.lastPublishedYaw = fixed->yawRadians;
            created.hasAuthoritativePosition = fixed->authoritative;
        }
        created.stickToGround = true;
        created.allowToFall = false;
        created.sleeping = false;
        physics = &ecs::emplace<ObjectPhysicsComponent>(
            registry, *entity, std::move(created));
    } else {
        // Static props with authored PhysicsBehavior normally sleep. Topple
        // owns an attitude transition and must wake the projection pass.
        physics->sleeping = false;
    }
    if (RenderModelComponent* render =
            ecs::try_get<RenderModelComponent>(registry, *entity)) {
        // ToppleUpdate explicitly stops SwayClientUpdate for this drawable;
        // the shared environmental sway must no longer rotate the fallen
        // object around the authoritative topple attitude.
        render->treeSwayEnabled = false;
    }
    if (!ecs::try_get<ObjectShadowSuppressionComponent>(registry, *entity)) {
        ecs::emplace<ObjectShadowSuppressionComponent>(
            registry, *entity,
            ObjectShadowSuppressionComponent{.confirmedTick = confirmedTick});
    }
    markObjectDirty(registry, *entity,
                    ObjectDirtyDomain::RenderExtraction);
    return true;
}

void ObjectTacticalSystem::consumeToppleRequests(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    uint64_t confirmedTick,
    uint64_t& nextGameplaySubmissionOrdinal) const {
    ObjectToppleJournal* journal =
        registry.ctx().find<ObjectToppleJournal>();
    if (!journal || journal->pending.empty()) return;
    container::Vector<QueuedToppleRequest> pending =
        std::move(journal->pending);
    journal->pending.clear();
    container::Vector<QueuedToppleRequest> deferred;
    std::stable_sort(
        pending.begin(), pending.end(),
        [](const QueuedToppleRequest& left,
           const QueuedToppleRequest& right) {
            if (left.request.confirmedTick != right.request.confirmedTick) {
                return left.request.confirmedTick <
                    right.request.confirmedTick;
            }
            return left.submissionOrdinal < right.submissionOrdinal;
        });
    for (QueuedToppleRequest& queued : pending) {
        ObjectToppleRequest& request = queued.request;
        if (request.confirmedTick > confirmedTick) {
            deferred.push_back(std::move(queued));
            continue;
        }
        static_cast<void>(applyTopplingForce(
            registry, lifecycle, request.object,
            request.direction, request.speed,
            request.confirmedTick ? request.confirmedTick : confirmedTick,
            nextGameplaySubmissionOrdinal,
            request.noBounce, request.noFx));
    }
    if (!deferred.empty()) {
        journal->pending.insert(
            journal->pending.begin(),
            std::make_move_iterator(deferred.begin()),
            std::make_move_iterator(deferred.end()));
    }
}

void ObjectTacticalSystem::onHealthEvent(
    ecs::registry&, const ObjectLifecycle&, ObjectId,
    math::q32_32, uint64_t) const {
    // ProneUpdate is absent from all active stock recipes; its six textual
    // occurrences are commented-out INI. Keep the damage hook explicit so a
    // future product decision can add it without reopening Body transaction
    // ordering, but do not fabricate runtime state for inactive content.
}

bool ObjectTacticalSystem::onBattlePlanDelete(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint32_t authoredOrder,
    const ObjectSimulationRules& rules,
    ObjectUpgradeExecutionContext context,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    ObjectTacticalComponent* component = entity
        ? ecs::try_get<ObjectTacticalComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan) return false;
    const size_t count = std::min(component->battlePlans.size(),
                                  component->plan->battlePlans.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectBattlePlanRule& rule =
            component->plan->battlePlans[index];
        if (rule.authoredOrder != authoredOrder) continue;
        ObjectBattlePlanRuntime& runtime = component->battlePlans[index];
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, *entity);
        if (runtime.transition == ObjectBattlePlanTransition::Active &&
            runtime.current != game::ObjectBattlePlanStatus::None) {
            if (runtime.current ==
                game::ObjectBattlePlanStatus::SearchAndDestroy) {
                appendBattlePlanPresentationEvent(
                    m_battlePlanPresentationEvents,
                    ObjectBattlePlanPresentationPhase::Packing, object,
                    owner ? owner->player : INVALID_PLAYER_ID,
                    rule, runtime.current, confirmedTick);
            }
            applyStrategyCenterBattlePlan(
                registry, *entity, rule, runtime.current, false, rules,
                confirmedTick);
            runtime.desired = game::ObjectBattlePlanStatus::None;
            runtime.current = game::ObjectBattlePlanStatus::None;
            runtime.transition = ObjectBattlePlanTransition::Idle;
            runtime.nextTransitionTick = confirmedTick;
            rebuildBattlePlanProjections(
                registry, lifecycle, context.content, rules, context.random,
                confirmedTick);
        }
        return true;
    }
    return false;
}

void ObjectTacticalSystem::onSpecialAbilityReclaim(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& damageRequests) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    ObjectTacticalComponent* component = entity
        ? ecs::try_get<ObjectTacticalComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan) return;
    const size_t count = std::min(component->specialAbilities.size(),
                                  component->plan->specialAbilities.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectSpecialAbilityRuntime& runtime =
            component->specialAbilities[index];
        const game::ObjectSpecialAbilityUpdateRule& rule =
            component->plan->specialAbilities[index];
        if (!rule.specialObjectsPersistWhenOwnerDies) {
            for (const ObjectSpecialAbilityObject& special :
                 runtime.specialObjects) {
                if (!special.object ||
                    !lifecycle.entityFromId(special.object)) continue;
                damageRequests.push_back({
                    .target = special.object,
                    .source = object,
                    .sourceSequence = rule.authoredOrder,
                    .forceKill = true,
                    .confirmedTick = confirmedTick,
                });
            }
        }
        runtime.specialObjects.clear();
        runtime.active = false;
        runtime.phase = ObjectSpecialAbilityPhase::Inactive;
    }
}

bool ObjectTacticalSystem::onPropagandaTowerDie(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry* players, const GameContentSnapshot* content,
    const ObjectSimulationRules& rules, SimulationRandom* random,
    ObjectId object, uint32_t authoredOrder,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> sourceEntity =
        lifecycle.entityFromIdIncludingPending(object);
    ObjectTacticalComponent* source = sourceEntity
        ? ecs::try_get<ObjectTacticalComponent>(registry, *sourceEntity)
        : nullptr;
    if (!source || !source->plan) return false;

    size_t dyingRuleIndex = source->plan->propagandaTowers.size();
    for (size_t index = 0;
         index < source->plan->propagandaTowers.size() &&
         index < source->propagandaTowers.size(); ++index) {
        if (source->plan->propagandaTowers[index].authoredOrder ==
            authoredOrder) {
            dyingRuleIndex = index;
            break;
        }
    }
    if (dyingRuleIndex >= source->propagandaTowers.size()) return false;

    container::Vector<ObjectId> affected =
        std::move(source->propagandaTowers[dyingRuleIndex].members);
    source->propagandaTowers[dyingRuleIndex].members.clear();
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()),
                   affected.end());

    for (const ObjectId targetId : affected) {
        const std::optional<ecs::entity> target =
            lifecycle.entityFromId(targetId);
        if (!target) continue;

        if (ObjectPropagandaBenefactorComponent* benefactor =
                ecs::try_get<ObjectPropagandaBenefactorComponent>(
                    registry, *target);
            benefactor && benefactor->source == object) {
            ecs::remove<ObjectPropagandaBenefactorComponent>(
                registry, *target);
        }

        game::WeaponBonusConditionMask desired = 0;
        if (content && hasAnyDamageWeapon(registry, *target, *content)) {
            const auto towers = ecs::view<
                const ObjectIdentityComponent, const OwnerComponent,
                const ObjectTacticalComponent>(registry);
            for (const ecs::entity candidate : towers) {
                const ObjectId candidateId = towers.template get<
                    const ObjectIdentityComponent>(candidate).id;
                if (!alive(registry, lifecycle, candidateId, candidate))
                    continue;
                const OwnerComponent& owner = towers.template get<
                    const OwnerComponent>(candidate);
                const ObjectTacticalComponent& tactical = towers.template get<
                    const ObjectTacticalComponent>(candidate);
                if (!tactical.plan) continue;
                const ObjectUpgradeInventoryComponent* objectUpgrades =
                    ecs::try_get<ObjectUpgradeInventoryComponent>(
                        registry, candidate);
                const size_t count = std::min(
                    tactical.plan->propagandaTowers.size(),
                    tactical.propagandaTowers.size());
                for (size_t index = 0; index < count; ++index) {
                    const ObjectPropagandaTowerRuntime& runtime =
                        tactical.propagandaTowers[index];
                    if (!std::binary_search(runtime.members.begin(),
                                            runtime.members.end(), targetId)) {
                        continue;
                    }
                    const game::ObjectPropagandaTowerRule& rule =
                        tactical.plan->propagandaTowers[index];
                    desired |= game::weaponBonusConditionBit(
                        game::WeaponBonusCondition::Enthusiastic);
                    const bool upgraded = rule.upgradeRequiredId &&
                        ((players && owner.player &&
                          players->hasUpgradeComplete(
                              owner.player, rule.upgradeRequiredId)) ||
                         (objectUpgrades && upgradeMaskTest(
                              objectUpgrades->completed,
                              rule.upgradeRequiredId)));
                    if (upgraded) {
                        desired |= game::weaponBonusConditionBit(
                            game::WeaponBonusCondition::Subliminal);
                    }
                }
            }
        }

        ObjectPropagandaAuraProjection* projection =
            ecs::try_get<ObjectPropagandaAuraProjection>(registry, *target);
        const game::WeaponBonusConditionMask previous =
            projection ? projection->conditions : 0;
        for (const game::WeaponBonusCondition condition : {
                game::WeaponBonusCondition::Enthusiastic,
                game::WeaponBonusCondition::Subliminal}) {
            const game::WeaponBonusConditionMask bit =
                game::weaponBonusConditionBit(condition);
            if ((previous & bit) == (desired & bit)) continue;
            static_cast<void>(setObjectWeaponBonusCondition(
                registry, *target, condition, (desired & bit) != 0,
                content, random, rules.logicFramesPerSecond,
                confirmedTick));
        }
        if (desired == 0) {
            if (projection) {
                ecs::remove<ObjectPropagandaAuraProjection>(
                    registry, *target);
            }
        } else if (projection) {
            projection->conditions = desired;
        } else {
            ecs::emplace<ObjectPropagandaAuraProjection>(
                registry, *target, desired);
        }
    }
    return true;
}


} // namespace engine
