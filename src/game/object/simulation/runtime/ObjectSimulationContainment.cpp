#include "core/container/container_types.h"
#include "game/object/definition/ObjectArchetype.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ThingObjectRecipe.h"
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

namespace engine {

namespace {

using container::asciiEqualIgnoreCase;

[[nodiscard]] std::optional<game::LocomotorSetSlot>
parseRiderLocomotorSet(container::StringView value) noexcept {
    if (asciiEqualIgnoreCase(value, "SET_NORMAL"))
        return game::LocomotorSetSlot::Normal;
    if (asciiEqualIgnoreCase(value, "SET_SLUGGISH"))
        return game::LocomotorSetSlot::Sluggish;
    if (asciiEqualIgnoreCase(value, "SET_PANIC"))
        return game::LocomotorSetSlot::Panic;
    if (asciiEqualIgnoreCase(value, "SET_WANDER"))
        return game::LocomotorSetSlot::Wander;
    return std::nullopt;
}

} // namespace

bool ObjectSimulationContainmentDomain::containObject(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentAttachRequest& request) const {
    const bool accepted = object_simulation_detail::state(*this).m_containment.attach(registry, lifecycle, request);
    if (accepted && !request.enclosing && request.destroyWithContainer) {
        const std::optional<ecs::entity> host =
            lifecycle.entityFromIdIncludingPending(request.container);
        const ObjectStatusComponent* status = host
            ? ecs::try_get<ObjectStatusComponent>(registry, *host)
            : nullptr;
        if (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::Stealthed))) {
            static_cast<void>(object_simulation_detail::state(*this).m_stealth.receiveGrant(
                registry, lifecycle, request.object, true, 0,
                request.confirmedEnteredTick));
        }
    }
    return accepted;
}

