#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/structure/ObjectBridge.h"

#include "core/container/string_utils.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/terrain/TerrainLogic.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include "game/object/simulation/containment/ObjectContainmentDetail.h"

namespace engine {
using namespace object_containment_detail;

bool ObjectContainmentSystem::attach(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentAttachRequest& request) const {
    if (!canAttach(registry, lifecycle, request)) return false;
    const std::optional<ecs::entity> container =
        lifecycle.entityFromId(request.container);
    const std::optional<ecs::entity> object =
        lifecycle.entityFromId(request.object);
    if (!container || !object) return false;

    ObjectMapStatusComponent* mapStatus =
        ecs::try_get<ObjectMapStatusComponent>(registry, *object);
    const bool hadMapStatus = mapStatus != nullptr;
    const bool previousOffMap = mapStatus && mapStatus->offMap;
    if (request.enclosing) {
        if (!mapStatus)
            mapStatus = &ecs::emplace<ObjectMapStatusComponent>(registry,
                                                                *object);
        mapStatus->offMap = true;
    } else if (mapStatus) {
        // A visible host attachment is an in-world object by definition.
        // Preserve the prior value on the edge so an explicit later Detach
        // can restore a deliberately off-map creation state.
        mapStatus->offMap = false;
    }

    ecs::emplace<ObjectContainedByComponent>(registry, *object,
        ObjectContainedByComponent{
            .container = request.container,
            .containmentRuleIndex = request.containmentRuleIndex,
            .confirmedEnteredTick = request.confirmedEnteredTick,
            .destroyWithContainer = request.destroyWithContainer,
            .enclosing = request.enclosing,
            .followsContainerTransform = request.followsContainerTransform,
            .hadMapStatus = hadMapStatus,
            .previousOffMap = previousOffMap,
        });
    ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, *container);
    if (!contents)
        contents = &ecs::emplace<ObjectContainmentComponent>(registry, *container);
    const auto position = std::lower_bound(
        contents->objects.begin(), contents->objects.end(), request.object,
        [](const ObjectContainedObjectRecord& record, ObjectId id) {
            return record.object < id;
        });
    const uint64_t entryOrdinal = contents->nextEntryOrdinal++;
    if (contents->nextEntryOrdinal == 0) {
        ++contents->nextEntryOrdinal;
    }
    contents->objects.insert(position, {
        .object = request.object,
        .confirmedEnteredTick = request.confirmedEnteredTick,
        .entryOrdinal = entryOrdinal,
        .destroyWithContainer = request.destroyWithContainer,
    });
    bumpRevision(*contents);
    ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *container);
    const ObjectContainmentRule* rule = runtime && runtime->plan &&
            request.containmentRuleIndex < runtime->plan->rules.size()
        ? &runtime->plan->rules[request.containmentRuleIndex] : nullptr;
    if (rule && rule->kind == ObjectContainmentKind::MobNexus &&
        contents->objects.size() == 1u) {
        const game::ModelConditionMask loaded =
            game::modelConditionMaskOf(game::ModelConditionFlag::Loaded);
        publishObjectModelConditionContribution(
            registry, *container,
            ObjectModelConditionContributionSource::Containment,
            loaded, loaded, request.confirmedEnteredTick);
    }
    if (request.followsContainerTransform ||
        (rule && rule->kind == ObjectContainmentKind::Garrison &&
         request.enclosing)) {
        synchronizeOne(registry, *object, *container);
    }
    if (rule && rule->kind != ObjectContainmentKind::Open &&
        (request.enclosing ||
         rule->kind == ObjectContainmentKind::Garrison)) {
        static_cast<void>(ObjectDisabledSystem::setUntil(
            registry, *object, ObjectDisabledReason::Held,
            OBJECT_DISABLED_FOREVER_TICK, request.confirmedEnteredTick));
    }
    if (rule && rule->kind == ObjectContainmentKind::Garrison) {
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *container,
            {.setMask = game::objectStatusBit(
                 game::ObjectStatusFlag::CanAttack),
             .confirmedTick = request.confirmedEnteredTick}));
    }
    if (rule && (rule->kind == ObjectContainmentKind::Garrison ||
                 rule->kind == ObjectContainmentKind::Helix)) {
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, *object, game::WeaponBonusCondition::Garrisoned, true,
            nullptr, nullptr, 30u, request.confirmedEnteredTick));
    }
    if (rule && rule->kind == ObjectContainmentKind::RiderChange) {
        runtime->activeRiderRule = std::numeric_limits<uint32_t>::max();
        const ThingTemplateComponent* passengerType =
            ecs::try_get<ThingTemplateComponent>(registry, *object);
        if (passengerType && passengerType->archetype) {
            for (size_t riderIndex = 0; riderIndex < rule->riders.size();
                 ++riderIndex) {
                if (templateMatchesRider(
                        passengerType,
                        rule->riders[riderIndex].templateName)) {
                    runtime->activeRiderRule =
                        static_cast<uint32_t>(riderIndex);
                    break;
                }
            }
        }
        if (runtime->activeRiderRule < rule->riders.size()) {
            const ObjectContainmentRule::RiderInfo& rider =
                rule->riders[runtime->activeRiderRule];
            if (RenderModelComponent* render =
                    ecs::try_get<RenderModelComponent>(registry,
                                                       *container)) {
                const game::ModelConditionMask model =
                    game::parseModelConditionMask(rider.modelCondition);
                for (size_t word = 0; word < model.words.size(); ++word)
                    render->modelConditionFlags.words[word] |=
                        model.words[word];
            }
            if (ObjectCombatProfileComponent* combat =
                    ecs::try_get<ObjectCombatProfileComponent>(registry,
                                                               *container)) {
                static_cast<void>(game::applyWeaponSetConditions(
                    rider.weaponSetCondition, combat->weaponConditions));
            }
            const game::ObjectStatusMaskParseResult riderStatus =
                game::parseObjectStatusMask(rider.objectStatus);
            if (riderStatus.resolved) {
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, *container,
                    {.setMask = riderStatus.mask,
                     .confirmedTick = request.confirmedEnteredTick}));
            }
            ObjectCommandSetOverrideComponent* commandSet =
                ecs::try_get<ObjectCommandSetOverrideComponent>(registry,
                                                                 *container);
            if (!commandSet)
                commandSet = &ecs::emplace<
                    ObjectCommandSetOverrideComponent>(registry, *container);
            commandSet->name = rider.commandSet;
            ++commandSet->revision;
            commandSet->lastAppliedTick = request.confirmedEnteredTick;
        }
        runtime->riderScuttleTick = 0;
        const game::ObjectStatusMask scuttleStatus =
            game::objectStatusBit(game::ObjectStatusFlag::Unselectable) |
            game::objectStatusBit(game::ObjectStatusFlag::Immobile);
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *container,
            {.clearMask = scuttleStatus,
             .confirmedTick = request.confirmedEnteredTick}));
        if (RenderModelComponent* render =
                ecs::try_get<RenderModelComponent>(registry, *container))
            render->modelConditionFlags.clear(
                game::parseModelConditionMask(rule->scuttleStatus));
    }
    if (rule && rule->kind == ObjectContainmentKind::Parachute) {
        runtime->parachuteHasStartZ = false;
        runtime->parachuteOpened = false;
        runtime->parachuteOpenLocomotorProjected = false;
        runtime->parachuteHasLandingOverride = false;
        runtime->parachuteHasLandingTarget = false;
        runtime->parachutePitch = {};
        runtime->parachuteRoll = {};
        runtime->parachutePitchRate = deterministicVariance(
            rule->pitchRateMax,
            (static_cast<uint64_t>(request.container.value) << 32u) ^
                request.object.value ^ request.confirmedEnteredTick);
        runtime->parachuteRollRate = deterministicVariance(
            rule->rollRateMax,
            (static_cast<uint64_t>(request.object.value) << 32u) ^
                request.container.value ^ request.confirmedEnteredTick);
        const game::ObjectStatusMask parachuting =
            game::objectStatusBit(game::ObjectStatusFlag::Parachuting) |
            game::objectStatusBit(game::ObjectStatusFlag::NoCollisions);
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *container,
            {.setMask = parachuting,
             .confirmedTick = request.confirmedEnteredTick}));
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *object,
            {.setMask = parachuting,
             .confirmedTick = request.confirmedEnteredTick}));
        static_cast<void>(ObjectDisabledSystem::setUntil(
            registry, *object, ObjectDisabledReason::Held,
            OBJECT_DISABLED_FOREVER_TICK, request.confirmedEnteredTick));
        if (RenderModelComponent* parachuteRender =
                ecs::try_get<RenderModelComponent>(registry, *container))
            parachuteRender->hidden = true;
        if (RenderModelComponent* riderRender =
                ecs::try_get<RenderModelComponent>(registry, *object)) {
            const game::ModelConditionMask freefall =
                game::modelConditionMaskOf(game::ModelConditionFlag::FreeFall);
            for (size_t word = 0; word < freefall.words.size(); ++word)
                riderRender->modelConditionFlags.words[word] |=
                    freefall.words[word];
        }
    }
    markObjectDirty(registry, *object, kObjectDirtyAll);
    markObjectDirty(
        registry, *container,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    return true;
}

