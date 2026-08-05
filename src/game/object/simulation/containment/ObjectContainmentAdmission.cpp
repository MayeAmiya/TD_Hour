#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/structure/ObjectBridge.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
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

void ObjectContainmentSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot* content,
    const ObjectSimulationRules& rules) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->containmentPlan)
        return;

    ObjectContainmentRuntimeComponent runtime{
        .plan = type->archetype->containmentPlan,
        .networkCapacity = rules.maxTunnelCapacity,
    };
    const auto cave = std::find_if(
        runtime.plan->rules.begin(), runtime.plan->rules.end(),
        [](const ObjectContainmentRule& rule) {
            return rule.kind == ObjectContainmentKind::Cave;
        });
    if (cave != runtime.plan->rules.end()) {
        runtime.caveIndex = cave->caveIndex;
        runtime.hasCave = true;
    }
    if (std::any_of(runtime.plan->rules.begin(), runtime.plan->rules.end(),
                    [](const ObjectContainmentRule& rule) {
                        return rule.kind == ObjectContainmentKind::Garrison;
                    })) {
        runtime.evacuationDisposition =
            ObjectContainmentEvacuationDisposition::BurstFromCenter;
    }
    runtime.behaviorStates.resize(
        type->archetype->containmentPlan->behaviorRules.size());
    runtime.exitPathSets.resize(runtime.plan->rules.size());
    const game::W3dPristineBoneCatalog* catalog = content
        ? content->pristineBoneCatalog() : nullptr;
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, entity);
    const game::ThingTemplate& templateData =
        type->archetype->templateData;
    if (catalog && catalog->isLoaded() &&
        !templateData.modelConditionVisuals.empty()) {
        const game::ModelConditionMask initialConditions = visual
            ? visual->modelConditionFlags : game::ModelConditionMask{};
        const size_t visualRuleIndex =
            game::selectModelConditionVisualRuleIndex(
                templateData, initialConditions);
        const auto boneName = [](container::StringView prefix,
                                 uint32_t ordinal,
                                 uint32_t count) {
            container::String result{prefix};
            if (count > 1) {
                if (ordinal < 10) result.push_back('0');
                result += std::to_string(ordinal);
            }
            return result;
        };
        if (visualRuleIndex < templateData.modelConditionVisuals.size()) {
            for (size_t ruleIndex = 0; ruleIndex < runtime.plan->rules.size();
                 ++ruleIndex) {
                const ObjectContainmentRule& rule =
                    runtime.plan->rules[ruleIndex];
                if (rule.railedDockOwnsExit ||
                    (rule.numberOfExitPaths == 0 && rule.exitBone.empty()))
                    continue;
                ObjectContainmentExitPathSet& pathSet =
                    runtime.exitPathSets[ruleIndex];
                pathSet.paths.resize(std::max<uint32_t>(
                    rule.numberOfExitPaths, rule.exitBone.empty() ? 0u : 1u));
                if (!rule.exitBone.empty()) {
                    const auto exit = catalog->find(
                        type->archetype->name, visualRuleIndex,
                        rule.exitBone);
                    if (exit) {
                        ObjectContainmentExitPath& path = pathSet.paths.front();
                        path.startX = exit->translation.x;
                        path.startY = exit->translation.y;
                        path.startZ = exit->translation.z;
                        path.endX = path.startX;
                        path.endY = path.startY;
                        path.endZ = path.startZ;
                        path.valid = true;
                        path.hasEnd = false;
                    }
                    continue;
                }
                for (uint32_t ordinal = 1;
                     ordinal <= rule.numberOfExitPaths; ++ordinal) {
                    const auto start = catalog->find(
                        type->archetype->name, visualRuleIndex,
                        boneName("ExitStart", ordinal,
                                 rule.numberOfExitPaths));
                    const auto end = catalog->find(
                        type->archetype->name, visualRuleIndex,
                        boneName("ExitEnd", ordinal,
                                 rule.numberOfExitPaths));
                    if (!start || !end) continue;
                    ObjectContainmentExitPath& path =
                        pathSet.paths[ordinal - 1u];
                    path.startX = start->translation.x;
                    path.startY = start->translation.y;
                    path.startZ = start->translation.z;
                    path.endX = end->translation.x;
                    path.endY = end->translation.y;
                    path.endZ = end->translation.z;
                    path.valid = true;
                }
            }
        }
    }
    if (ObjectContainmentRuntimeComponent* existing =
            ecs::try_get<ObjectContainmentRuntimeComponent>(registry, entity)) {
        *existing = std::move(runtime);
    } else {
        ecs::emplace<ObjectContainmentRuntimeComponent>(
            registry, entity, std::move(runtime));
    }
    ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, entity);
    if (!contents)
        contents = &ecs::emplace<ObjectContainmentComponent>(registry, entity);
    for (const ObjectContainmentRule& rule : type->archetype->containmentPlan->rules)
        contents->passengersAllowedToFire =
            contents->passengersAllowedToFire || rule.passengersAllowedToFire;
    const bool hasTunnel = std::any_of(
        type->archetype->containmentPlan->rules.begin(),
        type->archetype->containmentPlan->rules.end(),
        [](const ObjectContainmentRule& rule) {
            return rule.kind == ObjectContainmentKind::Tunnel;
        });
    if (hasTunnel) {
        if (ObjectTunnelNetworkCombatHandoffComponent* handoff =
                ecs::try_get<ObjectTunnelNetworkCombatHandoffComponent>(
                    registry, entity)) {
            *handoff = {};
        } else {
            ecs::emplace<ObjectTunnelNetworkCombatHandoffComponent>(
                registry, entity);
        }
    }
}