bool ObjectSimulationContainmentDomain::requestContainment(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectContainmentRequest& request,
    const PlayerRegistry* players,
    const GameContentSnapshot* content) {
    const auto synchronizeRiderLocomotor = [&](ObjectId container) {
        if (!content || !container) return;
        const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(container);
        if (!entity) return;
        ObjectContainmentRuntimeComponent* runtime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                             *entity);
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(registry, *entity);
        ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, *entity);
        if (!runtime || !runtime->plan || !type || !type->archetype ||
            !locomotion) return;
        game::LocomotorSetSlot slot = game::LocomotorSetSlot::Normal;
        bool riderChange = false;
        for (const ObjectContainmentRule& rule : runtime->plan->rules) {
            if (rule.kind != ObjectContainmentKind::RiderChange) continue;
            riderChange = true;
            if (runtime->activeRiderRule < rule.riders.size()) {
                if (const auto parsed = parseRiderLocomotorSet(
                        rule.riders[runtime->activeRiderRule].locomotorSet))
                    slot = *parsed;
            }
            break;
        }
        if (!riderChange) return;
        container::Vector<game::FrozenLocomotorTemplate> selected =
            object_simulation_detail::collectRuntimeLocomotors(
                type->archetype->templateData, *content, slot);
        if (!selected.empty()) {
            locomotion->profiles = std::move(selected);
            object_simulation_detail::applyLocomotorTemplate(
                *locomotion, locomotion->profiles.front());
        }
    };
    const auto isParachutePassenger = [&](ObjectId object) {
        const std::optional<ecs::entity> passenger =
            lifecycle.entityFromIdIncludingPending(object);
        const ObjectContainedByComponent* edge = passenger
            ? ecs::try_get<ObjectContainedByComponent>(registry, *passenger)
            : nullptr;
        const std::optional<ecs::entity> host = edge
            ? lifecycle.entityFromIdIncludingPending(edge->container)
            : std::nullopt;
        const ObjectContainmentRuntimeComponent* runtime = host
            ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *host)
            : nullptr;
        return edge && runtime && runtime->plan &&
            edge->containmentRuleIndex < runtime->plan->rules.size() &&
            runtime->plan->rules[edge->containmentRuleIndex].kind ==
                ObjectContainmentKind::Parachute;
    };
    const auto hasGarrisonWeaponBonus = [&](ObjectId object) {
        const std::optional<ecs::entity> passenger =
            lifecycle.entityFromIdIncludingPending(object);
        const ObjectContainedByComponent* edge = passenger
            ? ecs::try_get<ObjectContainedByComponent>(registry, *passenger)
            : nullptr;
        const std::optional<ecs::entity> host = edge
            ? lifecycle.entityFromIdIncludingPending(edge->container)
            : std::nullopt;
        const ObjectContainmentRuntimeComponent* runtime = host
            ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *host)
            : nullptr;
        if (!edge || !runtime || !runtime->plan ||
            edge->containmentRuleIndex >= runtime->plan->rules.size())
            return false;
        const ObjectContainmentKind kind =
            runtime->plan->rules[edge->containmentRuleIndex].kind;
        return kind == ObjectContainmentKind::Garrison ||
            kind == ObjectContainmentKind::Helix;
    };
    const auto projectGarrisonWeaponBonus = [&](ObjectId object,
                                                 bool enabled) {
        const std::optional<ecs::entity> passenger =
            lifecycle.entityFromIdIncludingPending(object);
        if (!passenger) return;
        static_cast<void>(setObjectWeaponBonusCondition(
            registry, *passenger,
            game::WeaponBonusCondition::Garrisoned, enabled,
            content, nullptr, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
            request.confirmedTick));
    };
    const auto projectPortableHostStealth = [&](ObjectId object) {
        const std::optional<ecs::entity> passenger =
            lifecycle.entityFromIdIncludingPending(object);
        const ObjectContainedByComponent* edge = passenger
            ? ecs::try_get<ObjectContainedByComponent>(registry, *passenger)
            : nullptr;
        if (!edge || edge->enclosing || !edge->destroyWithContainer ||
            !edge->container) {
            return;
        }
        const std::optional<ecs::entity> host =
            lifecycle.entityFromIdIncludingPending(edge->container);
        const ObjectStatusComponent* status = host
            ? ecs::try_get<ObjectStatusComponent>(registry, *host)
            : nullptr;
        if (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::Stealthed))) {
            static_cast<void>(object_simulation_detail::state(*this).m_stealth.receiveGrant(
                registry, lifecycle, object, true, 0,
                request.confirmedTick));
        }
    };
    const auto revealRiderChangeHost = [&](ObjectId object) {
        const std::optional<ecs::entity> passenger =
            lifecycle.entityFromIdIncludingPending(object);
        const ObjectContainedByComponent* edge = passenger
            ? ecs::try_get<ObjectContainedByComponent>(registry, *passenger)
            : nullptr;
        const std::optional<ecs::entity> host = edge
            ? lifecycle.entityFromIdIncludingPending(edge->container)
            : std::nullopt;
        const ObjectContainmentRuntimeComponent* runtime = host
            ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                               *host)
            : nullptr;
        const ObjectStatusComponent* status = host
            ? ecs::try_get<ObjectStatusComponent>(registry, *host)
            : nullptr;
        if (!edge || !runtime || !runtime->plan ||
            edge->containmentRuleIndex >= runtime->plan->rules.size() ||
            runtime->plan->rules[edge->containmentRuleIndex].kind !=
                ObjectContainmentKind::RiderChange ||
            !status || !status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::Stealthed))) {
            return;
        }
        static_cast<void>(object_simulation_detail::state(*this).m_stealth.markDetected(
            registry, lifecycle, edge->container, 0, object_simulation_detail::state(*this).m_rules,
            request.confirmedTick));
    };
    const auto applyParachuteLocomotor = [&](ObjectId object,
                                             bool freefall) {
        if (!content || !object) return;
        const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(object);
        if (!entity) return;
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(registry, *entity);
        ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, *entity);
        if (!type || !type->archetype || !locomotion) return;
        container::Vector<game::FrozenLocomotorTemplate> selected =
            object_simulation_detail::collectRuntimeLocomotors(
                type->archetype->templateData, *content,
                freefall ? game::LocomotorSetSlot::Freefall
                         : game::LocomotorSetSlot::Normal);
        if (!selected.empty()) {
            locomotion->profiles = std::move(selected);
            object_simulation_detail::applyLocomotorTemplate(
                *locomotion, locomotion->profiles.front());
        }
        if (ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, *entity)) {
            physics->allowToFall = freefall;
            if (freefall) physics->sleeping = false;
        }
    };
    const auto synchronizeExperienceSink = [&](ObjectId object) {
        if (!object) return;
        ObjectId sink = INVALID_OBJECT_ID;
        const std::optional<ecs::entity> passenger =
            lifecycle.entityFromIdIncludingPending(object);
        const ObjectContainedByComponent* edge = passenger
            ? ecs::try_get<ObjectContainedByComponent>(registry, *passenger)
            : nullptr;
        if (edge && edge->container) {
            const std::optional<ecs::entity> host =
                lifecycle.entityFromIdIncludingPending(edge->container);
            const ObjectContainmentRuntimeComponent* runtime = host
                ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                                   *host)
                : nullptr;
            if (runtime && runtime->plan && edge->containmentRuleIndex <
                    runtime->plan->rules.size() &&
                runtime->plan->rules[edge->containmentRuleIndex]
                    .experienceSinkForRider) {
                sink = edge->container;
            }
        }
        static_cast<void>(object_simulation_detail::state(*this).m_experience.setSink(
            registry, lifecycle, object, sink, request.confirmedTick));
    };
    const auto synchronizeArmedRiderWeaponSet = [&](ObjectId container) {
        if (!content || !container) return;
        const std::optional<ecs::entity> host =
            lifecycle.entityFromIdIncludingPending(container);
        if (!host) return;
        const ObjectContainmentRuntimeComponent* runtime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *host);
        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(registry, *host);
        ObjectCombatProfileComponent* hostCombat =
            ecs::try_get<ObjectCombatProfileComponent>(registry, *host);
        if (!runtime || !runtime->plan || !contents || !hostCombat) return;

        bool ownsArmedRiderRule = false;
        bool armedRiderPresent = false;
        for (const ObjectContainmentRule& rule : runtime->plan->rules)
            ownsArmedRiderRule = ownsArmedRiderRule ||
                rule.armedRidersUpgradeMyWeaponSet;
        if (!ownsArmedRiderRule) return;

        for (const ObjectContainedObjectRecord& record : contents->objects) {
            const std::optional<ecs::entity> rider =
                lifecycle.entityFromId(record.object);
            const ObjectContainedByComponent* edge = rider
                ? ecs::try_get<ObjectContainedByComponent>(registry, *rider)
                : nullptr;
            if (!rider || !edge || edge->container != container ||
                edge->containmentRuleIndex >= runtime->plan->rules.size() ||
                !runtime->plan->rules[edge->containmentRuleIndex]
                     .armedRidersUpgradeMyWeaponSet) {
                continue;
            }
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, *rider);
            if (!kinds || !game::objectHasKind(
                    kinds->mask, game::ObjectKindOf::Infantry)) {
                continue;
            }
            const ObjectCombatProfileComponent* riderCombat =
                ecs::try_get<ObjectCombatProfileComponent>(registry, *rider);
            if (!riderCombat || !riderCombat->profile) continue;
            const game::WeaponSetProfile* activeSet =
                riderCombat->profile->findBestWeaponSet(
                    riderCombat->weaponConditions);
            if (!activeSet) continue;
            for (const game::WeaponSlotProfile& slot : activeSet->slots) {
                if (!slot.hasWeapon()) continue;
                const game::WeaponTemplate* weapon =
                    content->findWeapon(slot.weaponTemplateName);
                constexpr math::q32_32 kMinimumUsableGarrisonRange =
                    math::q32_32::from_fraction(25, 2);
                if (!weapon ||
                    weapon->fixed.attackRange < kMinimumUsableGarrisonRange ||
                    (weapon->fixed.primaryDamage <= math::q32_32{} &&
                     weapon->fixed.secondaryDamage <= math::q32_32{})) {
                    continue;
                }
                armedRiderPresent = true;
                break;
            }
            if (armedRiderPresent) break;
        }
        const game::WeaponSetConditionMask bit =
            game::weaponSetConditionBit(
                game::WeaponSetCondition::PlayerUpgrade);
        if (armedRiderPresent) hostCombat->weaponConditions |= bit;
        else hostCombat->weaponConditions &= ~bit;
    };
    const auto clearExperienceSinksTo = [&](ObjectId sink) {
        if (!sink) return;
        const auto view = ecs::view<const ObjectIdentityComponent,
                                    const ObjectExperienceComponent>(registry);
        container::Vector<ObjectId> affected;
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            const ObjectExperienceComponent& experience =
                view.template get<const ObjectExperienceComponent>(entity);
            if (identity.id && experience.sink == sink)
                affected.push_back(identity.id);
        }
        std::sort(affected.begin(), affected.end());
        for (const ObjectId object : affected) {
            static_cast<void>(object_simulation_detail::state(*this).m_experience.setSink(
                registry, lifecycle, object, INVALID_OBJECT_ID,
                request.confirmedTick));
        }
    };
    struct RiderExperienceTransfer final {
        ObjectId host = INVALID_OBJECT_ID;
        ObjectId enteringRider = INVALID_OBJECT_ID;
        container::Vector<ObjectId> exitingRiders;
        game::ObjectVeterancyLevel enteringLevel =
            game::ObjectVeterancyLevel::Regular;
        game::ObjectVeterancyLevel hostLevel =
            game::ObjectVeterancyLevel::Regular;
        bool enteringTrainable = false;
        bool active = false;
    };
    const auto captureRiderTransfer = [&](ObjectId hostId,
                                          ObjectId enteringRider) {
        RiderExperienceTransfer transfer{
            .host = hostId,
            .enteringRider = enteringRider,
        };
        const std::optional<ecs::entity> host =
            lifecycle.entityFromId(hostId);
        if (!host) return transfer;
        const ObjectContainmentRuntimeComponent* runtime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *host);
        if (!runtime || !runtime->plan ||
            std::none_of(runtime->plan->rules.begin(),
                         runtime->plan->rules.end(),
                         [](const ObjectContainmentRule& rule) {
                             return rule.kind ==
                                 ObjectContainmentKind::RiderChange;
                         })) {
            return transfer;
        }
        transfer.active = true;
        if (const ObjectVeterancyComponent* veterancy =
                ecs::try_get<ObjectVeterancyComponent>(registry, *host))
            transfer.hostLevel = veterancy->level;
        if (const ObjectContainmentComponent* contents =
                ecs::try_get<ObjectContainmentComponent>(registry, *host)) {
            for (const ObjectContainedObjectRecord& record :
                 contents->objects) {
                const std::optional<ecs::entity> rider =
                    lifecycle.entityFromId(record.object);
                const ObjectContainedByComponent* edge = rider
                    ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                 *rider)
                    : nullptr;
                if (edge && edge->containmentRuleIndex <
                        runtime->plan->rules.size() &&
                    runtime->plan->rules[edge->containmentRuleIndex].kind ==
                        ObjectContainmentKind::RiderChange)
                    transfer.exitingRiders.push_back(record.object);
            }
        }
        if (enteringRider) {
            const std::optional<ecs::entity> rider =
                lifecycle.entityFromId(enteringRider);
            if (rider) {
                if (const ObjectVeterancyComponent* veterancy =
                        ecs::try_get<ObjectVeterancyComponent>(registry,
                                                               *rider))
                    transfer.enteringLevel = veterancy->level;
                if (const ObjectExperienceComponent* experience =
                        ecs::try_get<ObjectExperienceComponent>(registry,
                                                                *rider))
                    transfer.enteringTrainable = experience->trainable;
            }
        }
        return transfer;
    };
    const auto finishRiderAttach = [&](const RiderExperienceTransfer& transfer) {
        if (!transfer.active || !transfer.enteringRider) return;
        for (const ObjectId oldRider : transfer.exitingRiders) {
            static_cast<void>(object_simulation_detail::state(*this).m_experience.setLevel(
                registry, lifecycle, oldRider, transfer.hostLevel,
                request.confirmedTick));
        }
        // setVeterancyLevel in RefCode replaces the bike's previous tracker
        // state even when the incoming rider has the same displayed rank.
        static_cast<void>(object_simulation_detail::state(*this).m_experience.resetPointsAndLevel(
            registry, lifecycle, transfer.host, request.confirmedTick));
        static_cast<void>(object_simulation_detail::state(*this).m_experience.setLevel(
            registry, lifecycle, transfer.host, transfer.enteringLevel,
            request.confirmedTick));
        static_cast<void>(object_simulation_detail::state(*this).m_experience.setTrainable(
            registry, lifecycle, transfer.host, transfer.enteringTrainable,
            request.confirmedTick));
        static_cast<void>(object_simulation_detail::state(*this).m_experience.resetPointsAndLevel(
            registry, lifecycle, transfer.enteringRider,
            request.confirmedTick));
    };
    const auto finishRiderExit = [&](const RiderExperienceTransfer& transfer) {
        if (!transfer.active) return;
        for (const ObjectId rider : transfer.exitingRiders) {
            static_cast<void>(object_simulation_detail::state(*this).m_experience.setLevel(
                registry, lifecycle, rider, transfer.hostLevel,
                request.confirmedTick));
        }
        static_cast<void>(object_simulation_detail::state(*this).m_experience.resetPointsAndLevel(
            registry, lifecycle, transfer.host, request.confirmedTick));
        if (const std::optional<ecs::entity> host =
                lifecycle.entityFromId(transfer.host)) {
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(registry, *host);
            if (type && type->archetype) {
                static_cast<void>(object_simulation_detail::state(*this).m_experience.setTrainable(
                    registry, lifecycle, transfer.host,
                    type->archetype->templateData.isTrainable,
                    request.confirmedTick));
            }
        }
    };
    switch (request.kind) {
    case ObjectContainmentRequestKind::Attach: {
        if (request.force) {
            const RiderExperienceTransfer riderTransfer =
                captureRiderTransfer(request.container, request.object);
            const bool accepted = object_simulation_detail::state(*this).m_containment.requestAttach(
                registry, lifecycle, request, object_simulation_detail::state(*this).m_containmentEvents, players);
            if (accepted) {
                finishRiderAttach(riderTransfer);
                synchronizeRiderLocomotor(request.container);
                synchronizeExperienceSink(request.object);
                synchronizeArmedRiderWeaponSet(request.container);
                if (isParachutePassenger(request.object))
                    applyParachuteLocomotor(request.container, true);
                if (hasGarrisonWeaponBonus(request.object))
                    projectGarrisonWeaponBonus(request.object, true);
                projectPortableHostStealth(request.object);
                revealRiderChangeHost(request.object);
            }
            return accepted;
        }
        const std::optional<ObjectContainmentAttachRequest> prepared =
            object_simulation_detail::state(*this).m_containment.prepareAttach(registry, lifecycle, request,
                                         players);
        if (!prepared) {
            return object_simulation_detail::state(*this).m_containment.requestAttach(
                registry, lifecycle, request, object_simulation_detail::state(*this).m_containmentEvents, players);
        }
        const RiderExperienceTransfer riderTransfer =
            captureRiderTransfer(prepared->container, prepared->object);
        const ObjectRailedTransportDockAdmission dockAdmission =
            object_simulation_detail::state(*this).m_bridge.beginRailedTransportDockAttach(
                registry, lifecycle,
                {.container = prepared->container,
                 .object = prepared->object,
                 .containmentRuleIndex = prepared->containmentRuleIndex,
                 .destroyWithContainer =
                     prepared->destroyWithContainer,
                  .enclosing = prepared->enclosing,
                  .followsContainerTransform =
                      prepared->followsContainerTransform,
                  .logicFramesPerSecond = object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
                  .confirmedTick = request.confirmedTick});
        if (dockAdmission ==
            ObjectRailedTransportDockAdmission::Deferred) {
            return true;
        }
        if (dockAdmission ==
            ObjectRailedTransportDockAdmission::Rejected) {
            object_simulation_detail::state(*this).m_containmentEvents.push_back({
                .kind = ObjectContainmentRequestKind::Attach,
                .container = request.container,
                .object = request.object,
                .confirmedTick = request.confirmedTick,
                .accepted = false,
            });
            return false;
        }
        const bool accepted = object_simulation_detail::state(*this).m_containment.commitPreparedAttach(
            registry, lifecycle, *prepared, request.confirmedTick,
            object_simulation_detail::state(*this).m_containmentEvents);
        if (accepted) {
            finishRiderAttach(riderTransfer);
            synchronizeRiderLocomotor(prepared->container);
            synchronizeExperienceSink(prepared->object);
            synchronizeArmedRiderWeaponSet(prepared->container);
            if (isParachutePassenger(prepared->object))
                applyParachuteLocomotor(prepared->container, true);
            if (hasGarrisonWeaponBonus(prepared->object))
                projectGarrisonWeaponBonus(prepared->object, true);
            projectPortableHostStealth(prepared->object);
            revealRiderChangeHost(prepared->object);
        }
        return accepted;
    }
    case ObjectContainmentRequestKind::Detach:
        {
            const bool wasParachute = isParachutePassenger(request.object);
            const bool hadGarrisonWeaponBonus =
                hasGarrisonWeaponBonus(request.object);
            ObjectId actualContainer = request.container;
            if (const std::optional<ecs::entity> object =
                    lifecycle.entityFromIdIncludingPending(request.object)) {
                if (const ObjectContainedByComponent* edge =
                        ecs::try_get<ObjectContainedByComponent>(registry,
                                                                  *object);
                    edge && edge->container)
                    actualContainer = edge->container;
            }
            RiderExperienceTransfer riderTransfer =
                captureRiderTransfer(actualContainer, INVALID_OBJECT_ID);
            if (riderTransfer.active) {
                riderTransfer.exitingRiders.erase(
                    std::remove_if(riderTransfer.exitingRiders.begin(),
                                   riderTransfer.exitingRiders.end(),
                                   [&](ObjectId rider) {
                                       return rider != request.object;
                                   }),
                    riderTransfer.exitingRiders.end());
            }
            const bool accepted = object_simulation_detail::state(*this).m_containment.requestDetach(
                registry, lifecycle, request, object_simulation_detail::state(*this).m_containmentEvents);
            if (accepted) {
                finishRiderExit(riderTransfer);
                synchronizeRiderLocomotor(actualContainer);
                synchronizeExperienceSink(request.object);
                synchronizeArmedRiderWeaponSet(actualContainer);
                if (wasParachute) {
                    if (const std::optional<ecs::entity> rider =
                            lifecycle.entityFromIdIncludingPending(
                                request.object)) {
                        if (ObjectPhysicsComponent* physics =
                                ecs::try_get<ObjectPhysicsComponent>(registry,
                                                                     *rider)) {
                            physics->allowToFall = false;
                            physics->inFreeFall = false;
                        }
                    }
                }
                if (hadGarrisonWeaponBonus)
                    projectGarrisonWeaponBonus(request.object, false);
            }
            return accepted;
        }
    case ObjectContainmentRequestKind::EjectAll:
        {
            const RiderExperienceTransfer riderTransfer =
                captureRiderTransfer(request.container, INVALID_OBJECT_ID);
            container::Vector<ObjectId> parachutePassengers;
            container::Vector<ObjectId> garrisonBonusPassengers;
            if (const std::optional<ecs::entity> host =
                    lifecycle.entityFromIdIncludingPending(request.container)) {
                if (const ObjectContainmentComponent* contents =
                        ecs::try_get<ObjectContainmentComponent>(registry,
                                                                  *host)) {
                    for (const ObjectContainedObjectRecord& record :
                         contents->objects) {
                        if (isParachutePassenger(record.object))
                            parachutePassengers.push_back(record.object);
                        if (hasGarrisonWeaponBonus(record.object))
                            garrisonBonusPassengers.push_back(record.object);
                    }
                }
            }
            const bool accepted = object_simulation_detail::state(*this).m_containment.requestEjectAll(
                registry, lifecycle, request, object_simulation_detail::state(*this).m_containmentEvents);
            if (accepted) {
                finishRiderExit(riderTransfer);
                synchronizeRiderLocomotor(request.container);
                clearExperienceSinksTo(request.container);
                synchronizeArmedRiderWeaponSet(request.container);
                for (const ObjectId passenger : parachutePassengers) {
                    if (const std::optional<ecs::entity> rider =
                            lifecycle.entityFromIdIncludingPending(passenger)) {
                        if (ObjectPhysicsComponent* physics =
                                ecs::try_get<ObjectPhysicsComponent>(registry,
                                                                     *rider)) {
                            physics->allowToFall = false;
                            physics->inFreeFall = false;
                        }
                    }
                }
                for (const ObjectId passenger : garrisonBonusPassengers)
                    projectGarrisonWeaponBonus(passenger, false);
            }
            return accepted;
        }
    }
    return false;
}

