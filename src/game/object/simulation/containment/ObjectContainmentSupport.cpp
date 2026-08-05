#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/structure/ObjectBridge.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/movement/ObjectFootprintEvacuation.h"
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

namespace engine::object_containment_detail {
using container::asciiEqualIgnoreCase;

[[nodiscard]] bool railedTransportDockAllowsContainment(
    const ecs::registry& registry, ecs::entity container) noexcept {
    const ObjectRailedTransportRuntimeComponent* railed =
        ecs::try_get<ObjectRailedTransportRuntimeComponent>(registry,
                                                            container);
    if (!railed || !railed->plan ||
        railed->plan->railedTransportContains.empty()) {
        return true;
    }
    for (const ObjectRailedTransportRuntime& runtime : railed->instances) {
        if (!runtime.dockOpen || runtime.inTransit ||
            runtime.loadingOrUnloading) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool railedTransportDockAcceptsObject(
    const ecs::registry& registry, ecs::entity container,
    ecs::entity object) noexcept {
    const ObjectRailedTransportRuntimeComponent* existing =
        ecs::try_get<ObjectRailedTransportRuntimeComponent>(registry,
                                                            container);
    const ObjectIdentityComponent* identity =
        ecs::try_get<ObjectIdentityComponent>(registry, object);
    const ObjectIdentityComponent* containerIdentity =
        ecs::try_get<ObjectIdentityComponent>(registry, container);
    if (existing && identity && identity->id) {
        for (const ObjectRailedTransportRuntime& runtime :
             existing->instances) {
            if (runtime.dockingObject == identity->id) return true;
        }
    }
    if (!railedTransportDockAllowsContainment(registry, container))
        return false;
    const ObjectRailedTransportRuntimeComponent* railed =
        ecs::try_get<ObjectRailedTransportRuntimeComponent>(registry,
                                                            container);
    if (!railed || !railed->plan ||
        railed->plan->railedTransportContains.empty() ||
        railed->plan->railedTransportDocks.empty()) {
        return true;
    }
    const ObjectFixedTransformComponent* dockTransform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, container);
    const ObjectFixedTransformComponent* objectTransform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, object);
    if (!dockTransform || !dockTransform->authoritative || !objectTransform ||
        !objectTransform->authoritative) return false;
    const math::q32_32 dx =
        objectTransform->position.x - dockTransform->position.x;
    const math::q32_32 dy =
        objectTransform->position.y - dockTransform->position.y;
    const math::q32_32 dz =
        objectTransform->position.z - dockTransform->position.z;
    const math::q32_32 distanceSquared = dx * dx + dy * dy + dz * dz;
    for (const game::ObjectRailedTransportDockRule& rule :
         railed->plan->railedTransportDocks) {
        if (distanceSquared <= rule.toleranceDistance *
                                   rule.toleranceDistance) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasContainInterface(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    return type && type->archetype && type->archetype->containmentPlan &&
           !type->archetype->containmentPlan->rules.empty();
}

[[nodiscard]] const ObjectContainmentRule* selectedContainmentRule(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity passenger, std::optional<ecs::entity>& host) noexcept {
    const ObjectContainedByComponent* contained =
        ecs::try_get<ObjectContainedByComponent>(registry, passenger);
    if (!contained || !contained->container) return nullptr;
    host = lifecycle.entityFromId(contained->container);
    const ObjectContainmentRuntimeComponent* runtime = host
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *host)
        : nullptr;
    if (!runtime || !runtime->plan ||
        contained->containmentRuleIndex >= runtime->plan->rules.size()) {
        return nullptr;
    }
    return &runtime->plan->rules[contained->containmentRuleIndex];
}

[[nodiscard]] bool passengerAllowedToFireRecursive(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity containedEntity, std::optional<ecs::entity> kindSubject,
    uint64_t confirmedTick, uint32_t depth) noexcept {
    if (depth >= 16u) return false;
    std::optional<ecs::entity> host;
    const ObjectContainmentRule* rule = selectedContainmentRule(
        registry, lifecycle, containedEntity, host);
    if (!rule) {
        return ecs::try_get<ObjectContainedByComponent>(
                   registry, containedEntity) == nullptr;
    }
    const ObjectContainmentComponent* contents = host
        ? ecs::try_get<ObjectContainmentComponent>(registry, *host)
        : nullptr;
    if (!host || !contents) return false;

    const ObjectKindOfComponent* kinds = kindSubject
        ? ecs::try_get<ObjectKindOfComponent>(registry, *kindSubject)
        : nullptr;
    const bool infantry = !kindSubject ||
        (kinds && game::objectHasKind(
            kinds->mask, game::ObjectKindOf::Infantry));
    const bool portable = kindSubject && kinds && game::objectHasKind(
        kinds->mask, game::ObjectKindOf::PortableStructure);
    const bool hostIsContained =
        ecs::try_get<ObjectContainedByComponent>(registry, *host) != nullptr;

    switch (rule->kind) {
    case ObjectContainmentKind::Garrison:
        return !isObjectDisabledBy(
            registry, *host, ObjectDisabledReason::Subdued, confirmedTick);
    case ObjectContainmentKind::Transport:
        if (!infantry) return false;
        break;
    case ObjectContainmentKind::Overlord:
        if ((!infantry && !portable) || hostIsContained) return false;
        break;
    case ObjectContainmentKind::Helix:
        if (hostIsContained || (!infantry && !portable)) return false;
        if (portable) return true;
        break;
    default:
        break;
    }
    if (!contents->passengersAllowedToFire) return false;
    if (!hostIsContained) return true;

    std::optional<ecs::entity> outerHost;
    const ObjectContainmentRule* outerRule = selectedContainmentRule(
        registry, lifecycle, *host, outerHost);
    const bool transportDelegatesOriginalPassenger =
        rule->kind == ObjectContainmentKind::Transport && outerRule &&
        (outerRule->kind == ObjectContainmentKind::Overlord ||
         outerRule->kind == ObjectContainmentKind::Helix);
    return passengerAllowedToFireRecursive(
        registry, lifecycle, *host,
        transportDelegatesOriginalPassenger ? kindSubject : std::nullopt,
        confirmedTick, depth + 1u);
}

[[nodiscard]] bool wouldCreateCycle(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId container, ObjectId object) noexcept {
    ObjectId cursor = container;
    for (size_t depth = 0; cursor && depth < 4096; ++depth) {
        if (cursor == object) return true;
        const std::optional<ecs::entity> entity =
            lifecycle.entityFromId(cursor);
        if (!entity) return false;
        const ObjectContainedByComponent* edge =
            ecs::try_get<ObjectContainedByComponent>(registry, *entity);
        if (!edge || !edge->container) return false;
        cursor = edge->container;
    }
    return static_cast<bool>(cursor);
}

void bumpRevision(ObjectContainmentComponent& component) noexcept {
    if (component.revision != std::numeric_limits<uint64_t>::max())
        ++component.revision;
}

[[nodiscard]] bool ruleAllowsPassenger(
    const ObjectContainmentRule& rule,
    const ObjectKindOfComponent* objectKinds) noexcept {
    if (!objectKinds) return rule.allowInsideKindOf.none();
    return (rule.allowInsideKindOf.none() ||
            objectKinds->mask.test_for_any(rule.allowInsideKindOf)) &&
           objectKinds->mask.test_for_none(rule.forbidInsideKindOf);
}

[[nodiscard]] uint32_t transportSlotCount(
    const ecs::registry& registry, ecs::entity object) noexcept {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, object);
    return type && type->archetype
        ? type->archetype->templateData.transportSlotCount : 0u;
}

[[nodiscard]] bool templateMatchesRider(
    const ThingTemplateComponent* passenger,
    container::StringView authoredTemplate) noexcept {
    if (!passenger || !passenger->archetype) return false;
    if (asciiEqualIgnoreCase(passenger->archetype->name,
                                   authoredTemplate)) return true;
    const game::ThingTemplate& value = passenger->archetype->templateData;
    if (!value.legacyReskinRootName.empty() &&
        asciiEqualIgnoreCase(value.legacyReskinRootName,
                                   authoredTemplate)) return true;
    return std::any_of(
        value.buildVariations.begin(), value.buildVariations.end(),
        [authoredTemplate](const container::String& candidate) {
            return asciiEqualIgnoreCase(candidate,
                                               authoredTemplate);
        });
}

[[nodiscard]] bool ruleUsesTransportSlots(
    ObjectContainmentKind kind) noexcept {
    return kind == ObjectContainmentKind::Transport ||
        kind == ObjectContainmentKind::MobNexus ||
        kind == ObjectContainmentKind::RiderChange ||
        kind == ObjectContainmentKind::Overlord ||
        kind == ObjectContainmentKind::Helix ||
        kind == ObjectContainmentKind::Parachute;
}

[[nodiscard]] bool ruleAllowsRelationship(
    const ObjectContainmentRule& rule, const ecs::registry& registry,
    ecs::entity container, ecs::entity object,
    const PlayerRegistry* players) noexcept {
    const OwnerComponent* passengerOwner =
        ecs::try_get<OwnerComponent>(registry, object);
    const OwnerComponent* containerOwner =
        ecs::try_get<OwnerComponent>(registry, container);
    if (!passengerOwner || !containerOwner || !passengerOwner->player ||
        !containerOwner->player) return rule.allowNeutralInside;
    const PlayerRelationship relationship = players
        ? players->relationship(passengerOwner->player,
                                containerOwner->player)
        : passengerOwner->player == containerOwner->player
            ? PlayerRelationship::Allies
            : (passengerOwner->player == NEUTRAL_PLAYER_ID ||
               containerOwner->player == NEUTRAL_PLAYER_ID)
                ? PlayerRelationship::Neutral
                : PlayerRelationship::Enemies;
    switch (relationship) {
    case PlayerRelationship::Allies: return rule.allowAlliesInside;
    case PlayerRelationship::Enemies: return rule.allowEnemiesInside;
    case PlayerRelationship::Neutral: return rule.allowNeutralInside;
    }
    return false;
}

[[nodiscard]] bool isCompletedContainmentEntrance(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return !status || !status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
        game::objectStatusBit(game::ObjectStatusFlag::Sold));
}