bool ObjectContainmentSystem::canAttach(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentAttachRequest& request) const noexcept {
    if (!request.container || !request.object ||
        request.container == request.object ||
        lifecycle.isPendingDestroy(request.container) ||
        lifecycle.isPendingDestroy(request.object)) return false;
    const std::optional<ecs::entity> container =
        lifecycle.entityFromId(request.container);
    const std::optional<ecs::entity> object =
        lifecycle.entityFromId(request.object);
    if (!container || !object || !hasContainInterface(registry, *container) ||
        ecs::try_get<ObjectContainedByComponent>(registry, *object) ||
        wouldCreateCycle(registry, lifecycle, request.container,
                         request.object)) return false;
    const ObjectHealthComponent* containerHealth =
        ecs::try_get<ObjectHealthComponent>(registry, *container);
    return !containerHealth || !containerHealth->effectivelyDead;
}


std::optional<ObjectContainmentAttachRequest>
ObjectContainmentSystem::prepareAttach(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentRequest& request,
    const PlayerRegistry* players) const {
    ObjectId targetContainer = request.container;
    std::optional<ecs::entity> container =
        lifecycle.entityFromId(targetContainer);
    const std::optional<ecs::entity> object =
        lifecycle.entityFromId(request.object);
    if (!container || !object) return std::nullopt;
    // OverlordContain's first structural passenger is the add-on. Once it is
    // present, every ordinary contain query redirects through the add-on's
    // own Contain interface instead of treating the Overlord as a second
    // independent passenger store.
    for (uint32_t depth = 0; depth < 8; ++depth) {
        const ObjectContainmentRuntimeComponent* hostRuntime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                             *container);
        if (!hostRuntime || !hostRuntime->plan ||
            std::none_of(hostRuntime->plan->rules.begin(),
                         hostRuntime->plan->rules.end(),
                         [](const ObjectContainmentRule& candidate) {
                             return candidate.kind ==
                                 ObjectContainmentKind::Overlord;
                         })) break;
        const ObjectContainmentComponent* hostContents =
            ecs::try_get<ObjectContainmentComponent>(registry, *container);
        if (!hostContents) break;
        ObjectId addOn = INVALID_OBJECT_ID;
        for (const ObjectContainedObjectRecord& record :
             hostContents->objects) {
            if (record.object && record.destroyWithContainer) {
                addOn = record.object;
                break;
            }
        }
        if (!addOn) break;
        const std::optional<ecs::entity> addOnEntity =
            lifecycle.entityFromId(addOn);
        if (!addOnEntity || !hasContainInterface(registry, *addOnEntity))
            break;
        targetContainer = addOn;
        container = addOnEntity;
    }
    if (!request.force &&
        !railedTransportDockAcceptsObject(registry, *container, *object)) {
        return std::nullopt;
    }
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *container);
    const ObjectContainmentRule* rule =
        selectContainmentRule(registry, lifecycle, *container, *object,
                              request.force, players);
    if (!runtime || !runtime->plan || !rule) return std::nullopt;
    bool destroyWithContainer = rule->destroyPassengersWithContainer;
    bool enclosing = rule->enclosingContainer;
    if (rule->kind == ObjectContainmentKind::Overlord ||
        rule->kind == ObjectContainmentKind::Helix) {
        const ObjectKindOfComponent* passengerKinds =
            ecs::try_get<ObjectKindOfComponent>(registry, *object);
        destroyWithContainer = passengerKinds && game::objectHasKind(
            passengerKinds->mask,
            game::ObjectKindOf::PortableStructure);
        if (destroyWithContainer) enclosing = false;
    }
    return ObjectContainmentAttachRequest{
        .container = targetContainer,
        .object = request.object,
        .containmentRuleIndex = static_cast<uint32_t>(
            rule - runtime->plan->rules.data()),
        .confirmedEnteredTick = request.confirmedTick,
        .destroyWithContainer = destroyWithContainer,
        .enclosing = enclosing,
        .followsContainerTransform = rule->followsContainerTransform,
    };
}

