#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/structure/ObjectBridge.h"

#include "core/container/string_utils.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
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
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/lifecycle/ObjectDeleteWalk.h"

namespace engine {
using namespace object_containment_detail;

namespace {

[[nodiscard]] bool transportDerivedContainment(
    ObjectContainmentKind kind) noexcept {
    return kind == ObjectContainmentKind::Transport ||
        kind == ObjectContainmentKind::RiderChange ||
        kind == ObjectContainmentKind::Overlord ||
        kind == ObjectContainmentKind::Helix;
}

[[nodiscard]] bool hostAiAllowsRiderExit(
    const ecs::registry& registry, ecs::entity host, ecs::entity rider,
    const ObjectContainmentRuntimeComponent& containment) noexcept {
    const ObjectHealthComponent* hostHealth =
        ecs::try_get<ObjectHealthComponent>(registry, host);
    const size_t behaviorCount = containment.plan
        ? containment.plan->behaviorRules.size() : size_t{0};
    for (size_t index = 0; index < behaviorCount; ++index) {
        if (containment.plan->behaviorRules[index].kind !=
            ObjectTransportBehaviorKind::DeliverPayloadAI) {
            continue;
        }
        // DeliverPayloadAIUpdate::getAiFreeToExit first rejects an
        // effectively-dead carrier. DeliveringState does not raise its
        // m_freeToExit latch until DoorDelay has elapsed, so the visual door
        // transition alone is not sufficient.
        return hostHealth && !hostHealth->effectivelyDead &&
            !hostHealth->terminalDeathIssued &&
            index < containment.behaviorStates.size() &&
            containment.behaviorStates[index].deliveryFreeToExit;
    }

    const ObjectAirfieldComponent* airfield =
        ecs::try_get<ObjectAirfieldComponent>(registry, host);
    if (!airfield || !airfield->plan || airfield->plan->chinookAi.empty())
        return true;
    const ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(registry, host);
    if (!airborne || !airborne->isAirborne) return true;
    const ObjectKindOfComponent* riderKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, rider);
    const bool canRappel = riderKinds && game::objectHasKind(
        riderKinds->mask, game::ObjectKindOf::CanRappel);
    if (!canRappel) return false;
    return std::any_of(
        airfield->chinookAi.begin(), airfield->chinookAi.end(),
        [](const ObjectChinookAiRuntime& runtime) {
            return runtime.combatDropActive;
        });
}

[[nodiscard]] bool validRiderMovementTerrain(
    const ecs::registry& registry, ecs::entity host,
    const ObjectLocomotionComponent& riderLocomotion,
    const game::terrain::TerrainLogic*,
    const navigation::NavigationSystem* navigation) noexcept {
    if (!navigation || !navigation->isInitialized()) return false;
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, host);
    if (!transform) return false;
    const LogicFixedVec3 position = readAuthoritativeObjectPosition(
        registry, host, *transform);
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, host);
    navigation::NavigationLayerId layer;
    if (!navigation::tryNavigationLayerFromTerrainPathfindLayer(
            terrainLayer ? terrainLayer->pathfindLayer
                         : game::terrain::kGroundPathfindLayer,
            layer)) {
        return false;
    }
    // RefCode validMovementTerrain deliberately ignores obstacle occupancy.
    // The static grid retains the underlying terrain surface while excluding
    // the dynamic building/unit overlay.
    const navigation::NavigationGrid* grid =
        navigation->staticLayers().find(layer);
    if (!grid) return false;
    const navigation::NavigationCellId cell = grid->cellAt({
        position.x.raw(), position.y.raw(), position.z.raw()});
    if (!cell) return false;
    const navigation::NavigationMovementMask movement =
        grid->cell(cell).movementMask;
    // Legacy obstacle/impassable cells are accepted without testing the
    // rider surface. Navigation encodes those terrain-neutral cells as AIR.
    if (movement == navigation::NavigationMovement::Air) return true;
    return (movement & static_cast<navigation::NavigationMovementMask>(
        riderLocomotion.surfaces)) != 0;
}

[[nodiscard]] bool isSpecificRiderFreeToExit(
    const ecs::registry& registry, ecs::entity host, ecs::entity rider,
    const ObjectContainmentRuntimeComponent& containment,
    const ObjectContainmentRule& rule,
    uint64_t confirmedTick,
    const game::terrain::TerrainLogic* terrain,
    const navigation::NavigationSystem* navigation) noexcept {
    if (rule.railedDockOwnsExit) {
        return railedTransportDockAllowsContainment(registry, host);
    }
    if (!hostAiAllowsRiderExit(registry, host, rider, containment))
        return false;
    // ZH's TransportContain rejects an exit when the containing transport is
    // itself HELD by another container.  The rider's ContainedBy points at
    // this host, so the disabled query belongs on the host rather than on the
    // rider edge.
    if (isObjectDisabledBy(registry, host, ObjectDisabledReason::Held,
                           confirmedTick)) {
        return false;
    }
    const ObjectLocomotionComponent* hostLocomotion =
        ecs::try_get<ObjectLocomotionComponent>(registry, host);
    if (hostLocomotion &&
        (hostLocomotion->surfaces & game::locomotorSurfaceBit(
             game::LocomotorSurface::Air)) != 0) {
        return true;
    }
    const ThingTemplateComponent* riderTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, rider);
    if (!riderTemplate || !riderTemplate->archetype ||
        !riderTemplate->archetype->hasAiUpdate) {
        return false;
    }
    const ObjectLocomotionComponent* riderLocomotion =
        ecs::try_get<ObjectLocomotionComponent>(registry, rider);
    if (!riderLocomotion || riderLocomotion->surfaces == 0)
        return false;
    return validRiderMovementTerrain(
        registry, host, *riderLocomotion, terrain, navigation);
}

} // namespace