[[nodiscard]] const ObjectContainmentRule* selectContainmentRule(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity container,
    ecs::entity object, bool force, const PlayerRegistry* players) {
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, container);
    if (!runtime || !runtime->plan ||
        !isCompletedContainmentEntrance(registry, container)) return nullptr;
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, container);
    const ObjectKindOfComponent* objectKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, object);
    const uint32_t passengerSlots = transportSlotCount(registry, object);
    for (size_t ruleIndex = 0; ruleIndex < runtime->plan->rules.size();
         ++ruleIndex) {
        const ObjectContainmentRule& rule = runtime->plan->rules[ruleIndex];
        if (!force && rule.kind == ObjectContainmentKind::Garrison) {
            // GarrisonContain::isValidContainerFor rejects NO_GARRISON before
            // capacity or relationship tests.  Keep it at the shared rule
            // selector so UI context, player orders, AI and scripts cannot
            // each accidentally invent a different exception.
            if (objectKinds && game::objectHasKind(
                    objectKinds->mask, game::ObjectKindOf::NoGarrison)) {
                continue;
            }
            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(registry, container);
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, container);
            const bool garrisonableUntilDestroyed = kinds &&
                game::objectHasKind(
                    kinds->mask,
                    game::ObjectKindOf::GarrisonableUntilDestroyed);
            if (health &&
                health->damageState ==
                    ObjectBodyDamageState::ReallyDamaged &&
                !garrisonableUntilDestroyed) {
                continue;
            }
        }
        if (!force && rule.kind == ObjectContainmentKind::RiderChange &&
            !rule.riders.empty()) {
            const ThingTemplateComponent* passengerType =
                ecs::try_get<ThingTemplateComponent>(registry, object);
            const bool knownRider = std::any_of(
                rule.riders.begin(), rule.riders.end(),
                    [&](const ObjectContainmentRule::RiderInfo& rider) {
                        return templateMatchesRider(
                            passengerType, rider.templateName);
                    });
            if (!knownRider) continue;
        }
        uint64_t usedCapacity = 0;
        const auto accumulateRuleCapacity = [&]
            (const ObjectContainmentComponent* candidateContents,
             size_t candidateRuleIndex,
             const ObjectContainmentRule& candidateRule) {
            if (!candidateContents) return;
            for (const ObjectContainedObjectRecord& record :
                 candidateContents->objects) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromId(record.object);
                if (!passenger) continue;
                const ObjectContainedByComponent* edge =
                    ecs::try_get<ObjectContainedByComponent>(registry,
                                                              *passenger);
                if (!edge || edge->containmentRuleIndex !=
                        candidateRuleIndex) continue;
                usedCapacity += candidateRule.kind ==
                        ObjectContainmentKind::Parachute
                    ? 1u : ruleUsesTransportSlots(candidateRule.kind)
                    ? std::max<uint32_t>(1u,
                          transportSlotCount(registry, *passenger))
                    : 1u;
            }
        };
        accumulateRuleCapacity(contents, ruleIndex, rule);
        if (rule.kind == ObjectContainmentKind::Cave ||
            rule.kind == ObjectContainmentKind::Tunnel) {
            const OwnerComponent* networkOwner =
                ecs::try_get<OwnerComponent>(registry, container);
            const auto networkView = ecs::view<
                const ObjectContainmentRuntimeComponent,
                const ObjectContainmentComponent>(registry);
            for (const ecs::entity entrance : networkView) {
                if (entrance == container ||
                    !isCompletedContainmentEntrance(registry, entrance))
                    continue;
                const ObjectContainmentRuntimeComponent& otherRuntime =
                    networkView.template get<
                        const ObjectContainmentRuntimeComponent>(entrance);
                if (!otherRuntime.plan) continue;
                if (rule.kind == ObjectContainmentKind::Cave) {
                    if (!otherRuntime.hasCave || !runtime->hasCave ||
                        otherRuntime.caveIndex != runtime->caveIndex) continue;
                } else {
                    const OwnerComponent* otherOwner =
                        ecs::try_get<OwnerComponent>(registry, entrance);
                    if (!networkOwner || !otherOwner ||
                        networkOwner->player != otherOwner->player) continue;
                }
                const ObjectContainmentComponent& otherContents =
                    networkView.template get<
                        const ObjectContainmentComponent>(entrance);
                for (size_t otherRuleIndex = 0;
                     otherRuleIndex < otherRuntime.plan->rules.size();
                     ++otherRuleIndex) {
                    const ObjectContainmentRule& otherRule =
                        otherRuntime.plan->rules[otherRuleIndex];
                    if (otherRule.kind != rule.kind) continue;
                    accumulateRuleCapacity(&otherContents, otherRuleIndex,
                                           otherRule);
                }
            }
        }
        const bool parachuteSlotException =
            rule.kind == ObjectContainmentKind::Parachute &&
            objectKinds &&
            (game::objectHasKind(objectKinds->mask,
                                 game::ObjectKindOf::Infantry) ||
             game::objectHasKind(objectKinds->mask,
                                 game::ObjectKindOf::Parachutable));
        if (!force && ruleUsesTransportSlots(rule.kind) &&
            passengerSlots == 0 && !parachuteSlotException) continue;
        const uint64_t requestedCapacity = rule.kind ==
                ObjectContainmentKind::Parachute
            ? 1u : ruleUsesTransportSlots(rule.kind)
            ? std::max<uint32_t>(1u, passengerSlots) : 1u;
        const uint64_t effectiveCapacity =
            rule.kind == ObjectContainmentKind::Cave ||
                    rule.kind == ObjectContainmentKind::Tunnel
                ? runtime->networkCapacity
                : rule.containMax;
        if (!force && rule.kind != ObjectContainmentKind::RiderChange &&
            (usedCapacity > effectiveCapacity ||
            requestedCapacity >
                effectiveCapacity - usedCapacity)) continue;
        if (!force && !ruleAllowsPassenger(rule, objectKinds)) continue;
        if (!force && !ruleAllowsRelationship(
                rule, registry, container, object, players)) continue;
        return &rule;
    }
    return nullptr;
}