bool ObjectSimulationContainmentDomain::requestRailedTransportExecute(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId transport, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_bridge
        .requestRailedTransportExecute(
            registry, lifecycle, transport, confirmedTick);
}

bool ObjectSimulationContainmentDomain::ejectContainmentOnCapture(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId container, uint64_t confirmedTick,
    const PlayerRegistry* players, const GameContentSnapshot* content) {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(container);
    const ObjectContainmentRuntimeComponent* runtime = entity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *entity)
        : nullptr;
    if (!runtime || !runtime->plan || runtime->plan->rules.empty())
        return false;

    switch (runtime->plan->rules.front().kind) {
    case ObjectContainmentKind::Cave:
    case ObjectContainmentKind::Tunnel:
        return false;
    case ObjectContainmentKind::Overlord:
        // Defection redirects the remove-all transaction into the carried
        // add-on's Contain module while the structural add-on stays attached.
        for (const ObjectId addOn : object_simulation_detail::state(*this).m_containment.captureDependents(
                 registry, lifecycle, container)) {
            return ejectContainmentOnCapture(
                registry, lifecycle, addOn, confirmedTick,
                players, content);
        }
        return false;
    case ObjectContainmentKind::Helix:
        return false;
    default:
        // Use the high-level request facade so Rider experience, locomotor,
        // GARRISONED bonus and stealth-exposure projections are committed
        // together with the structural detach.
        return requestContainment(
            registry, lifecycle,
            {.kind = ObjectContainmentRequestKind::EjectAll,
             .container = container,
             .confirmedTick = confirmedTick,
             .force = true,
             .exposeStealthUnits = true},
            players, content);
    }
}