bool ObjectContainmentSystem::requestDetach(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentRequest& request,
    container::Vector<ObjectContainmentEvent>& events) const {
    ObjectId container = request.container;
    std::optional<ecs::entity> containerEntity;
    const ObjectContainmentRule* rule = nullptr;
    if (!container) {
        if (const std::optional<ecs::entity> entity =
                lifecycle.entityFromIdIncludingPending(request.object)) {
            if (const ObjectContainedByComponent* edge =
                    ecs::try_get<ObjectContainedByComponent>(registry, *entity)) {
                container = edge->container;
            }
        }
    }
    if (container) {
        containerEntity = lifecycle.entityFromIdIncludingPending(container);
        if (containerEntity) {
            const std::optional<ecs::entity> objectEntity =
                lifecycle.entityFromIdIncludingPending(request.object);
            if (objectEntity) {
                rule = findContainmentRuleForObject(
                    registry, *containerEntity, *objectEntity);
            }
        }
    }
    ObjectContainmentRuntimeComponent* runtime = containerEntity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                          *containerEntity)
        : nullptr;
    const uint32_t ruleIndex = runtime && runtime->plan && rule
        ? static_cast<uint32_t>(rule - runtime->plan->rules.data())
        : std::numeric_limits<uint32_t>::max();
    // RiderChangeContain overrides TransportContain::isExitBusy() and never
    // serializes rider replacement/exits on the transport exit-delay clock.
    // Keep that semantic at the request boundary; otherwise a stale
    // exitNotBusyTick from a prior ordinary transport operation can reject a
    // valid rider dismount even though RefCode accepts it immediately.
    if (!request.force && runtime && rule &&
        rule->kind != ObjectContainmentKind::RiderChange &&
        request.confirmedTick < runtime->exitNotBusyTick) {
        pushContainmentEvent(events, ObjectContainmentRequestKind::Detach,
                             container, request.object,
                             request.confirmedTick, false);
        return false;
    }
    if (!request.force && rule && rule->delayExitInAir && containerEntity) {
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry,
                                                   *containerEntity);
        if (airborne && airborne->isAirborne) {
            pushContainmentEvent(events,
                                 ObjectContainmentRequestKind::Detach,
                                 container, request.object,
                                 request.confirmedTick, false);
            return false;
        }
    }
    if (!request.force && container) {
        if (containerEntity &&
            !railedTransportDockAllowsContainment(registry, *containerEntity)) {
            pushContainmentEvent(events, ObjectContainmentRequestKind::Detach,
                                 container, request.object,
                                 request.confirmedTick, false);
            return false;
        }
    }
    const bool accepted = detach(
        registry, lifecycle, request.object, request.confirmedTick);
    if (accepted && rule && runtime) {
        // TransportContain::onRemoving pushes the passenger to an aggressive
        // stance before any exit path is chosen, so an AI drop force engages
        // instead of standing on the drop point.  RailedTransportContain,
        // RiderChangeContain and InternetHackContain all derive from
        // TransportContain, so this must not sit behind the railed-dock exit
        // branch further down.  RefCode guards on rider->getAI(); the modern
        // equivalent is the archetype AI-update fact, and the policy component
        // is only materialized for non-default values (see
        // GameSessionObjectLifecycleTransactions).
        if (rule->goAggressiveOnExit) {
            const std::optional<ecs::entity> riderEntity =
                lifecycle.entityFromIdIncludingPending(request.object);
            const ThingTemplateComponent* riderTemplate = riderEntity
                ? ecs::try_get<ThingTemplateComponent>(registry, *riderEntity)
                : nullptr;
            if (riderTemplate && riderTemplate->archetype &&
                riderTemplate->archetype->hasAiUpdate) {
                if (ObjectAIBehaviorPolicyComponent* policy =
                        ecs::try_get<ObjectAIBehaviorPolicyComponent>(
                            registry, *riderEntity)) {
                    if (policy->attitude != ObjectAIAttitude::Aggressive) {
                        policy->attitude = ObjectAIAttitude::Aggressive;
                        ++policy->revision;
                        if (policy->revision == 0) policy->revision = 1;
                    }
                } else {
                    ecs::emplace<ObjectAIBehaviorPolicyComponent>(
                        registry, *riderEntity,
                        ObjectAIBehaviorPolicyComponent{
                            .attitude = ObjectAIAttitude::Aggressive,
                        });
                }
            }
        }
        // TransportContain::onRemoving invokes wakeUpAndAttemptToTarget after
        // the rider is detached.  Preserve its exact narrow meaning: only AI
        // actors receive an edge, and ObjectAIRuntime decides whether the
        // actor is currently Idle before resetting the scan deadline.
        if (rule->resetMoodCheckTimeOnExit) {
            const std::optional<ecs::entity> riderEntity =
                lifecycle.entityFromIdIncludingPending(request.object);
            const ThingTemplateComponent* riderTemplate = riderEntity
                ? ecs::try_get<ThingTemplateComponent>(registry, *riderEntity)
                : nullptr;
            if (riderEntity && riderTemplate && riderTemplate->archetype &&
                riderTemplate->archetype->hasAiUpdate) {
                if (ObjectAITargetScanWakeComponent* wake =
                        ecs::try_get<ObjectAITargetScanWakeComponent>(
                            registry, *riderEntity)) {
                    wake->requestedTick = request.confirmedTick;
                    ++wake->revision;
                    if (wake->revision == 0) ++wake->revision;
                } else {
                    ecs::emplace<ObjectAITargetScanWakeComponent>(
                        registry, *riderEntity,
                        ObjectAITargetScanWakeComponent{
                            .requestedTick = request.confirmedTick,
                        });
                }
            }
        }
        // TransportContain::m_exitDelay alone serializes passenger exits.
        // OpenContain::DoorOpenTime is a visual door-close countdown and must
        // never stall the next passenger (notably the stock Ferry authors a
        // 2000 ms door time but no ExitDelay).
        const uint32_t delayMilliseconds = rule->exitDelayMilliseconds;
        // The legacy logic clock is fixed at 30 Hz for object simulation. The
        // request API intentionally carries confirmed ticks, not wall time.
        runtime->exitNotBusyTick = saturatingAddTicks(
            request.confirmedTick,
            millisecondsToTicks(delayMilliseconds, 30u));
        if (rule->doorOpenTimeMilliseconds != 0 && containerEntity) {
            runtime->doorCloseTick = saturatingAddTicks(
                request.confirmedTick,
                millisecondsToTicks(rule->doorOpenTimeMilliseconds, 30u));
            projectContainmentDoorTransition(registry, *containerEntity, true,
                                             request.confirmedTick);
        }
        const uint32_t pathCount = rule->numberOfExitPaths;
        const uint32_t selectedExitPath = pathCount == 0 ? 1u
            : std::clamp(runtime->nextExitPath, 1u, pathCount);
        const ObjectContainmentExitPath* precisePath = nullptr;
        if (!rule->railedDockOwnsExit &&
            ruleIndex < runtime->exitPathSets.size()) {
            const ObjectContainmentExitPathSet& pathSet =
                runtime->exitPathSets[ruleIndex];
            if (selectedExitPath > 0 &&
                selectedExitPath <= pathSet.paths.size() &&
                pathSet.paths[selectedExitPath - 1u].valid) {
                precisePath = &pathSet.paths[selectedExitPath - 1u];
            }
        }
        if (pathCount != 0) {
            runtime->nextExitPath = selectedExitPath >= pathCount
                ? 1u : selectedExitPath + 1u;
        }
        const auto reserveCommandSequence = [&runtime]() {
            uint32_t sequence = runtime->nextExitCommandSequence++;
            if (sequence == 0)
                sequence = runtime->nextExitCommandSequence++;
            if (runtime->nextExitCommandSequence == 0)
                runtime->nextExitCommandSequence = 1;
            return sequence;
        };
        const uint32_t firstCommandSequence = reserveCommandSequence();
        const uint32_t secondCommandSequence = reserveCommandSequence();
        if (containerEntity && !rule->railedDockOwnsExit) {
            const std::optional<ecs::entity> objectEntity =
                lifecycle.entityFromIdIncludingPending(request.object);
            if (objectEntity) {
                applyTransportExitPolicy(registry, *containerEntity,
                                         *objectEntity, *rule,
                                         precisePath, selectedExitPath,
                                         ruleIndex, firstCommandSequence,
                                         secondCommandSequence,
                                         request.confirmedTick);
            }
        }
        if (rule->kind == ObjectContainmentKind::RiderChange &&
            containerEntity) {
            runtime->riderScuttleTick = std::max<uint64_t>(
                1u, saturatingAddTicks(
                    request.confirmedTick,
                    millisecondsToTicks(
                        rule->scuttleDelayMilliseconds, 30u)));
            const game::ObjectStatusMask scuttleStatus =
                game::objectStatusBit(
                    game::ObjectStatusFlag::Unselectable) |
                game::objectStatusBit(game::ObjectStatusFlag::Immobile);
            static_cast<void>(ObjectStatusSystem::apply(
                registry, *containerEntity,
                {.setMask = scuttleStatus,
                 .confirmedTick = request.confirmedTick}));
            if (RenderModelComponent* render =
                    ecs::try_get<RenderModelComponent>(registry,
                                                       *containerEntity)) {
                const game::ModelConditionMask scuttle =
                    game::parseModelConditionMask(rule->scuttleStatus);
                for (size_t word = 0; word < scuttle.words.size(); ++word)
                    render->modelConditionFlags.words[word] |=
                        scuttle.words[word];
            }
        }
    }
    pushContainmentEvent(events, ObjectContainmentRequestKind::Detach,
                         container, request.object, request.confirmedTick,
                         accepted,
                         accepted && request.exposeStealthUnits,
                         accepted ? request.parachuteLandingTransport
                                  : INVALID_OBJECT_ID);
    return accepted;
}