[[nodiscard]] uint64_t millisecondsToTicks(uint32_t milliseconds,
                                           uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    return std::max<uint64_t>(
        1, (static_cast<uint64_t>(milliseconds) * framesPerSecond + 999u) / 1000u);
}

[[nodiscard]] uint64_t saturatingAddTicks(uint64_t tick, uint64_t delay) noexcept {
    return delay > std::numeric_limits<uint64_t>::max() - tick
        ? std::numeric_limits<uint64_t>::max() : tick + delay;
}

[[nodiscard]] const ObjectContainmentRule* findContainmentRuleForObject(
    const ecs::registry& registry, ecs::entity container,
    ecs::entity object) {
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, container);
    if (!runtime || !runtime->plan) return nullptr;
    if (const ObjectContainedByComponent* edge =
            ecs::try_get<ObjectContainedByComponent>(registry, object);
        edge && edge->containmentRuleIndex < runtime->plan->rules.size()) {
        return &runtime->plan->rules[edge->containmentRuleIndex];
    }
    // Structural forced containment predates rule-aware admission.  Retain a
    // deterministic fallback for those edges only; ordinary requestAttach()
    // always records the exact selected rule above.
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, object);
    for (const ObjectContainmentRule& rule : runtime->plan->rules) {
        if (ruleAllowsPassenger(rule, kinds)) return &rule;
    }
    return nullptr;
}

[[nodiscard]] bool sameContainmentNetwork(
    const ecs::registry& registry, ecs::entity source,
    const ObjectContainmentRuntimeComponent& sourceRuntime,
    ObjectContainmentKind kind, ecs::entity candidate,
    const ObjectContainmentRuntimeComponent& candidateRuntime) noexcept {
    if (kind == ObjectContainmentKind::Cave) {
        return sourceRuntime.hasCave && candidateRuntime.hasCave &&
            sourceRuntime.caveIndex == candidateRuntime.caveIndex;
    }
    if (kind != ObjectContainmentKind::Tunnel) return source == candidate;
    const OwnerComponent* sourceOwner =
        ecs::try_get<OwnerComponent>(registry, source);
    const OwnerComponent* candidateOwner =
        ecs::try_get<OwnerComponent>(registry, candidate);
    return sourceOwner && candidateOwner && sourceOwner->player &&
        sourceOwner->player == candidateOwner->player;
}

[[nodiscard]] container::Vector<NetworkPassengerRecord>
collectNetworkPassengers(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity source, const ObjectContainmentRuntimeComponent& sourceRuntime,
    ObjectContainmentKind kind) {
    container::Vector<NetworkPassengerRecord> result;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectContainmentRuntimeComponent,
                                const ObjectContainmentComponent>(registry);
    for (const ecs::entity entrance : view) {
        if (!isCompletedContainmentEntrance(registry, entrance)) continue;
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entrance);
        const ObjectContainmentRuntimeComponent& runtime =
            view.template get<const ObjectContainmentRuntimeComponent>(
                entrance);
        if (!identity.id || !runtime.plan ||
            !sameContainmentNetwork(registry, source, sourceRuntime, kind,
                                    entrance, runtime)) {
            continue;
        }
        const ObjectContainmentComponent& contents =
            view.template get<const ObjectContainmentComponent>(entrance);
        for (const ObjectContainedObjectRecord& record : contents.objects) {
            const std::optional<ecs::entity> passenger =
                lifecycle.entityFromId(record.object);
            const ObjectContainedByComponent* edge = passenger
                ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                            *passenger)
                : nullptr;
            if (!edge || edge->container != identity.id ||
                edge->containmentRuleIndex >= runtime.plan->rules.size() ||
                runtime.plan->rules[edge->containmentRuleIndex].kind != kind) {
                continue;
            }
            result.push_back({record, identity.id, entrance,
                              edge->containmentRuleIndex});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const NetworkPassengerRecord& left,
                 const NetworkPassengerRecord& right) {
                  if (left.record.object != right.record.object)
                      return left.record.object < right.record.object;
                  return left.entrance < right.entrance;
              });
    result.erase(std::unique(
        result.begin(), result.end(),
        [](const NetworkPassengerRecord& left,
           const NetworkPassengerRecord& right) {
            return left.record.object == right.record.object;
        }), result.end());
    return result;
}

void collectNetworkPassengers(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    container::Span<const ContainmentUpdateCandidate> entrances,
    ObjectContainmentKind kind,
    container::Vector<NetworkPassengerRecord>& out) {
    out.clear();
    for (const ContainmentUpdateCandidate& candidate : entrances) {
        if (!candidate.container ||
            !isCompletedContainmentEntrance(registry, candidate.entity)) {
            continue;
        }
        const ObjectContainmentRuntimeComponent* runtime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(
                registry, candidate.entity);
        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(
                registry, candidate.entity);
        if (!runtime || !runtime->plan || !contents) continue;
        for (const ObjectContainedObjectRecord& record : contents->objects) {
            const std::optional<ecs::entity> passenger =
                lifecycle.entityFromId(record.object);
            const ObjectContainedByComponent* edge = passenger
                ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                            *passenger)
                : nullptr;
            if (!edge || edge->container != candidate.container ||
                edge->containmentRuleIndex >= runtime->plan->rules.size() ||
                runtime->plan->rules[edge->containmentRuleIndex].kind != kind) {
                continue;
            }
            out.push_back({record, candidate.container, candidate.entity,
                           edge->containmentRuleIndex});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const NetworkPassengerRecord& left,
                 const NetworkPassengerRecord& right) {
                  if (left.record.object != right.record.object)
                      return left.record.object < right.record.object;
                  return left.entrance < right.entrance;
              });
    out.erase(std::unique(
        out.begin(), out.end(),
        [](const NetworkPassengerRecord& left,
           const NetworkPassengerRecord& right) {
            return left.record.object == right.record.object;
        }), out.end());
}

[[nodiscard]] bool liveTunnelNemesis(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object) noexcept {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return false;
    if (const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *entity);
        health && health->effectivelyDead) {
        return false;
    }
    return true;
}

[[nodiscard]] bool visibleTunnelNemesis(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object) noexcept {
    if (!liveTunnelNemesis(registry, lifecycle, object)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return false;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, *entity);
    if (!status) return true;
    const bool stealthed = status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Stealthed));
    const bool detected = status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Detected));
    const bool disguised = status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Disguised));
    return !stealthed || detected || disguised;
}

[[nodiscard]] LogicFixedVec3 transformContainmentExitPoint(
    const LogicFixedVec3& origin, math::q32_32 yaw,
    math::q32_32 localX, math::q32_32 localY,
    math::q32_32 localZ) noexcept {
    const math::q32_32_sincos rotation = math::fixed_sincos(yaw);
    return {
        .x = origin.x + localX * rotation.cosine - localY * rotation.sine,
        .y = origin.y + localX * rotation.sine + localY * rotation.cosine,
        .z = origin.z + localZ,
    };
}