bool ObjectContainmentSystem::detach(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    const ObjectContainedByComponent* stored =
        ecs::try_get<ObjectContainedByComponent>(registry, *entity);
    if (!stored || !stored->container) return false;
    const ObjectContainedByComponent edge = *stored;
    ecs::remove<ObjectContainedByComponent>(registry, *entity);

    if (ObjectMapStatusComponent* status =
            ecs::try_get<ObjectMapStatusComponent>(registry, *entity)) {
        if (edge.hadMapStatus) {
            status->offMap = edge.previousOffMap;
        } else if (edge.enclosing) {
            ecs::remove<ObjectMapStatusComponent>(registry, *entity);
        }
    }
    const std::optional<ecs::entity> container =
        lifecycle.entityFromIdIncludingPending(edge.container);
    ObjectContainmentRuntimeComponent* runtime = container
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                          *container)
        : nullptr;
    const ObjectContainmentRule* rule = runtime && runtime->plan &&
            edge.containmentRuleIndex < runtime->plan->rules.size()
        ? &runtime->plan->rules[edge.containmentRuleIndex] : nullptr;
    if (container) {
        if (ObjectGarrisonFirePointComponent* firePoints =
                ecs::try_get<ObjectGarrisonFirePointComponent>(
                    registry, *container)) {
            const auto found = std::lower_bound(
                firePoints->assignments.begin(),
                firePoints->assignments.end(), object,
                [](const ObjectGarrisonFirePointAssignment& assignment,
                   ObjectId id) {
                    return assignment.occupant < id;
                });
            if (found != firePoints->assignments.end() &&
                found->occupant == object) {
                firePoints->assignments.erase(found);
                ++firePoints->revision;
            }
        }
    }
    if (rule && rule->kind == ObjectContainmentKind::Parachute) {
        // ParachuteContain is a visible, in-world attachment.  Landing (and
        // chute loss) must not restore an off-map creation state captured
        // before the rider was attached to the chute.
        if (ObjectMapStatusComponent* status =
                ecs::try_get<ObjectMapStatusComponent>(registry, *entity)) {
            status->offMap = false;
        }
        // RefCode's onRemoving temporarily overrides stick-to-ground and
        // applies a zero force so Physics observes the release immediately.
        if (ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, *entity)) {
            physics->allowToFall = true;
            physics->sleeping = false;
        }
    }
    if (container && rule &&
        rule->kind == ObjectContainmentKind::MobNexus) {
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry, *container);
        if (airborne && airborne->isAirborne) {
            if (ObjectPhysicsComponent* physics =
                    ecs::try_get<ObjectPhysicsComponent>(registry, *entity)) {
                physics->allowToFall = true;
                physics->sleeping = false;
            }
        }
    }
    if (rule && rule->kind != ObjectContainmentKind::Open &&
        (edge.enclosing || rule->kind == ObjectContainmentKind::Garrison)) {
        static_cast<void>(ObjectDisabledSystem::clear(
            registry, *entity, ObjectDisabledReason::Held,
            confirmedTick));
    }
    if (container) {
        if (ObjectContainmentComponent* contents =
                ecs::try_get<ObjectContainmentComponent>(registry, *container)) {
            const auto found = std::lower_bound(
                contents->objects.begin(), contents->objects.end(), object,
                [](const ObjectContainedObjectRecord& record, ObjectId id) {
                    return record.object < id;
                });
            if (found != contents->objects.end() && found->object == object) {
                contents->objects.erase(found);
                bumpRevision(*contents);
                if (rule &&
                    rule->kind == ObjectContainmentKind::MobNexus &&
                    contents->objects.empty()) {
                    const game::ModelConditionMask loaded =
                        game::modelConditionMaskOf(game::ModelConditionFlag::Loaded);
                    publishObjectModelConditionContribution(
                        registry, *container,
                        ObjectModelConditionContributionSource::Containment,
                        loaded, {}, confirmedTick);
                }
            }
        }
    }
    if (container && rule &&
        rule->kind == ObjectContainmentKind::Garrison) {
        bool hasGarrisonOccupants = false;
        const ObjectContainmentComponent* remaining =
            ecs::try_get<ObjectContainmentComponent>(registry, *container);
        if (remaining && runtime && runtime->plan) {
            for (const ObjectContainedObjectRecord& record :
                 remaining->objects) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromIdIncludingPending(record.object);
                const ObjectContainedByComponent* passengerEdge = passenger
                    ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                *passenger)
                    : nullptr;
                if (passengerEdge &&
                    passengerEdge->containmentRuleIndex <
                        runtime->plan->rules.size() &&
                    runtime->plan->rules[
                        passengerEdge->containmentRuleIndex].kind ==
                        ObjectContainmentKind::Garrison) {
                    hasGarrisonOccupants = true;
                    break;
                }
            }
        }
        if (!hasGarrisonOccupants) {
            static_cast<void>(ObjectStatusSystem::apply(
                registry, *container,
                {.clearMask = game::objectStatusBit(
                     game::ObjectStatusFlag::CanAttack),
                 .confirmedTick = confirmedTick}));
        }
    }
    if (rule && (rule->kind == ObjectContainmentKind::Garrison ||
                 rule->kind == ObjectContainmentKind::Helix)) {
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, *entity, game::WeaponBonusCondition::Garrisoned, false,
            nullptr, nullptr, 30u, confirmedTick));
    }
    if (rule && rule->kind == ObjectContainmentKind::RiderChange) {
        if (runtime->activeRiderRule < rule->riders.size() && container) {
            const ObjectContainmentRule::RiderInfo& rider =
                rule->riders[runtime->activeRiderRule];
            if (RenderModelComponent* render =
                    ecs::try_get<RenderModelComponent>(registry,
                                                       *container)) {
                render->modelConditionFlags.clear(
                    game::parseModelConditionMask(rider.modelCondition));
            }
            if (ObjectCombatProfileComponent* combat =
                    ecs::try_get<ObjectCombatProfileComponent>(registry,
                                                               *container)) {
                game::WeaponSetConditionMask riderWeapons = 0;
                static_cast<void>(game::applyWeaponSetConditions(
                    rider.weaponSetCondition, riderWeapons));
                combat->weaponConditions &= ~riderWeapons;
            }
            const game::ObjectStatusMaskParseResult riderStatus =
                game::parseObjectStatusMask(rider.objectStatus);
            if (riderStatus.resolved) {
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, *container,
                    {.clearMask = riderStatus.mask,
                     .confirmedTick = confirmedTick}));
            }
            if (ObjectCommandSetOverrideComponent* commandSet =
                    ecs::try_get<ObjectCommandSetOverrideComponent>(
                        registry, *container)) {
                commandSet->name.clear();
                ++commandSet->revision;
                commandSet->lastAppliedTick = confirmedTick;
            }
        }
        runtime->activeRiderRule = std::numeric_limits<uint32_t>::max();
    }
    if (rule && rule->kind == ObjectContainmentKind::Parachute) {
        const game::ObjectStatusMask parachuting =
            game::objectStatusBit(game::ObjectStatusFlag::Parachuting) |
            game::objectStatusBit(game::ObjectStatusFlag::NoCollisions);
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *entity,
            {.clearMask = parachuting,
             .confirmedTick = confirmedTick}));
        static_cast<void>(ObjectDisabledSystem::clear(
            registry, *entity, ObjectDisabledReason::Held,
            confirmedTick));
        if (container) {
            static_cast<void>(ObjectStatusSystem::apply(
                registry, *container,
                {.clearMask = parachuting,
                 .confirmedTick = confirmedTick}));
        }
        if (RenderModelComponent* riderRender =
                ecs::try_get<RenderModelComponent>(registry, *entity)) {
            riderRender->modelConditionFlags.clear(
                game::modelConditionMaskOf(game::ModelConditionFlag::FreeFall, game::ModelConditionFlag::Parachuting));
        }
    }
    markObjectDirty(registry, *entity, kObjectDirtyAll);
    if (container) {
        markObjectDirty(
            registry, *container,
            objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    }
    return true;
}