bool ObjectContainmentSystem::requestEjectAll(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentRequest& request,
    container::Vector<ObjectContainmentEvent>& events) const {
    const std::optional<ecs::entity> container =
        lifecycle.entityFromIdIncludingPending(request.container);
    if (!container) {
        pushContainmentEvent(events, ObjectContainmentRequestKind::EjectAll,
                             request.container, INVALID_OBJECT_ID,
                             request.confirmedTick, false);
        return false;
    }
    if (!request.force &&
        !railedTransportDockAllowsContainment(registry, *container)) {
        pushContainmentEvent(events, ObjectContainmentRequestKind::EjectAll,
                             request.container, INVALID_OBJECT_ID,
                             request.confirmedTick, false);
        return false;
    }
    container::Vector<ObjectContainedObjectRecord> contents;
    if (const ObjectContainmentComponent* component =
            ecs::try_get<ObjectContainmentComponent>(registry, *container))
        contents = component->objects;
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *container);
    const ObjectContainmentRule* networkRule = nullptr;
    if (runtime && runtime->plan) {
        const auto found = std::find_if(
            runtime->plan->rules.begin(), runtime->plan->rules.end(),
            [](const ObjectContainmentRule& rule) {
                return rule.kind == ObjectContainmentKind::Cave ||
                    rule.kind == ObjectContainmentKind::Tunnel;
            });
        if (found != runtime->plan->rules.end()) networkRule = &*found;
    }
    if (networkRule && runtime && runtime->plan) {
        contents.clear();
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, *container);
        const auto view = ecs::view<const ObjectContainmentRuntimeComponent,
                                    const ObjectContainmentComponent>(registry);
        for (const ecs::entity entrance : view) {
            if (!isCompletedContainmentEntrance(registry, entrance))
                continue;
            const ObjectContainmentRuntimeComponent& otherRuntime =
                view.template get<
                    const ObjectContainmentRuntimeComponent>(entrance);
            if (!otherRuntime.plan) continue;
            if (networkRule->kind == ObjectContainmentKind::Cave) {
                if (!runtime->hasCave || !otherRuntime.hasCave ||
                    runtime->caveIndex != otherRuntime.caveIndex) continue;
            } else {
                const OwnerComponent* otherOwner =
                    ecs::try_get<OwnerComponent>(registry, entrance);
                if (!owner || !otherOwner ||
                    owner->player != otherOwner->player) continue;
            }
            const ObjectContainmentComponent& otherContents =
                view.template get<const ObjectContainmentComponent>(entrance);
            for (const ObjectContainedObjectRecord& record :
                 otherContents.objects) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromId(record.object);
                const ObjectContainedByComponent* edge = passenger
                    ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                *passenger)
                    : nullptr;
                if (!edge || edge->containmentRuleIndex >=
                        otherRuntime.plan->rules.size() ||
                    otherRuntime.plan->rules[edge->containmentRuleIndex].kind !=
                        networkRule->kind) continue;
                contents.push_back(record);
            }
        }
        std::sort(contents.begin(), contents.end(),
                  [](const ObjectContainedObjectRecord& left,
                     const ObjectContainedObjectRecord& right) {
                      return left.object < right.object;
                  });
        contents.erase(std::unique(
            contents.begin(), contents.end(),
            [](const ObjectContainedObjectRecord& left,
               const ObjectContainedObjectRecord& right) {
                return left.object == right.object;
            }), contents.end());
    }
    bool any = false;
    for (const ObjectContainedObjectRecord& record : contents) {
        if (!record.object) continue;
        ObjectId actualContainer = request.container;
        if (networkRule) {
            const std::optional<ecs::entity> passenger =
                lifecycle.entityFromId(record.object);
            const ObjectContainedByComponent* edge = passenger
                ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                            *passenger)
                : nullptr;
            if (edge && edge->container) actualContainer = edge->container;
        }
        const bool accepted = requestDetach(
            registry, lifecycle,
            ObjectContainmentRequest{
                .kind = ObjectContainmentRequestKind::Detach,
                .container = actualContainer,
                .object = record.object,
                .confirmedTick = request.confirmedTick,
                // EjectAll is already admitted as one atomic operation above;
                // do not let the per-passenger exit delay serialize the batch.
                .force = true,
                .exposeStealthUnits = request.exposeStealthUnits,
            }, events);
        any = any || accepted;
    }
    pushContainmentEvent(events, ObjectContainmentRequestKind::EjectAll,
                         request.container, INVALID_OBJECT_ID,
                         request.confirmedTick, any);
    return any;
}