void applyTransportExitPolicy(
    ecs::registry& registry, ecs::entity container, ecs::entity object,
    const ObjectContainmentRule& rule,
    const ObjectContainmentExitPath* precisePath,
    uint32_t exitPath, uint32_t ruleIndex,
    uint32_t firstCommandSequence, uint32_t secondCommandSequence,
    uint64_t confirmedTick) {
    const ObjectFixedTransformComponent* objectTransform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, object);
    const ObjectFixedTransformComponent* containerTransform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, container);
    if (!objectTransform || !objectTransform->authoritative ||
        !containerTransform || !containerTransform->authoritative) return;

    LogicFixedVec3 position = objectTransform->position;
    const LogicFixedVec3 containerPosition = containerTransform->position;
    const math::q32_32 containerYaw = containerTransform->yawRadians;
    const ObjectIdentityComponent* containerIdentity =
        ecs::try_get<ObjectIdentityComponent>(registry, container);
    const ObjectIdentityComponent* objectIdentity =
        ecs::try_get<ObjectIdentityComponent>(registry, object);
    LogicFixedVec3 exitEnd{};
    // GarrisonContain owns its own Burst/Left/Right geometry and never uses
    // OpenContain ExitStart/ExitEnd paths even when a model happens to expose
    // bones with those names.
    const bool hasPrecisePath = precisePath && precisePath->valid &&
        rule.kind != ObjectContainmentKind::Garrison;
    bool scatterExit = false;
    bool geometryFallbackExit = false;
    bool garrisonBurstExit = false;
    math::q32_32 exitYaw = containerYaw;
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, container);
    const ObjectContainmentEvacuationDisposition evacuation = runtime
        ? runtime->evacuationDisposition
        : ObjectContainmentEvacuationDisposition::Invalid;
    const bool sideEvacuation =
        rule.kind == ObjectContainmentKind::Garrison &&
        (evacuation == ObjectContainmentEvacuationDisposition::Left ||
         evacuation == ObjectContainmentEvacuationDisposition::Right);
    if (sideEvacuation) {
        // GarrisonContain interprets left/right in container-local space:
        // left is +Y and right is -Y. Keep the authored doorway spread and
        // long outward destination deterministic without consulting render
        // bones or client state.
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, container);
        const math::q32_32 major = geometry
            ? math::q32_32::max(math::q32_32{int32_t{1}},
                                geometry->majorRadiusFixed)
            : math::q32_32{int32_t{1}};
        const math::q32_32 minor = geometry
            ? math::q32_32::max(math::q32_32{int32_t{1}},
                                geometry->minorRadiusFixed)
            : math::q32_32{int32_t{1}};
        const math::q32_32 side = math::q32_32{
            evacuation == ObjectContainmentEvacuationDisposition::Left
                ? int32_t{1} : int32_t{-1}};
        const uint32_t seed = exitPath * 1103515245u +
            firstCommandSequence * 12345u + ruleIndex * 2654435761u;
        const auto centeredFraction = [](uint32_t value) noexcept {
            constexpr int32_t kScale = 1024;
            const int32_t signedValue =
                static_cast<int32_t>(value % (2u * kScale + 1u)) - kScale;
            return math::q32_32{signedValue} / math::q32_32{kScale};
        };
        const math::q32_32 doorwayX =
            centeredFraction(seed) * major / math::q32_32{4};
        const math::q32_32 doorwayY =
            (minor / math::q32_32{2} +
             (math::q32_32{static_cast<int32_t>((seed >> 11u) % 1537u)} /
              math::q32_32{1024}) * minor) * side;
        const math::q32_32 destinationX =
            centeredFraction(seed >> 7u) * major;
        const math::q32_32 destinationY = minor * math::q32_32{10} * side;
        position = transformContainmentExitPoint(
            containerPosition, containerYaw, doorwayX, doorwayY, {});
        exitEnd = transformContainmentExitPoint(
            containerPosition, containerYaw,
            destinationX, destinationY, {});
    } else if (rule.kind == ObjectContainmentKind::Garrison) {
        // The enclosing form appears at the host centre; a station-style
        // garrison starts from its current STATION position. In both cases
        // the one-point route is adjusted by ObjectAI navigation, matching
        // the legacy add-to-pathfind-map -> adjustToPossibleDestination ->
        // aiFollowPath handoff.
        garrisonBurstExit = true;
        if (rule.enclosingContainer) position = containerPosition;
        exitEnd = position;
    } else if (hasPrecisePath &&
               !(rule.kind == ObjectContainmentKind::MobNexus &&
                 rule.scatterNearbyOnExit)) {
        position = transformContainmentExitPoint(
            containerPosition, containerYaw,
            precisePath->startX, precisePath->startY, precisePath->startZ);
        exitEnd = transformContainmentExitPoint(
            containerPosition, containerYaw,
            precisePath->endX, precisePath->endY, precisePath->endZ);
    } else if (rule.scatterNearbyOnExit) {
        // OpenContain::scatterToNearbyPosition chooses a point one to one and
        // a half host radii away, starts AI passengers at the host centre and
        // sends them outward. Derive the two random fractions from stable
        // transaction facts so the modern lockstep path does not consume a
        // process-global RNG stream.
        scatterExit = true;
        uint64_t key = confirmedTick ^
            (static_cast<uint64_t>(containerIdentity
                ? containerIdentity->id.value : 0u) << 32u) ^
            static_cast<uint64_t>(objectIdentity
                ? objectIdentity->id.value : 0u) ^
            (static_cast<uint64_t>(ruleIndex) << 17u) ^ exitPath;
        key ^= key >> 30u;
        key *= 0xbf58476d1ce4e5b9ull;
        key ^= key >> 27u;
        key *= 0x94d049bb133111ebull;
        key ^= key >> 31u;

        constexpr math::q32_32 kTwoPi =
            math::q32_32::from_raw(26986075409ll);
        constexpr int64_t kFractionScale = 65536;
        const int64_t angleSample = static_cast<int64_t>(key & 0xffffu);
        exitYaw = math::q32_32::from_raw(
            kTwoPi.raw() * angleSample / kFractionScale);
        const math::q32_32 distanceFraction =
            math::q32_32{static_cast<int32_t>((key >> 16u) & 0xffffu)} /
            math::q32_32{static_cast<int32_t>(kFractionScale)};
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, container);
        const math::q32_32 radius = geometry
            ? math::q32_32::max(math::q32_32{int32_t{1}},
                                geometry->majorRadiusFixed)
            : math::q32_32{int32_t{1}};
        const math::q32_32 distance =
            radius + radius * distanceFraction / math::q32_32{2};
        const math::q32_32_sincos direction = math::fixed_sincos(exitYaw);
        exitEnd = {
            containerPosition.x + direction.cosine * distance,
            containerPosition.y + direction.sine * distance,
            containerPosition.z,
        };
        if (ecs::try_get<ObjectLocomotionComponent>(registry, object)) {
            position = containerPosition;
        } else {
            position = exitEnd;
        }
    } else if (rule.kind == ObjectContainmentKind::Transport &&
               !hasPrecisePath) {
        // RefCode resolves ExitStart/ExitEnd from the live Drawable.  The
        // simulation cannot query a render Drawable, so a missing pristine
        // bone record must not turn a valid TransportContain exit into a
        // passenger permanently left at the host center.  Keep the original
        // overlap-at-release behavior and provide the same next step: a
        // short deterministic path through the host's forward edge.  The
        // host remains the ignored obstacle for this route, and normal
        // navigation/collision then takes over after the exit.
        geometryFallbackExit = true;
        position = containerPosition;
        const ObjectGeometryComponent* containerGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, container);
        const ObjectGeometryComponent* objectGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, object);
        const std::optional<LogicFixedVec3> evacuationTarget =
            containerGeometry && objectGeometry && objectIdentity
            ? objectFootprintEvacuationTarget(
                  containerPosition, containerYaw, *containerGeometry,
                  containerPosition, *objectGeometry, objectIdentity->id,
                  math::q32_32{int32_t{1}})
            : std::nullopt;
        if (evacuationTarget) {
            exitEnd = *evacuationTarget;
        } else {
            const math::q32_32 containerRadius = containerGeometry
                ? math::q32_32::max(math::q32_32{int32_t{1}},
                                    containerGeometry->majorRadiusFixed)
                : math::q32_32{int32_t{1}};
            const math::q32_32 containerHalfWidth = containerGeometry
                ? math::q32_32::max(math::q32_32{},
                                    containerGeometry->minorRadiusFixed)
                : math::q32_32{};
            const math::q32_32 objectRadius = objectGeometry
                ? math::q32_32::max(
                      math::q32_32{},
                      objectGeometry->boundingCircleRadiusFixed)
                : math::q32_32{};
            const math::q32_32 distance = containerRadius + objectRadius +
                math::q32_32{int32_t{1}};
            math::q32_32 lateral{};
            if (rule.numberOfExitPaths > 1) {
                const int32_t lane = static_cast<int32_t>(
                    std::min(exitPath, rule.numberOfExitPaths) - 1u);
                const int32_t centeredLane = lane * 2 -
                    static_cast<int32_t>(rule.numberOfExitPaths - 1u);
                lateral = containerHalfWidth *
                    math::q32_32{centeredLane} /
                    math::q32_32{static_cast<int32_t>(
                        rule.numberOfExitPaths - 1u)};
            }
            exitEnd = transformContainmentExitPoint(
                containerPosition, containerYaw, distance, lateral, {});
        }
    }
    math::q32_32 objectYaw = objectTransform->yawRadians;
    if (scatterExit) {
        objectYaw = exitYaw;
    } else if (sideEvacuation || hasPrecisePath || geometryFallbackExit ||
               rule.orientLikeContainerOnExit) {
        objectYaw = containerTransform->yawRadians;
    }
    writeAuthoritativeObjectTransform(
        registry, object, position, objectYaw);

    // OpenContain::removeFromContainViaIterator always transfers the host's
    // current pathfind layer before any exit-path adjustment.  Keep that
    // durable fact independent of Transform Z so bridge exits do not select
    // the overlapping ground surface.
    const ObjectTerrainLayerComponent* containerLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, container);
    if (containerLayer) {
        ObjectTerrainLayerComponent* objectLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry, object);
        if (objectLayer) {
            static_cast<void>(objectLayer->assign(
                containerLayer->pathfindLayer, confirmedTick));
        } else {
            ecs::emplace<ObjectTerrainLayerComponent>(
                registry, object,
                ObjectTerrainLayerComponent{
                    .pathfindLayer = containerLayer->pathfindLayer,
                    .lastChangedTick = confirmedTick,
                });
        }
    }

    ObjectPhysicsComponent* objectPhysics =
        ecs::try_get<ObjectPhysicsComponent>(registry, object);
    const ObjectPhysicsComponent* containerPhysics =
        ecs::try_get<ObjectPhysicsComponent>(registry, container);
    if (objectPhysics) {
        objectPhysics->position = position;
        objectPhysics->lastPublishedPosition = position;
        objectPhysics->hasAuthoritativePosition = true;
        if (scatterExit) objectPhysics->yaw = exitYaw;
        else if (sideEvacuation || hasPrecisePath || geometryFallbackExit ||
                 rule.orientLikeContainerOnExit)
            objectPhysics->yaw = containerYaw;
        if (sideEvacuation || hasPrecisePath) {
            objectPhysics->pitch = {};
            objectPhysics->roll = {};
            objectPhysics->ownsAttitude = false;
        }
        if (rule.keepContainerVelocityOnExit && containerPhysics) {
            objectPhysics->velocityUnitsPerSecond =
                containerPhysics->velocityUnitsPerSecond;
        }
        if (rule.keepContainerVelocityOnExit &&
            rule.exitPitchRate != math::q32_32{}) {
            // TransportContain applies ExitPitchRate through the passenger's
            // Physics center-of-mass offset; a centered object receives no
            // artificial tumble, while an offset chassis pitches with the
            // authored sign and magnitude.
            objectPhysics->pitchRate =
                objectPhysics->centerOfMassOffset * rule.exitPitchRate;
            objectPhysics->ownsAttitude = true;
        }
    }

    if (ecs::try_get<ObjectLocomotionComponent>(registry, object)) {
        const uint64_t untilTick = saturatingAddTicks(confirmedTick, 30u);
        if (ObjectTemporaryCollisionIgnoreComponent* ignore =
                ecs::try_get<ObjectTemporaryCollisionIgnoreComponent>(
                    registry, object)) {
            ignore->untilTick = std::max(ignore->untilTick, untilTick);
            ignore->other = containerIdentity
                ? containerIdentity->id : INVALID_OBJECT_ID;
        } else {
            ecs::emplace<ObjectTemporaryCollisionIgnoreComponent>(
                registry, object,
                ObjectTemporaryCollisionIgnoreComponent{
                    .untilTick = untilTick,
                    .other = containerIdentity
                        ? containerIdentity->id : INVALID_OBJECT_ID,
                });
        }
    }

    if (!scatterExit && !sideEvacuation && !geometryFallbackExit &&
        !garrisonBurstExit &&
        (!hasPrecisePath || !precisePath->hasEnd)) return;
    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, object);
    if (!queue) return;
    if (scatterExit || garrisonBurstExit) {
        while (queue->orders.size() + 1u >
               ObjectOrderQueueComponent::MaximumQueuedOrders) {
            queue->orders.pop_back();
        }
        queue->orders.insert(queue->orders.begin(), ObjectOrderIntent{
            .kind = ObjectOrderKind::Move,
            .source = ObjectOrderSource::System,
            .issuedTick = confirmedTick,
            .sourceSequence = firstCommandSequence,
            .targetX = exitEnd.x,
            .targetY = exitEnd.y,
            .targetZ = exitEnd.z,
            .hasTargetPosition = true,
            .moveRouteSubtype = ObjectMoveRouteSubtype::Direct,
            .systemPurpose = ObjectOrderSystemPurpose::ContainmentExit,
            .systemPurposeInstance = ruleIndex,
        });
        ++queue->revision;
        return;
    }
    while (queue->orders.size() + 2u >
           ObjectOrderQueueComponent::MaximumQueuedOrders) {
        queue->orders.pop_back();
    }
    // OpenContain deliberately appends ExitEnd twice: the second waypoint
    // makes a briefly-airborne/stacked unit retry the exact egress point.
    queue->orders.insert(queue->orders.begin(), ObjectOrderIntent{
        .kind = ObjectOrderKind::Move,
        .source = ObjectOrderSource::System,
        .issuedTick = confirmedTick,
        .sourceSequence = secondCommandSequence,
        .targetX = exitEnd.x,
        .targetY = exitEnd.y,
        .targetZ = exitEnd.z,
        .hasTargetPosition = true,
        .systemPurpose = ObjectOrderSystemPurpose::ContainmentExit,
        .systemPurposeInstance = ruleIndex,
    });
    queue->orders.insert(queue->orders.begin(), ObjectOrderIntent{
        .kind = ObjectOrderKind::Move,
        .source = ObjectOrderSource::System,
        .issuedTick = confirmedTick,
        .sourceSequence = firstCommandSequence,
        .targetX = exitEnd.x,
        .targetY = exitEnd.y,
        .targetZ = exitEnd.z,
        .hasTargetPosition = true,
        .moveRouteSubtype = ObjectMoveRouteSubtype::FollowPath,
        .systemPurpose = ObjectOrderSystemPurpose::ContainmentExit,
        .systemPurposeInstance = ruleIndex,
    });
    ObjectSystemPathSequenceComponent route{
        .routeSubtype = ObjectMoveRouteSubtype::FollowPath,
        .systemPurpose = ObjectOrderSystemPurpose::ContainmentExit,
        .ignoredObstacle = containerIdentity
            ? containerIdentity->id : INVALID_OBJECT_ID,
        .issuedTick = confirmedTick,
        .firstSourceSequence = firstCommandSequence,
        .queuedOrderCount = 2,
        .points = {exitEnd, exitEnd},
    };
    if (ObjectSystemPathSequenceComponent* existing =
            ecs::try_get<ObjectSystemPathSequenceComponent>(
                registry, object)) {
        *existing = std::move(route);
    } else {
        ecs::emplace<ObjectSystemPathSequenceComponent>(
            registry, object, std::move(route));
    }
    ++queue->revision;
}