container::Vector<ObjectId> ObjectContainmentSystem::captureDependents(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId container) const {
    container::Vector<ObjectId> result;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(container);
    if (!entity) return result;

    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *entity);
    if (!runtime || !runtime->plan || runtime->plan->rules.empty()) {
        return result;
    }
    const ObjectContainmentKind kind = runtime->plan->rules.front().kind;
    if (kind == ObjectContainmentKind::Cave && runtime->hasCave) {
        const auto view = ecs::view<const ObjectIdentityComponent,
                                    const ObjectContainmentRuntimeComponent>(
            registry);
        for (const ecs::entity candidate : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(candidate);
            const ObjectContainmentRuntimeComponent& other =
                view.template get<
                    const ObjectContainmentRuntimeComponent>(candidate);
            if (!identity.id || identity.id == container ||
                !isCompletedContainmentEntrance(registry, candidate) ||
                !other.hasCave ||
                other.caveIndex != runtime->caveIndex ||
                !lifecycle.entityFromId(identity.id)) continue;
            result.push_back(identity.id);
        }
        std::sort(result.begin(), result.end());
        return result;
    }
    if (kind != ObjectContainmentKind::Overlord &&
        kind != ObjectContainmentKind::Helix) return result;

    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, *entity);
    if (!contents) return result;

    // ObjectContainmentComponent is maintained in stable ObjectId order.
    // RefCode redirects Overlord capture through m_containList.front(), so
    // exactly the first live carried add-on participates in this recursion.
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        if (!record.object || !record.destroyWithContainer ||
            !lifecycle.entityFromId(record.object)) continue;
        result.push_back(record.object);
        break;
    }
    return result;
}