bool ObjectContainmentSystem::setParachuteLandingOverride(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId parachute, math::q32_32 x, math::q32_32 y,
    math::q32_32 z) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(parachute);
    ObjectContainmentRuntimeComponent* runtime = entity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *entity)
        : nullptr;
    if (!runtime || !runtime->plan || !std::any_of(
            runtime->plan->rules.begin(), runtime->plan->rules.end(),
            [](const ObjectContainmentRule& rule) {
                return rule.kind == ObjectContainmentKind::Parachute;
            })) {
        return false;
    }
    runtime->parachuteLandingX = x;
    runtime->parachuteLandingY = y;
    runtime->parachuteLandingZ = z;
    if (const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, *entity)) {
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            registry, *entity, *transform);
        runtime->parachuteOverrideStartX = position.x;
        runtime->parachuteOverrideStartY = position.y;
        runtime->parachuteOverrideStartZ = position.z;
    }
    runtime->parachuteHasLandingOverride = true;
    runtime->parachuteHasLandingTarget = true;
    return true;
}

void ObjectContainmentSystem::onContainerDie(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId container, ObjectId damageSource, uint32_t authoredOrder,
    uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage,
    std::optional<ObjectContainmentDeathFinalizeCommand>& outFinalize) const {
    outFinalize.reset();
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(container);
    if (!entity) return;
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *entity);
    ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, *entity);
    if (!runtime || !runtime->plan || !contents || contents->objects.empty())
        return;
    const auto selected = std::find_if(
        runtime->plan->rules.begin(), runtime->plan->rules.end(),
        [authoredOrder](const ObjectContainmentRule& rule) {
            return rule.authoredOrder == authoredOrder;
        });
    if (selected == runtime->plan->rules.end()) return;
    const uint32_t ruleIndex = static_cast<uint32_t>(
        std::distance(runtime->plan->rules.begin(), selected));
    const ObjectContainmentRule& rule = *selected;

    const auto belongsToSelectedRule =
        [&](const ObjectContainedObjectRecord& record) {
            const std::optional<ecs::entity> passenger =
                lifecycle.entityFromIdIncludingPending(record.object);
            const ObjectContainedByComponent* edge = passenger
                ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                            *passenger)
                : nullptr;
            if (!edge || edge->container != container) return false;
            if (edge->containmentRuleIndex == ruleIndex) return true;
            // Portable Overlord/Helix add-ons are structural OCL edges and
            // intentionally carry no admission rule index. Their authored
            // Contain capability nevertheless owns their terminal lifetime.
            return record.destroyWithContainer &&
                edge->containmentRuleIndex ==
                    std::numeric_limits<uint32_t>::max() &&
                (rule.kind == ObjectContainmentKind::Overlord ||
                 rule.kind == ObjectContainmentKind::Helix);
        };

    container::Vector<ObjectContainedObjectRecord> selectedContents;
    selectedContents.reserve(contents->objects.size());
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        if (belongsToSelectedRule(record)) selectedContents.push_back(record);
    }
    if (selectedContents.empty()) return;

    if (rule.kind == ObjectContainmentKind::Parachute) {
        // ParachuteContain::onDie releases the rider instead of cascading the
        // parachute's destruction. Losing the chute while airborne applies
        // the authored FALLING/SPLATTED penalty and leaves Physics awake in
        // free fall; the later ground contact owns the remaining damage.
        const ObjectAirborneComponent* parachuteAirborne =
            ecs::try_get<ObjectAirborneComponent>(registry, *entity);
        const bool airborne =
            parachuteAirborne && parachuteAirborne->isAirborne;
        for (const ObjectContainedObjectRecord& record : selectedContents) {
            const std::optional<ecs::entity> rider =
                lifecycle.entityFromIdIncludingPending(record.object);
            const ObjectHealthComponent* health = rider
                ? ecs::try_get<ObjectHealthComponent>(registry, *rider)
                : nullptr;
            const bool riderDead = !health || health->effectivelyDead;
            const math::q32_32 maximumHealth = health
                ? health->maximumFixed : math::q32_32{};
            static_cast<void>(detach(
                registry, lifecycle, record.object, confirmedTick));
            if (rider) {
                static_cast<void>(ObjectDisabledSystem::clear(
                    registry, *rider, ObjectDisabledReason::Held,
                    confirmedTick));
                if (airborne) {
                    if (ObjectPhysicsComponent* physics =
                            ecs::try_get<ObjectPhysicsComponent>(registry,
                                                                 *rider)) {
                        physics->allowToFall = true;
                        physics->inFreeFall = true;
                        physics->sleeping = false;
                    }
                }
            }
            if (!airborne || riderDead ||
                rule.freeFallDamageFraction <= math::q32_32{}) {
                continue;
            }
            const math::q32_32 damage = maximumHealth *
                rule.freeFallDamageFraction;
            if (damage <= math::q32_32{}) continue;
            outDamage.push_back({
                .target = record.object,
                .source = damageSource,
                .sourceSequence = rule.authoredOrder,
                .amount = damage,
                .damageType = game::DamageType::FALLING,
                .deathType = game::DeathType::SPLATTED,
                .confirmedTick = confirmedTick,
            });
        }
        return;
    }
    if (rule.kind == ObjectContainmentKind::Cave ||
        rule.kind == ObjectContainmentKind::Tunnel) {
        const ObjectContainmentKind networkKind = rule.kind;
        const OwnerComponent* sourceOwner =
            ecs::try_get<OwnerComponent>(registry, *entity);
        struct AlternateEntrance final {
            ObjectId id = INVALID_OBJECT_ID;
            ecs::entity entity = ecs::null;
            uint32_t ruleIndex = std::numeric_limits<uint32_t>::max();
            uint64_t createdAtTick = std::numeric_limits<uint64_t>::max();
        } alternate;
        const auto view = ecs::view<const ObjectIdentityComponent,
                                    const ObjectContainmentRuntimeComponent>(
            registry);
        for (const ecs::entity candidate : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(candidate);
            if (!identity.id || identity.id == container ||
                !isCompletedContainmentEntrance(registry, candidate) ||
                !lifecycle.entityFromId(identity.id)) continue;
            const ObjectContainmentRuntimeComponent& otherRuntime =
                view.template get<
                    const ObjectContainmentRuntimeComponent>(candidate);
            if (!otherRuntime.plan) continue;
            if (networkKind == ObjectContainmentKind::Cave) {
                if (!runtime->hasCave || !otherRuntime.hasCave ||
                    runtime->caveIndex != otherRuntime.caveIndex) continue;
            } else {
                const OwnerComponent* otherOwner =
                    ecs::try_get<OwnerComponent>(registry, candidate);
                if (!sourceOwner || !otherOwner ||
                    sourceOwner->player != otherOwner->player) continue;
            }
            for (size_t ruleIndex = 0;
                 ruleIndex < otherRuntime.plan->rules.size(); ++ruleIndex) {
                if (otherRuntime.plan->rules[ruleIndex].kind != networkKind)
                    continue;
                if (!alternate.id || identity.id < alternate.id) {
                    alternate.id = identity.id;
                    alternate.entity = candidate;
                    alternate.ruleIndex = static_cast<uint32_t>(ruleIndex);
                }
                break;
            }
        }

        container::Vector<ObjectId> migrated;
        for (const ObjectContainedObjectRecord& record : selectedContents) {
            const std::optional<ecs::entity> passenger =
                lifecycle.entityFromIdIncludingPending(record.object);
            ObjectContainedByComponent* edge = passenger
                ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                            *passenger)
                : nullptr;
            if (!edge) continue;
            if (alternate.id) {
                edge->container = alternate.id;
                edge->containmentRuleIndex = alternate.ruleIndex;
                migrated.push_back(record.object);
                continue;
            }
            static_cast<void>(lifecycle.requestDestroy(
                record.object, ObjectDestroyReason::System,
                confirmedTick));
            static_cast<void>(detach(
                registry, lifecycle, record.object, confirmedTick));
        }
        if (!migrated.empty()) {
            ObjectContainmentComponent* destination =
                ecs::try_get<ObjectContainmentComponent>(
                    registry, alternate.entity);
            if (!destination)
                destination = &ecs::emplace<ObjectContainmentComponent>(
                    registry, alternate.entity);
            for (const ObjectContainedObjectRecord& record : selectedContents) {
                if (!std::binary_search(migrated.begin(), migrated.end(),
                                        record.object)) continue;
                const auto position = std::lower_bound(
                    destination->objects.begin(), destination->objects.end(),
                    record.object,
                    [](const ObjectContainedObjectRecord& candidate,
                       ObjectId id) { return candidate.object < id; });
                if (position == destination->objects.end() ||
                    position->object != record.object)
                    destination->objects.insert(position, record);
            }
            contents->objects.erase(std::remove_if(
                contents->objects.begin(), contents->objects.end(),
                [&](const ObjectContainedObjectRecord& record) {
                    return std::binary_search(migrated.begin(),
                                              migrated.end(),
                                              record.object);
                }), contents->objects.end());
            bumpRevision(*contents);
            bumpRevision(*destination);
        }
        return;
    }

    container::Vector<ObjectContainedObjectRecord> damageOrder =
        selectedContents;
    std::stable_sort(
        damageOrder.begin(), damageOrder.end(),
        [](const ObjectContainedObjectRecord& left,
           const ObjectContainedObjectRecord& right) {
            if (left.entryOrdinal != right.entryOrdinal)
                return left.entryOrdinal < right.entryOrdinal;
            // Legacy saves or partially migrated fixtures may lack an entry
            // ordinal. Preserve a total deterministic fallback without
            // replacing the normal ContainedItemsList FIFO contract.
            if (left.confirmedEnteredTick != right.confirmedEnteredTick)
                return left.confirmedEnteredTick < right.confirmedEnteredTick;
            return left.object < right.object;
        });
    for (const ObjectContainedObjectRecord& record : damageOrder) {
        if (!record.object) continue;
        const std::optional<ecs::entity> passenger =
            lifecycle.entityFromIdIncludingPending(record.object);
        const ObjectHealthComponent* health = passenger
            ? ecs::try_get<ObjectHealthComponent>(registry, *passenger)
            : nullptr;
        if (!health || health->effectivelyDead) continue;
        if (record.destroyWithContainer) {
            outDamage.push_back({
                .target = record.object,
                .source = container,
                .sourceSequence = rule.authoredOrder,
                .causalGroup = container,
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = game::DeathType::NORMAL,
                .forceKill = true,
                .confirmedTick = confirmedTick,
            });
            continue;
        }
        if (rule.damagePercentToUnits <= math::q32_32{} ||
            health->maximumFixed <= math::q32_32{}) continue;
        const math::q32_32 damage = health->maximumFixed *
            rule.damagePercentToUnits;
        if (damage > math::q32_32{}) {
            outDamage.push_back({
                .target = record.object,
                .source = container,
                .sourceSequence = rule.authoredOrder,
                .amount = damage,
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = rule.burnedDeathToUnits
                    ? game::DeathType::BURNED : game::DeathType::NORMAL,
                .forceKill = rule.damagePercentToUnits >=
                    math::q32_32{int32_t{1}},
                .confirmedTick = confirmedTick,
            });
        }
    }
    outFinalize = ObjectContainmentDeathFinalizeCommand{
        .container = container,
        .authoredOrder = authoredOrder,
        .confirmedTick = confirmedTick,
    };
}