void projectContainmentDoorTransition(ecs::registry& registry,
                                      ecs::entity container,
                                      bool opening,
                                      uint64_t confirmedTick) {
    publishObjectModelConditionDoor(
        registry, container, ObjectModelConditionDoorSource::Containment, 0,
        opening ? ObjectModelConditionDoorPhase::Opening
                : ObjectModelConditionDoorPhase::Closing,
        confirmedTick);
}

[[nodiscard]] size_t behaviorRuleIndex(
    const ObjectContainmentRuntimeComponent& runtime,
    ObjectTransportBehaviorKind kind,
    uint32_t authoredOrder) noexcept {
    if (!runtime.plan) return std::numeric_limits<size_t>::max();
    for (size_t index = 0; index < runtime.plan->behaviorRules.size(); ++index) {
        const ObjectTransportBehaviorRule& rule =
            runtime.plan->behaviorRules[index];
        if (rule.kind == kind &&
            (authoredOrder == std::numeric_limits<uint32_t>::max() ||
             rule.authoredOrder == authoredOrder)) {
            return index;
        }
    }
    return std::numeric_limits<size_t>::max();
}

[[nodiscard]] ObjectTransportBehaviorKind requestBehaviorKind(
    ObjectTransportBehaviorRequestKind kind) noexcept {
    switch (kind) {
    case ObjectTransportBehaviorRequestKind::BunkerBust:
        return ObjectTransportBehaviorKind::BunkerBuster;
    case ObjectTransportBehaviorRequestKind::BattleBusStartUndeath:
    case ObjectTransportBehaviorRequestKind::BattleBusLanded:
        return ObjectTransportBehaviorKind::BattleBusSlowDeath;
    case ObjectTransportBehaviorRequestKind::HijackTarget:
    case ObjectTransportBehaviorRequestKind::ReleaseHijacker:
        return ObjectTransportBehaviorKind::Hijacker;
    case ObjectTransportBehaviorRequestKind::PilotFindVehicle:
        return ObjectTransportBehaviorKind::PilotFindVehicle;
    case ObjectTransportBehaviorRequestKind::AssaultTransportUpdate:
        return ObjectTransportBehaviorKind::AssaultTransportAI;
    case ObjectTransportBehaviorRequestKind::DeliverPayload:
        return ObjectTransportBehaviorKind::DeliverPayloadAI;
    }
    return ObjectTransportBehaviorKind::TransportAI;
}