bool ObjectSimulationContainmentDomain::requestTransportBehavior(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectTransportBehaviorRequest& request) {
    container::Vector<ObjectDamageRequest> damage;
    const bool accepted = object_simulation_detail::state(*this).m_containment.requestBehavior(
        registry, lifecycle, object_simulation_detail::state(*this).m_rules, request, damage,
        object_simulation_detail::state(*this).m_containmentEvents,
        object_simulation_detail::state(*this).m_transportEvents,
        object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal);
    auto& simulation = static_cast<ObjectSimulation&>(*this);
    for (ObjectDamageRequest& item : damage) {
        simulation.queueDamage(std::move(item));
    }
    return accepted;
}

bool ObjectSimulationContainmentDomain::acknowledgeTransportPayloadDrop(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId transport, ObjectId payload, uint32_t ruleIndex,
    uint32_t attempt) {
    return object_simulation_detail::state(*this)
        .m_containment.acknowledgePayloadDrop(
            registry, lifecycle, transport, payload, ruleIndex, attempt);
}

bool ObjectSimulationContainmentDomain::beginHijackerRelease(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId hijacker, uint32_t ruleIndex, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this)
        .m_containment.beginHijackerRelease(
            registry, lifecycle, hijacker, ruleIndex, confirmedTick);
}