ObjectContainmentDeathFinalizeAdvance
ObjectContainmentSystem::advanceContainerDie(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectContainmentDeathFinalizeCommand& command,
    const game::terrain::TerrainLogic* terrain,
    const navigation::NavigationSystem* navigation,
    uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectDeleteDestroyRequest>& outDestroy) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(command.container);
    if (!entity) return ObjectContainmentDeathFinalizeAdvance::Completed;
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *entity);
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, *entity);
    if (!runtime || !runtime->plan || !contents)
        return ObjectContainmentDeathFinalizeAdvance::Completed;
    const auto selected = std::find_if(
        runtime->plan->rules.begin(), runtime->plan->rules.end(),
        [&](const ObjectContainmentRule& rule) {
            return rule.authoredOrder == command.authoredOrder;
        });
    if (selected == runtime->plan->rules.end())
        return ObjectContainmentDeathFinalizeAdvance::Completed;
    const uint32_t ruleIndex = static_cast<uint32_t>(
        std::distance(runtime->plan->rules.begin(), selected));

    container::Vector<ObjectContainedObjectRecord> snapshot;
    snapshot.reserve(contents->objects.size());
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        const std::optional<ecs::entity> passenger =
            lifecycle.entityFromIdIncludingPending(record.object);
        const ObjectContainedByComponent* edge = passenger
            ? ecs::try_get<ObjectContainedByComponent>(registry, *passenger)
            : nullptr;
        if (!edge || edge->container != command.container) continue;
        const bool exactRule = edge->containmentRuleIndex == ruleIndex;
        const bool structuralPortable = record.destroyWithContainer &&
            edge->containmentRuleIndex ==
                std::numeric_limits<uint32_t>::max() &&
            (selected->kind == ObjectContainmentKind::Overlord ||
             selected->kind == ObjectContainmentKind::Helix);
        if (exactRule || structuralPortable) snapshot.push_back(record);
    }
    std::stable_sort(
        snapshot.begin(), snapshot.end(),
        [](const ObjectContainedObjectRecord& left,
           const ObjectContainedObjectRecord& right) {
            if (left.entryOrdinal != right.entryOrdinal)
                return left.entryOrdinal < right.entryOrdinal;
            if (left.confirmedEnteredTick != right.confirmedEnteredTick)
                return left.confirmedEnteredTick < right.confirmedEnteredTick;
            return left.object < right.object;
        });

    if (command.phase ==
        ObjectContainmentDeathFinalizePhase::CullBlockedRiders) {
        command.phase =
            ObjectContainmentDeathFinalizePhase::DetachRemaining;
        if (transportDerivedContainment(selected->kind)) {
            const size_t initialDamageCount = outDamage.size();
            const size_t initialDestroyCount = outDestroy.size();
            uint32_t localOrdinal = 0;
            for (const ObjectContainedObjectRecord& record : snapshot) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromIdIncludingPending(record.object);
                const ObjectHealthComponent* health = passenger
                    ? ecs::try_get<ObjectHealthComponent>(registry, *passenger)
                    : nullptr;
                if (!passenger || lifecycle.isPendingDestroy(record.object) ||
                    !health || health->effectivelyDead ||
                    isSpecificRiderFreeToExit(
                        registry, *entity, *passenger, *runtime, *selected,
                        command.confirmedTick, terrain, navigation)) {
                    continue;
                }
                const uint64_t submissionOrdinal =
                    reserveTransportGameplayOrdinal(
                        nextGameplaySubmissionOrdinal);
                if (selected->destroyRidersWhoAreNotFreeToExit) {
                    outDestroy.push_back({
                        .object = record.object,
                        .reason = ObjectDestroyReason::System,
                        .source = command.container,
                        .authoredOrder = command.authoredOrder,
                        .localOrdinal = localOrdinal++,
                        .submissionOrdinal = submissionOrdinal,
                        .confirmedTick = command.confirmedTick,
                    });
                } else {
                    outDamage.push_back({
                        .target = record.object,
                        .source = command.container,
                        .sourceSequence = localOrdinal++,
                        .submissionOrdinal = submissionOrdinal,
                        .causalGroup = command.container,
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = selected->burnedDeathToUnits
                            ? game::DeathType::BURNED
                            : game::DeathType::NORMAL,
                        .forceKill = true,
                        .confirmedTick = command.confirmedTick,
                    });
                }
            }
            if (outDamage.size() != initialDamageCount ||
                outDestroy.size() != initialDestroyCount) {
                return ObjectContainmentDeathFinalizeAdvance::ChildrenEmitted;
            }
        }
    }

    for (const ObjectContainedObjectRecord& record : snapshot) {
        static_cast<void>(detach(
            registry, lifecycle, record.object, command.confirmedTick));
    }
    return ObjectContainmentDeathFinalizeAdvance::Completed;
}