ObjectTransportOclTransaction freezeTransportOclTransaction(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick, uint32_t authoredOrder,
    container::String payload, uint32_t sourcePathfindLayer) {
    ObjectTransportOclTransaction transaction{
        .source = object,
        .objectCreationList = std::move(payload),
        .sourcePathfindLayer = sourcePathfindLayer,
        .authoredOrder = authoredOrder,
        .confirmedTick = confirmedTick,
    };
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(registry, *entity) : nullptr;
    const PrimaryTeamComponent* team = entity
        ? ecs::try_get<PrimaryTeamComponent>(registry, *entity) : nullptr;
    const TransformComponent* transform = entity
        ? ecs::try_get<TransformComponent>(registry, *entity) : nullptr;
    if (entity && owner && team && transform) {
        const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, *entity);
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry, *entity);
        transaction.owner = owner->player;
        transaction.primaryTeam = team->team;
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            registry, *entity, *transform);
        transaction.primaryX = position.x;
        transaction.primaryY = position.y;
        transaction.primaryZ = position.z;
        if (physics) {
            transaction.sourceVelocityX =
                physics->velocityUnitsPerSecond.x;
            transaction.sourceVelocityY =
                physics->velocityUnitsPerSecond.y;
            transaction.sourceVelocityZ =
                physics->velocityUnitsPerSecond.z;
        }
        transaction.orientationRadians = physics && physics->ownsAttitude
            ? physics->yaw
            : readAuthoritativeObjectYaw(registry, *entity, *transform);
        transaction.pitchRadians = physics && physics->ownsAttitude
            ? physics->pitch : ObjectPhysicsComponent::Scalar{};
        transaction.rollRadians = physics && physics->ownsAttitude
            ? physics->roll : ObjectPhysicsComponent::Scalar{};
        transaction.sourceAirborne = airborne && airborne->isAirborne;
        transaction.sourceOwnsFullAttitude =
            physics && physics->ownsAttitude;
        transaction.hasFrozenSource = true;
    }
    return transaction;
}

void pushTransportOclTransaction(
    ObjectTransportEventStream& events, const ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick, uint32_t authoredOrder,
    container::String payload, uint32_t sourcePathfindLayer,
    uint64_t& nextGameplaySubmissionOrdinal) {
    pushTransportGameplayTransaction(
        events, freezeTransportOclTransaction(
            registry, lifecycle, object, confirmedTick, authoredOrder,
            std::move(payload), sourcePathfindLayer),
        nextGameplaySubmissionOrdinal);
}

[[nodiscard]] math::q32_32 deterministicVariance(
    math::q32_32 extent, uint64_t seed) noexcept {
    if (extent <= math::q32_32{}) return {};
    seed ^= seed >> 30u;
    seed *= 0xbf58476d1ce4e5b9ull;
    seed ^= seed >> 27u;
    seed *= 0x94d049bb133111ebull;
    seed ^= seed >> 31u;
    const int64_t signedUnit = static_cast<int64_t>(seed & 0xffffffffull) -
        static_cast<int64_t>(0x80000000ull);
    return extent * math::q32_32::from_fraction(
        signedUnit, static_cast<int64_t>(0x80000000ull));
}

void setHijackerStatus(ecs::registry& registry, ecs::entity entity,
                       bool attached, uint64_t confirmedTick) {
    const game::ObjectStatusMask mask =
        game::objectStatusBit(game::ObjectStatusFlag::Unselectable) |
        game::objectStatusBit(game::ObjectStatusFlag::NoCollisions) |
        game::objectStatusBit(game::ObjectStatusFlag::Masked);
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity, {
            .setMask = attached ? mask : 0,
            .clearMask = attached ? 0 : mask,
            .confirmedTick = confirmedTick,
        }));
    if (RenderModelComponent* render =
            ecs::try_get<RenderModelComponent>(registry, entity))
        render->hidden = attached;
}

void setSystemMoveOrder(ecs::registry& registry, ecs::entity entity,
                        ObjectId target, const LogicFixedVec3& position,
                        ObjectOrderSystemPurpose purpose,
                        uint32_t instance, uint64_t confirmedTick) {
    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
    if (!queue)
        queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, entity);
    const bool same = !queue->orders.empty() &&
        queue->orders.front().source == ObjectOrderSource::System &&
        queue->orders.front().systemPurpose == purpose &&
        queue->orders.front().systemPurposeInstance == instance &&
        queue->orders.front().targetObject == target;
    if (same) {
        queue->orders.front().targetX = position.x;
        queue->orders.front().targetY = position.y;
        queue->orders.front().targetZ = position.z;
        return;
    }
    queue->orders.clear();
    queue->orders.push_back({
        .kind = ObjectOrderKind::Move,
        .source = ObjectOrderSource::System,
        .issuedTick = confirmedTick,
        .sourceSequence = instance,
        .targetObject = target,
        .targetX = position.x,
        .targetY = position.y,
        .targetZ = position.z,
        .hasTargetPosition = true,
        .systemPurpose = purpose,
        .systemPurposeInstance = instance,
    });
    ++queue->revision;
}

void clearSystemOrder(ecs::registry& registry, ecs::entity entity,
                      ObjectOrderSystemPurpose purpose,
                      uint32_t instance) {
    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
    if (!queue || queue->orders.empty()) return;
    const ObjectOrderIntent& order = queue->orders.front();
    if (order.source != ObjectOrderSource::System ||
        order.systemPurpose != purpose ||
        order.systemPurposeInstance != instance) return;
    queue->orders.clear();
    ++queue->revision;
}

void setSystemAttackOrder(ecs::registry& registry, ecs::entity entity,
                          ObjectId target, const LogicFixedVec3& position,
                          bool hasPosition, bool attackMove,
                          ObjectOrderSystemPurpose purpose,
                          uint32_t instance, uint64_t confirmedTick) {
    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
    if (!queue)
        queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, entity);
    queue->orders.clear();
    queue->orders.push_back({
        .kind = attackMove ? ObjectOrderKind::Move : ObjectOrderKind::Attack,
        .source = ObjectOrderSource::System,
        .issuedTick = confirmedTick,
        .sourceSequence = instance,
        .targetObject = target,
        .targetX = position.x,
        .targetY = position.y,
        .targetZ = position.z,
        .hasTargetPosition = hasPosition,
        .forceAttack = false,
        .attackMove = attackMove,
        .systemPurpose = purpose,
        .systemPurposeInstance = instance,
    });
    ++queue->revision;
}