bool ObjectSimulationContainmentDomain::finishHijackerRelease(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId hijacker, uint32_t ruleIndex) {
    return object_simulation_detail::state(*this)
        .m_containment.finishHijackerRelease(
            registry, lifecycle, hijacker, ruleIndex);
}

bool ObjectSimulationContainmentDomain::beginBattleBusUndeath(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this)
        .m_containment.beginBattleBusUndeath(
            registry, lifecycle, battleBus, ruleIndex, confirmedTick);
}

bool ObjectSimulationContainmentDomain::finishBattleBusUndeath(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this)
        .m_containment.finishBattleBusUndeath(
            registry, lifecycle, battleBus, ruleIndex, confirmedTick);
}

bool ObjectSimulationContainmentDomain::beginBattleBusLanded(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this)
        .m_containment.beginBattleBusLanded(
            registry, lifecycle, battleBus, ruleIndex, confirmedTick);
}

bool ObjectSimulationContainmentDomain::finishBattleBusLanded(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this)
        .m_containment.finishBattleBusLanded(
            registry, lifecycle, battleBus, ruleIndex, confirmedTick);
}

bool ObjectSimulationContainmentDomain::setParachuteLandingOverride(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId parachute, const LogicFixedVec3& destination) {
    return object_simulation_detail::state(*this).m_containment.setParachuteLandingOverride(
        registry, lifecycle, parachute,
        destination.x, destination.y, destination.z);
}

bool ObjectSimulationContainmentDomain::detachContainedObject(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_containment.requestDetach(
        registry, lifecycle,
        {.kind = ObjectContainmentRequestKind::Detach,
         .object = object,
         .confirmedTick = confirmedTick,
         .force = true},
        object_simulation_detail::state(*this).m_containmentEvents);
}

} // namespace engine