void ObjectContainmentSystem::onContainerDelete(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId container, uint32_t authoredOrder, uint64_t confirmedTick,
    container::Vector<ObjectDeleteDestroyRequest>& outDestroy) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(container);
    const ObjectContainmentRuntimeComponent* runtime = entity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *entity)
        : nullptr;
    ObjectContainmentComponent* contents = entity
        ? ecs::try_get<ObjectContainmentComponent>(registry, *entity)
        : nullptr;
    if (!runtime || !runtime->plan || !contents) return;

    const auto selected = std::find_if(
        runtime->plan->rules.begin(), runtime->plan->rules.end(),
        [authoredOrder](const ObjectContainmentRule& rule) {
            return rule.authoredOrder == authoredOrder;
        });
    if (selected == runtime->plan->rules.end()) return;
    const uint32_t ruleIndex = static_cast<uint32_t>(
        std::distance(runtime->plan->rules.begin(), selected));

    // TunnelContain::onDelete unregisters one entrance instead of invoking
    // OpenContain::onDelete. TunnelTracker rebinds every passenger which
    // remembers the removed entrance to a stable surviving entrance; only
    // removal of the final completed entrance destroys the shared roster.
    // Cave uses the same tracker topology keyed by caveIndex.
    if (selected->kind == ObjectContainmentKind::Tunnel ||
        selected->kind == ObjectContainmentKind::Cave) {
        const ObjectContainmentKind networkKind = selected->kind;
        const OwnerComponent* sourceOwner =
            ecs::try_get<OwnerComponent>(registry, *entity);
        struct AlternateEntrance final {
            ObjectId id = INVALID_OBJECT_ID;
            ecs::entity entity = ecs::null;
            uint32_t ruleIndex = std::numeric_limits<uint32_t>::max();
            uint64_t createdAtTick = std::numeric_limits<uint64_t>::max();
        } alternate;
        const auto entrances = ecs::view<
            const ObjectIdentityComponent,
            const ObjectContainmentRuntimeComponent>(registry);
        for (const ecs::entity candidate : entrances) {
            const ObjectIdentityComponent& identity =
                entrances.template get<const ObjectIdentityComponent>(
                    candidate);
            if (!identity.id || identity.id == container ||
                !lifecycle.entityFromId(identity.id) ||
                !isCompletedContainmentEntrance(registry, candidate)) {
                continue;
            }
            const ObjectContainmentRuntimeComponent& candidateRuntime =
                entrances.template get<
                    const ObjectContainmentRuntimeComponent>(candidate);
            const ObjectLifecycleComponent* candidateLifecycle =
                ecs::try_get<ObjectLifecycleComponent>(registry, candidate);
            const uint64_t candidateCreatedAtTick = candidateLifecycle
                ? candidateLifecycle->createdAtTick
                : std::numeric_limits<uint64_t>::max();
            if (!candidateRuntime.plan) continue;
            if (networkKind == ObjectContainmentKind::Cave) {
                if (!runtime->hasCave || !candidateRuntime.hasCave ||
                    runtime->caveIndex != candidateRuntime.caveIndex) {
                    continue;
                }
            } else {
                const OwnerComponent* candidateOwner =
                    ecs::try_get<OwnerComponent>(registry, candidate);
                if (!sourceOwner || !candidateOwner ||
                    sourceOwner->player != candidateOwner->player) {
                    continue;
                }
            }
            for (size_t candidateRuleIndex = 0;
                 candidateRuleIndex < candidateRuntime.plan->rules.size();
                 ++candidateRuleIndex) {
                if (candidateRuntime.plan->rules[candidateRuleIndex].kind !=
                    networkKind) {
                    continue;
                }
                // TunnelTracker keeps entrance registration order. Creation
                // tick plus never-reused ObjectId is the value-only equivalent
                // and remains stable when ECS storage order changes.
                if (!alternate.id ||
                    candidateCreatedAtTick < alternate.createdAtTick ||
                    (candidateCreatedAtTick == alternate.createdAtTick &&
                     identity.id < alternate.id)) {
                    alternate = {
                        .id = identity.id,
                        .entity = candidate,
                        .ruleIndex =
                            static_cast<uint32_t>(candidateRuleIndex),
                        .createdAtTick = candidateCreatedAtTick,
                    };
                }
                break;
            }
        }

        container::Vector<ObjectContainedObjectRecord> sourceRoster;
        for (const ObjectContainedObjectRecord& record : contents->objects) {
            const std::optional<ecs::entity> passenger =
                lifecycle.entityFromIdIncludingPending(record.object);
            const ObjectContainedByComponent* edge = passenger
                ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                            *passenger)
                : nullptr;
            if (edge && edge->container == container &&
                edge->containmentRuleIndex == ruleIndex) {
                sourceRoster.push_back(record);
            }
        }

        if (alternate.id) {
            ObjectContainmentComponent* destination =
                ecs::try_get<ObjectContainmentComponent>(registry,
                                                          alternate.entity);
            if (!destination) {
                destination = &ecs::emplace<ObjectContainmentComponent>(
                    registry, alternate.entity);
            }
            container::Vector<ObjectId> migrated;
            migrated.reserve(sourceRoster.size());
            for (const ObjectContainedObjectRecord& record : sourceRoster) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromIdIncludingPending(record.object);
                ObjectContainedByComponent* edge = passenger
                    ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                *passenger)
                    : nullptr;
                if (!edge) continue;
                edge->container = alternate.id;
                edge->containmentRuleIndex = alternate.ruleIndex;
                const auto position = std::lower_bound(
                    destination->objects.begin(),
                    destination->objects.end(), record.object,
                    [](const ObjectContainedObjectRecord& candidate,
                       ObjectId object) {
                        return candidate.object < object;
                    });
                if (position == destination->objects.end() ||
                    position->object != record.object) {
                    destination->objects.insert(position, record);
                }
                migrated.push_back(record.object);
            }
            std::sort(migrated.begin(), migrated.end());
            contents->objects.erase(
                std::remove_if(
                    contents->objects.begin(), contents->objects.end(),
                    [&](const ObjectContainedObjectRecord& record) {
                        return std::binary_search(
                            migrated.begin(), migrated.end(), record.object);
                    }),
                contents->objects.end());
            if (!migrated.empty()) {
                bumpRevision(*contents);
                bumpRevision(*destination);
            }
            return;
        }

        // No completed entrance survives.  The modern roster is stored on
        // entrance components instead of one TunnelTracker, so collect the
        // whole typed network before scheduling depth-first child deletes.
        container::Vector<ObjectId> networkRoster;
        const auto networkView = ecs::view<
            const ObjectContainmentRuntimeComponent,
            const ObjectContainmentComponent>(registry);
        for (const ecs::entity entrance : networkView) {
            const ObjectContainmentRuntimeComponent& entranceRuntime =
                networkView.template get<
                    const ObjectContainmentRuntimeComponent>(entrance);
            if (!entranceRuntime.plan) continue;
            if (networkKind == ObjectContainmentKind::Cave) {
                if (!runtime->hasCave || !entranceRuntime.hasCave ||
                    runtime->caveIndex != entranceRuntime.caveIndex) {
                    continue;
                }
            } else {
                const OwnerComponent* entranceOwner =
                    ecs::try_get<OwnerComponent>(registry, entrance);
                if (!sourceOwner || !entranceOwner ||
                    sourceOwner->player != entranceOwner->player) {
                    continue;
                }
            }
            const ObjectContainmentComponent& entranceContents =
                networkView.template get<
                    const ObjectContainmentComponent>(entrance);
            for (const ObjectContainedObjectRecord& record :
                 entranceContents.objects) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromIdIncludingPending(record.object);
                const ObjectContainedByComponent* edge = passenger
                    ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                *passenger)
                    : nullptr;
                if (!edge || edge->containmentRuleIndex >=
                        entranceRuntime.plan->rules.size() ||
                    entranceRuntime.plan->rules[
                        edge->containmentRuleIndex].kind != networkKind) {
                    continue;
                }
                networkRoster.push_back(record.object);
            }
        }
        std::sort(networkRoster.begin(), networkRoster.end());
        networkRoster.erase(
            std::unique(networkRoster.begin(), networkRoster.end()),
            networkRoster.end());
        uint32_t localOrdinal = 0;
        for (const ObjectId passenger : networkRoster) {
            outDestroy.push_back({
                .object = passenger,
                .reason = ObjectDestroyReason::System,
                .source = container,
                .authoredOrder = authoredOrder,
                .localOrdinal = localOrdinal++,
                .confirmedTick = confirmedTick,
            });
        }
        return;
    }

    container::Vector<ObjectContainedObjectRecord> literalList;
    literalList.reserve(contents->objects.size());
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        const std::optional<ecs::entity> passenger =
            lifecycle.entityFromIdIncludingPending(record.object);
        const ObjectContainedByComponent* edge = passenger
            ? ecs::try_get<ObjectContainedByComponent>(registry, *passenger)
            : nullptr;
        if (!edge || edge->container != container) continue;
        const bool exactRule = edge->containmentRuleIndex == ruleIndex;
        const bool structuralPortable = record.destroyWithContainer &&
            edge->containmentRuleIndex ==
                std::numeric_limits<uint32_t>::max() &&
            (selected->kind == ObjectContainmentKind::Overlord ||
             selected->kind == ObjectContainmentKind::Helix);
        if (exactRule || structuralPortable) literalList.push_back(record);
    }
    std::stable_sort(
        literalList.begin(), literalList.end(),
        [](const ObjectContainedObjectRecord& left,
           const ObjectContainedObjectRecord& right) {
            if (left.confirmedEnteredTick != right.confirmedEnteredTick) {
                return left.confirmedEnteredTick < right.confirmedEnteredTick;
            }
            return left.object < right.object;
        });
    uint32_t localOrdinal = 0;
    for (const ObjectContainedObjectRecord& record : literalList) {
        outDestroy.push_back({
            .object = record.object,
            .reason = ObjectDestroyReason::System,
            .source = container,
            .authoredOrder = authoredOrder,
            .localOrdinal = localOrdinal++,
            .confirmedTick = confirmedTick,
        });
    }
}


} // namespace engine