void setSystemAttackOrderAfterContainmentExit(
    ecs::registry& registry, ecs::entity entity, ObjectId target,
    const LogicFixedVec3& position, bool hasPosition, bool attackMove,
    ObjectOrderSystemPurpose purpose, uint32_t instance,
    uint64_t confirmedTick) {
    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
    if (!queue)
        queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, entity);

    // requestDetach installs one or two front-of-queue ContainmentExit moves.
    // A normal setSystemAttackOrder deliberately replaces all system intent;
    // doing that here would issue the combat order while the passenger is
    // still inside the host and leave every passenger at the same origin.
    auto exitEnd = queue->orders.begin();
    while (exitEnd != queue->orders.end() &&
           exitEnd->source == ObjectOrderSource::System &&
           exitEnd->systemPurpose == ObjectOrderSystemPurpose::ContainmentExit) {
        ++exitEnd;
    }
    if (exitEnd == queue->orders.begin()) {
        setSystemAttackOrder(registry, entity, target, position, hasPosition,
                             attackMove, purpose, instance, confirmedTick);
        return;
    }

    // Keep only the egress prefix.  This gives a refreshed assault target the
    // same replacement semantics as a normal system order without allowing a
    // stale pre-exit command to survive behind it.
    queue->orders.erase(exitEnd, queue->orders.end());
    while (queue->orders.size() >=
           ObjectOrderQueueComponent::MaximumQueuedOrders) {
        queue->orders.pop_back();
    }
    queue->orders.push_back({
        .kind = attackMove ? ObjectOrderKind::Move : ObjectOrderKind::Attack,
        .source = ObjectOrderSource::System,
        .issuedTick = confirmedTick,
        .sourceSequence = instance,
        .targetObject = target,
        .targetX = position.x,
        .targetY = position.y,
        .targetZ = position.z,
        .hasTargetPosition = hasPosition,
        .forceAttack = false,
        .attackMove = attackMove,
        .systemPurpose = purpose,
        .systemPurposeInstance = instance,
    });
    ++queue->revision;
}

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf kind) noexcept {
    return kinds && game::objectHasKind(kinds->mask, kind);
}

void pushContainmentEvent(container::Vector<ObjectContainmentEvent>& events,
                          ObjectContainmentRequestKind kind,
                          ObjectId container, ObjectId object,
                          uint64_t confirmedTick, bool accepted,
                          bool exposeStealthUnits,
                          ObjectId parachuteLandingTransport) {
    events.push_back({
        .kind = kind,
        .container = container,
        .object = object,
        .confirmedTick = confirmedTick,
        .accepted = accepted,
        .exposeStealthUnits = exposeStealthUnits,
        .parachuteLandingTransport = parachuteLandingTransport,
    });
}

namespace {

struct ContainmentAttachmentTransform final {
    LogicFixedVec3 position{};
    math::q32_32 yawRadians{};
};

[[nodiscard]] math::q32_32 fixedQuaternionYaw(
    const data::w3d::FixedQuaternion& rotation) noexcept {
    const math::q32_32 two{int32_t{2}};
    const math::q32_32 one{int32_t{1}};
    return math::fixed_atan2(
        two * (rotation.w * rotation.z + rotation.x * rotation.y),
        one - two * (rotation.y * rotation.y +
                     rotation.z * rotation.z));
}

[[nodiscard]] LogicFixedVec3 rotateAttachmentZ(
    const LogicFixedVec3& point, const LogicFixedVec3& pivot,
    math::q32_32 radians) noexcept {
    const math::q32_32_sincos angle = math::fixed_sincos(radians);
    const math::q32_32 x = point.x - pivot.x;
    const math::q32_32 y = point.y - pivot.y;
    return {
        .x = pivot.x + x * angle.cosine - y * angle.sine,
        .y = pivot.y + x * angle.sine + y * angle.cosine,
        .z = point.z,
    };
}

[[nodiscard]] LogicFixedVec3 rotateAttachmentY(
    const LogicFixedVec3& point, const LogicFixedVec3& pivot,
    math::q32_32 radians) noexcept {
    const math::q32_32_sincos angle = math::fixed_sincos(radians);
    const math::q32_32 x = point.x - pivot.x;
    const math::q32_32 z = point.z - pivot.z;
    return {
        .x = pivot.x + x * angle.cosine + z * angle.sine,
        .y = point.y,
        .z = pivot.z - x * angle.sine + z * angle.cosine,
    };
}

[[nodiscard]] container::String containerAttachmentBone(
    const ecs::registry& registry, ecs::entity object,
    ecs::entity container) {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, object);
    if (type && type->archetype) {
        for (const game::ModelDrawVisualChannel& channel :
             type->archetype->templateData.drawVisualChannels) {
            if (!channel.attachToBoneInContainer.empty())
                return channel.attachToBoneInContainer;
        }
    }

    const ObjectContainedByComponent* edge =
        ecs::try_get<ObjectContainedByComponent>(registry, object);
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, container);
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, container);
    const ObjectIdentityComponent* identity =
        ecs::try_get<ObjectIdentityComponent>(registry, object);
    const ObjectIdentityComponent* containerIdentity =
        ecs::try_get<ObjectIdentityComponent>(registry, container);
    if (!edge || edge->enclosing || !runtime || !runtime->plan ||
        edge->containmentRuleIndex >= runtime->plan->rules.size() ||
        runtime->plan->rules[edge->containmentRuleIndex].kind !=
            ObjectContainmentKind::Garrison ||
        !contents || !identity || !identity->id || !containerIdentity ||
        !containerIdentity->id) {
        return {};
    }

    // Non-enclosing GarrisonContain assigns host STATION01.. bones. Preserve
    // the confirmed containment admission order even though the shared host
    // roster itself remains ObjectId-sorted for deterministic lookup.
    const auto ownRecord = std::lower_bound(
        contents->objects.begin(), contents->objects.end(), identity->id,
        [](const ObjectContainedObjectRecord& record, ObjectId object) {
            return record.object < object;
        });
    if (ownRecord == contents->objects.end() ||
        ownRecord->object != identity->id) {
        return {};
    }
    uint32_t station = 1;
    const auto passengerView = ecs::view<
        const ObjectIdentityComponent,
        const ObjectContainedByComponent>(registry);
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        if (record.object == identity->id) continue;
        bool sameGarrisonRule = false;
        for (const ecs::entity passenger : passengerView) {
            const ObjectIdentityComponent& passengerIdentity =
                passengerView.template get<
                    const ObjectIdentityComponent>(passenger);
            if (passengerIdentity.id != record.object) continue;
            const ObjectContainedByComponent& passengerEdge =
                passengerView.template get<
                    const ObjectContainedByComponent>(passenger);
            sameGarrisonRule =
                passengerEdge.container == containerIdentity->id &&
                passengerEdge.containmentRuleIndex ==
                    edge->containmentRuleIndex &&
                !passengerEdge.enclosing;
            break;
        }
        if (!sameGarrisonRule) continue;
        if (record.entryOrdinal < ownRecord->entryOrdinal ||
            (record.entryOrdinal == ownRecord->entryOrdinal &&
             record.object < identity->id)) {
            ++station;
        }
    }
    const uint32_t stationLimit = runtime->plan->rules[
        edge->containmentRuleIndex].containMax;
    if (station == 0 || station > stationLimit || station > 99) return {};
    container::String result{"STATION"};
    result.push_back(static_cast<char>('0' + station / 10));
    result.push_back(static_cast<char>('0' + station % 10));
    return result;
}