ObjectId ObjectContainmentSystem::recentTunnelNetworkNemesis(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId tunnelEntrance, uint64_t confirmedTick) const noexcept {
    const std::optional<ecs::entity> entrance =
        lifecycle.entityFromId(tunnelEntrance);
    if (!entrance) return INVALID_OBJECT_ID;
    const ObjectTunnelNetworkCombatHandoffComponent* handoff =
        ecs::try_get<ObjectTunnelNetworkCombatHandoffComponent>(registry,
                                                                 *entrance);
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, *entrance);
    if (!handoff || !owner || handoff->networkOwner != owner->player ||
        !handoff->recentNemesis ||
        confirmedTick > handoff->expiresTick ||
        !visibleTunnelNemesis(registry, lifecycle, handoff->recentNemesis)) {
        return INVALID_OBJECT_ID;
    }
    return handoff->recentNemesis;
}

bool ObjectContainmentSystem::publishTunnelNetworkNemesis(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId source, ObjectId target, uint64_t confirmedTick,
    uint32_t logicFramesPerSecond) const {
    const std::optional<ecs::entity> sourceEntity =
        lifecycle.entityFromId(source);
    const std::optional<ecs::entity> targetEntity =
        lifecycle.entityFromId(target);
    const OwnerComponent* sourceOwner = sourceEntity
        ? ecs::try_get<OwnerComponent>(registry, *sourceEntity) : nullptr;
    const ObjectKindOfComponent* targetKinds = targetEntity
        ? ecs::try_get<ObjectKindOfComponent>(registry, *targetEntity) : nullptr;
    if (!sourceOwner || !sourceOwner->player || !targetEntity ||
        !liveTunnelNemesis(registry, lifecycle, target) || !targetKinds ||
        !(game::objectHasKind(targetKinds->mask, game::ObjectKindOf::Vehicle) ||
          game::objectHasKind(targetKinds->mask, game::ObjectKindOf::Structure) ||
          game::objectHasKind(targetKinds->mask, game::ObjectKindOf::Infantry) ||
          game::objectHasKind(targetKinds->mask, game::ObjectKindOf::Aircraft))) {
        return false;
    }

    struct Entrance final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
        uint64_t createdAtTick = std::numeric_limits<uint64_t>::max();
    };
    container::Vector<Entrance> entrances;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectContainmentRuntimeComponent,
                                const OwnerComponent>(registry);
    entrances.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const OwnerComponent& owner =
            view.template get<const OwnerComponent>(entity);
        if (owner.player != sourceOwner->player ||
            !isCompletedContainmentEntrance(registry, entity)) {
            continue;
        }
        const ObjectContainmentRuntimeComponent& runtime =
            view.template get<const ObjectContainmentRuntimeComponent>(entity);
        const bool tunnel = runtime.plan && std::any_of(
            runtime.plan->rules.begin(), runtime.plan->rules.end(),
            [](const ObjectContainmentRule& rule) noexcept {
                return rule.kind == ObjectContainmentKind::Tunnel;
            });
        if (!tunnel) continue;
        const ObjectId entrance =
            view.template get<const ObjectIdentityComponent>(entity).id;
        if (entrance && lifecycle.entityFromId(entrance)) {
            const ObjectLifecycleComponent* lifecycleState =
                ecs::try_get<ObjectLifecycleComponent>(registry, entity);
            entrances.push_back({
                .object = entrance,
                .entity = entity,
                .createdAtTick = lifecycleState
                    ? lifecycleState->createdAtTick
                    : std::numeric_limits<uint64_t>::max(),
            });
        }
    }
    std::sort(entrances.begin(), entrances.end(),
              [](const Entrance& left, const Entrance& right) noexcept {
                  if (left.createdAtTick != right.createdAtTick)
                      return left.createdAtTick < right.createdAtTick;
                  return left.object < right.object;
              });
    if (entrances.empty()) return false;

    ObjectId current = INVALID_OBJECT_ID;
    for (const Entrance& entrance : entrances) {
        const ObjectTunnelNetworkCombatHandoffComponent* handoff =
            ecs::try_get<ObjectTunnelNetworkCombatHandoffComponent>(
                registry, entrance.entity);
        if (!handoff || handoff->networkOwner != sourceOwner->player ||
            !handoff->recentNemesis || confirmedTick > handoff->expiresTick ||
            !liveTunnelNemesis(registry, lifecycle,
                               handoff->recentNemesis)) {
            continue;
        }
        current = handoff->recentNemesis;
        break;
    }
    if (current && current != target) return false;

    const uint64_t ttl = static_cast<uint64_t>(
        logicFramesPerSecond == 0 ? 30u : logicFramesPerSecond) * 4u;
    const uint64_t expiresTick = saturatingAddTicks(confirmedTick, ttl);
    for (const Entrance& entrance : entrances) {
        ObjectTunnelNetworkCombatHandoffComponent* handoff =
            ecs::try_get<ObjectTunnelNetworkCombatHandoffComponent>(
                registry, entrance.entity);
        if (!handoff) {
            handoff = &ecs::emplace<ObjectTunnelNetworkCombatHandoffComponent>(
                registry, entrance.entity);
        }
        if (handoff->networkOwner == sourceOwner->player &&
            handoff->recentNemesis == target &&
            handoff->observedTick == confirmedTick &&
            handoff->expiresTick == expiresTick) {
            continue;
        }
        handoff->networkOwner = sourceOwner->player;
        handoff->recentNemesis = target;
        handoff->observedTick = confirmedTick;
        handoff->expiresTick = expiresTick;
        ++handoff->revision;
        if (handoff->revision == 0) ++handoff->revision;
    }
    return true;
}


} // namespace engine