bool ObjectContainmentSystem::commitPreparedAttach(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentAttachRequest& prepared,
    uint64_t confirmedTick,
    container::Vector<ObjectContainmentEvent>& events) const {
    if (const std::optional<ecs::entity> container =
            lifecycle.entityFromId(prepared.container)) {
        const ObjectContainmentRuntimeComponent* runtime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                             *container);
        const bool replacesRider = runtime && runtime->plan &&
            prepared.containmentRuleIndex < runtime->plan->rules.size() &&
            runtime->plan->rules[prepared.containmentRuleIndex].kind ==
                ObjectContainmentKind::RiderChange;
        const ObjectContainmentKind preparedKind = runtime && runtime->plan &&
                prepared.containmentRuleIndex < runtime->plan->rules.size()
            ? runtime->plan->rules[prepared.containmentRuleIndex].kind
            : ObjectContainmentKind::Cave;
        const bool replacesForeignOpen = runtime && runtime->plan &&
            prepared.containmentRuleIndex < runtime->plan->rules.size() &&
            preparedKind != ObjectContainmentKind::Cave &&
            preparedKind != ObjectContainmentKind::Tunnel &&
            preparedKind != ObjectContainmentKind::MobNexus &&
            preparedKind != ObjectContainmentKind::RiderChange;
        if (replacesForeignOpen) {
            const std::optional<ecs::entity> entering =
                lifecycle.entityFromId(prepared.object);
            const OwnerComponent* enteringOwner = entering
                ? ecs::try_get<OwnerComponent>(registry, *entering) : nullptr;
            container::Vector<ObjectId> foreignPassengers;
            if (enteringOwner) {
                if (const ObjectContainmentComponent* contents =
                        ecs::try_get<ObjectContainmentComponent>(registry,
                                                                  *container)) {
                    for (const ObjectContainedObjectRecord& record :
                         contents->objects) {
                        const std::optional<ecs::entity> passenger =
                            lifecycle.entityFromId(record.object);
                        const ObjectContainedByComponent* edge = passenger
                            ? ecs::try_get<ObjectContainedByComponent>(
                                registry, *passenger) : nullptr;
                        const OwnerComponent* passengerOwner = passenger
                            ? ecs::try_get<OwnerComponent>(registry,
                                                           *passenger)
                            : nullptr;
                        if (edge && passengerOwner &&
                            edge->containmentRuleIndex ==
                                prepared.containmentRuleIndex &&
                            passengerOwner->player != enteringOwner->player) {
                            foreignPassengers.push_back(record.object);
                        }
                    }
                }
            }
            for (const ObjectId passenger : foreignPassengers) {
                static_cast<void>(requestDetach(
                    registry, lifecycle,
                    {.kind = ObjectContainmentRequestKind::Detach,
                     .container = prepared.container,
                     .object = passenger,
                     .confirmedTick = confirmedTick,
                     .force = true}, events));
            }
        }
        if (replacesRider) {
            container::Vector<ObjectId> previousRiders;
            if (const ObjectContainmentComponent* contents =
                    ecs::try_get<ObjectContainmentComponent>(registry,
                                                              *container)) {
                for (const ObjectContainedObjectRecord& record :
                     contents->objects) {
                    const std::optional<ecs::entity> passenger =
                        lifecycle.entityFromId(record.object);
                    const ObjectContainedByComponent* edge = passenger
                        ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                   *passenger)
                        : nullptr;
                    if (edge && edge->containmentRuleIndex ==
                            prepared.containmentRuleIndex)
                        previousRiders.push_back(record.object);
                }
            }
            for (const ObjectId rider : previousRiders) {
                static_cast<void>(requestDetach(
                    registry, lifecycle,
                    {.kind = ObjectContainmentRequestKind::Detach,
                     .container = prepared.container,
                     .object = rider,
                     .confirmedTick = confirmedTick,
                     .force = true}, events));
            }
        }
    }
    const bool accepted = attach(registry, lifecycle, prepared);
    pushContainmentEvent(events, ObjectContainmentRequestKind::Attach,
                         prepared.container, prepared.object,
                         confirmedTick, accepted);
    return accepted;
}

bool ObjectContainmentSystem::requestAttach(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentRequest& request,
    container::Vector<ObjectContainmentEvent>& events,
    const PlayerRegistry* players) const {
    const std::optional<ObjectContainmentAttachRequest> prepared =
        prepareAttach(registry, lifecycle, request, players);
    if (!prepared) {
        pushContainmentEvent(events, ObjectContainmentRequestKind::Attach,
                             request.container, request.object,
                             request.confirmedTick, false);
        return false;
    }
    return commitPreparedAttach(
        registry, lifecycle, *prepared, request.confirmedTick, events);
}


} // namespace engine