[[nodiscard]] std::optional<ContainmentAttachmentTransform>
containerAttachmentTransform(
    const ecs::registry& registry, ecs::entity object,
    ecs::entity container, const GameContentSnapshot* content) noexcept {
    const ObjectContainedByComponent* edge =
        ecs::try_get<ObjectContainedByComponent>(registry, object);
    const ObjectContainmentRuntimeComponent* containment =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, container);
    const ObjectFixedTransformComponent* hostTransform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, container);
    const ThingTemplateComponent* hostType =
        ecs::try_get<ThingTemplateComponent>(registry, container);
    const RenderModelComponent* hostVisual =
        ecs::try_get<RenderModelComponent>(registry, container);
    const game::W3dPristineBoneCatalog* catalog = content
        ? content->pristineBoneCatalog() : nullptr;
    const container::String boneName =
        containerAttachmentBone(registry, object, container);
    if (!edge || edge->enclosing || !containment || !containment->plan ||
        edge->containmentRuleIndex >= containment->plan->rules.size() ||
        !hostTransform || !hostTransform->authoritative || !hostType ||
        !hostType->archetype || !hostVisual || !catalog ||
        !catalog->isLoaded() || boneName.empty()) {
        return std::nullopt;
    }

    const ObjectContainmentRule& containmentRule =
        containment->plan->rules[edge->containmentRuleIndex];
    const game::ThingTemplate& templateData =
        hostType->archetype->templateData;
    std::optional<data::w3d::FixedRigidTransform> bone;
    game::ModelTurretBoneDefinition parentTurret;
    std::optional<data::w3d::FixedRigidTransform> yawPivot;
    std::optional<data::w3d::FixedRigidTransform> pitchPivot;
    size_t flattenedVisualOffset = 0;
    for (const game::ModelDrawVisualChannel& channel :
         templateData.drawVisualChannels) {
        const size_t visualRule = game::selectModelConditionVisualRuleIndex(
            channel, hostVisual->modelConditionFlags);
        if (visualRule < channel.conditionVisuals.size()) {
            const size_t flattenedRule = flattenedVisualOffset + visualRule;
            bone = catalog->find(
                hostType->archetype->name, flattenedRule, boneName);
            if (bone) {
                parentTurret = channel.conditionVisuals[visualRule].turrets[0];
                if (!parentTurret.yawBone.empty()) {
                    yawPivot = catalog->find(
                        hostType->archetype->name, flattenedRule,
                        parentTurret.yawBone);
                }
                if (!parentTurret.pitchBone.empty()) {
                    pitchPivot = catalog->find(
                        hostType->archetype->name, flattenedRule,
                        parentTurret.pitchBone);
                }
                break;
            }
        }
        flattenedVisualOffset += channel.conditionVisuals.size();
    }
    if (!bone && templateData.drawVisualChannels.empty()) {
        const size_t visualRule = game::selectModelConditionVisualRuleIndex(
            templateData, hostVisual->modelConditionFlags);
        if (visualRule < templateData.modelConditionVisuals.size()) {
            bone = catalog->find(
                hostType->archetype->name, visualRule, boneName);
            if (bone) {
                parentTurret =
                    templateData.modelConditionVisuals[visualRule].turrets[0];
                if (!parentTurret.yawBone.empty()) {
                    yawPivot = catalog->find(
                        hostType->archetype->name, visualRule,
                        parentTurret.yawBone);
                }
                if (!parentTurret.pitchBone.empty()) {
                    pitchPivot = catalog->find(
                        hostType->archetype->name, visualRule,
                        parentTurret.pitchBone);
                }
            }
        }
    }
    if (!bone) return std::nullopt;

    LogicFixedVec3 local{
        .x = bone->translation.x,
        .y = bone->translation.y,
        .z = bone->translation.z,
    };
    math::q32_32 localYaw = fixedQuaternionYaw(bone->rotation);
    if (containmentRule.passengersInTurret) {
        // OpenContain::putObjAtNextFirePoint asks the host's logical turret
        // for the complete FIREPOINT matrix before writing the non-enclosing
        // rider transform. Apply the same pristine-art correction and live
        // turret state here, entirely from deterministic state.
        local = rotateAttachmentZ(
            local, {}, parentTurret.artYawRadiansFixed);
        local = rotateAttachmentY(
            local, {}, -parentTurret.artPitchRadiansFixed);
        localYaw += parentTurret.artYawRadiansFixed;
        if (const ObjectWeaponComponent* hostWeapons =
                ecs::try_get<ObjectWeaponComponent>(registry, container)) {
            const ObjectTurretRuntime& turret = hostWeapons->turrets[0];
            LogicFixedVec3 runtimePitchPivot{};
            if (pitchPivot) {
                runtimePitchPivot = {
                    .x = pitchPivot->translation.x,
                    .y = pitchPivot->translation.y,
                    .z = pitchPivot->translation.z,
                };
            }
            LogicFixedVec3 runtimeYawPivot{};
            if (yawPivot) {
                runtimeYawPivot = {
                    .x = yawPivot->translation.x,
                    .y = yawPivot->translation.y,
                    .z = yawPivot->translation.z,
                };
            }
            local = rotateAttachmentY(
                local, runtimePitchPivot, -turret.pitchRadians);
            local = rotateAttachmentZ(
                local, runtimeYawPivot, turret.yawRadians);
            localYaw += turret.yawRadians;
        }
    }

    const math::q32_32 hostYaw = hostTransform->yawRadians;
    const math::q32_32_sincos direction = math::fixed_sincos(hostYaw);
    return ContainmentAttachmentTransform{
        .position = {
            .x = hostTransform->position.x +
                local.x * direction.cosine - local.y * direction.sine,
            .y = hostTransform->position.y +
                local.x * direction.sine + local.y * direction.cosine,
            .z = hostTransform->position.z + local.z,
        },
        .yawRadians = hostYaw + localYaw,
    };
}

} // namespace

void synchronizeOne(ecs::registry& registry, ecs::entity object,
                    ecs::entity container,
                    const GameContentSnapshot* content) noexcept {
    const ObjectFixedTransformComponent* objectTransform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, object);
    const ObjectFixedTransformComponent* containerTransform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, container);
    if (!objectTransform || !objectTransform->authoritative ||
        !containerTransform || !containerTransform->authoritative) return;

    const std::optional<ContainmentAttachmentTransform> attachment =
        containerAttachmentTransform(
            registry, object, container, content);
    const LogicFixedVec3 position = attachment
        ? attachment->position : containerTransform->position;
    const math::q32_32 yaw = attachment
        ? attachment->yawRadians : containerTransform->yawRadians;
    writeAuthoritativeObjectTransform(
        registry, object, position, yaw);

    if (ObjectPhysicsComponent* objectPhysics =
            ecs::try_get<ObjectPhysicsComponent>(registry, object)) {
        objectPhysics->position = position;
        objectPhysics->lastPublishedPosition = position;
        objectPhysics->hasAuthoritativePosition = true;
        if (const ObjectPhysicsComponent* containerPhysics =
                ecs::try_get<ObjectPhysicsComponent>(registry, container)) {
            objectPhysics->velocityUnitsPerSecond =
                containerPhysics->velocityUnitsPerSecond;
            objectPhysics->yaw = yaw;
            objectPhysics->pitch = containerPhysics->pitch;
            objectPhysics->roll = containerPhysics->roll;
            objectPhysics->ownsAttitude = containerPhysics->ownsAttitude;
            objectPhysics->conformsToTerrain =
                containerPhysics->conformsToTerrain;
            objectPhysics->orientationBasisValid = false;
        } else {
            objectPhysics->velocityUnitsPerSecond = {};
            objectPhysics->yaw = yaw;
            objectPhysics->pitch = {};
            objectPhysics->roll = {};
            objectPhysics->ownsAttitude = false;
            objectPhysics->conformsToTerrain = false;
            objectPhysics->orientationBasisValid = false;
        }
    }
}

} // namespace engine::object_containment_detail

namespace engine {

bool objectPassengerAllowedToFire(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity passenger, uint64_t confirmedTick) noexcept {
    return object_containment_detail::passengerAllowedToFireRecursive(
        registry, lifecycle, passenger, passenger, confirmedTick, 0u);
}

} // namespace engine
