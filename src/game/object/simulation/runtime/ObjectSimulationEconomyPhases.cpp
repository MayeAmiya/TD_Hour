#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/TerrainLogic.h"
#include "core/container/string_utils.h"

#include <algorithm>
#include <utility>

namespace engine {

namespace {

[[nodiscard]] game::LocomotorSetSlot airfieldLocomotorSlot(
    container::StringView name,
    game::LocomotorSetSlot fallback) noexcept {
    using container::asciiEqualIgnoreCase;
    if (asciiEqualIgnoreCase(name, "SET_NORMAL"))
        return game::LocomotorSetSlot::Normal;
    if (asciiEqualIgnoreCase(name, "SET_NORMAL_UPGRADED"))
        return game::LocomotorSetSlot::NormalUpgraded;
    if (asciiEqualIgnoreCase(name, "SET_FREEFALL"))
        return game::LocomotorSetSlot::Freefall;
    if (asciiEqualIgnoreCase(name, "SET_WANDER"))
        return game::LocomotorSetSlot::Wander;
    if (asciiEqualIgnoreCase(name, "SET_PANIC"))
        return game::LocomotorSetSlot::Panic;
    if (asciiEqualIgnoreCase(name, "SET_TAXIING"))
        return game::LocomotorSetSlot::Taxiing;
    if (asciiEqualIgnoreCase(name, "SET_SUPERSONIC"))
        return game::LocomotorSetSlot::Supersonic;
    if (asciiEqualIgnoreCase(name, "SET_SLUGGISH"))
        return game::LocomotorSetSlot::Sluggish;
    return fallback;
}

} // namespace

void ObjectSimulation::updateAirOperationsPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    auto& damage = object_simulation_detail::state(*this).m_damageScratch;
    damage.clear();
    container::Vector<ObjectAirfieldServiceRequest> airfieldServiceRequests;
    object_simulation_detail::state(*this).m_airfield.update(registry, lifecycle, object_simulation_detail::state(*this).m_rules, confirmedTick,
                       object_simulation_detail::state(*this).m_airfieldEvents, damage,
                       object_simulation_detail::state(*this).m_slowDeathPhaseEvents,
                       object_simulation_detail::state(*this).m_deleteDestroyRequests,
                       object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
                       context.content, context.players,
                       &terrain, context.mapVisibility, context.random,
                       &object_simulation_detail::state(*this).m_systemWeaponFireCommands,
                       &airfieldServiceRequests,
                       &object_simulation_detail::state(*this).
                           m_airfieldAutomaticProductionRequests,
                       &object_simulation_detail::state(*this).m_chinookRopePresentationEvents,
                       &object_simulation_detail::state(*this).m_radiusDecalEvents);
    if (context.content) {
        const auto spectreView = ecs::view<
            const ThingTemplateComponent, ObjectAirfieldComponent,
            ObjectLocomotionComponent>(registry);
        for (const ecs::entity entity : spectreView) {
            const ThingTemplateComponent& type = spectreView.template get<
                const ThingTemplateComponent>(entity);
            ObjectAirfieldComponent& airfield = spectreView.template get<
                ObjectAirfieldComponent>(entity);
            ObjectLocomotionComponent& locomotion = spectreView.template get<
                ObjectLocomotionComponent>(entity);
            if (!type.archetype || !airfield.plan) continue;
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(registry, entity);
            const bool attacking = status && status->hasAny(
                game::objectStatusBit(
                    game::ObjectStatusFlag::IsAttacking));
            const size_t jetCount = std::min(
                airfield.jetAi.size(), airfield.plan->jetAi.size());
            for (size_t index = 0; index < jetCount; ++index) {
                ObjectJetAiRuntime& runtime = airfield.jetAi[index];
                const game::ObjectJetAiRule& rule =
                    airfield.plan->jetAi[index];
                game::LocomotorSetSlot slot =
                    game::LocomotorSetSlot::Normal;
                switch (runtime.phase) {
                case ObjectJetAirfieldPhase::Parked:
                case ObjectJetAirfieldPhase::AwaitTakeoffClearance:
                case ObjectJetAirfieldPhase::TaxiToTakeoff:
                case ObjectJetAirfieldPhase::PauseBeforeTakeoff:
                case ObjectJetAirfieldPhase::TaxiToParking:
                case ObjectJetAirfieldPhase::OrientForParking:
                case ObjectJetAirfieldPhase::Reloading:
                    slot = game::LocomotorSetSlot::Taxiing;
                    break;
                case ObjectJetAirfieldPhase::ReturningToBase:
                case ObjectJetAirfieldPhase::ReturningToDeadAirfield:
                case ObjectJetAirfieldPhase::CirclingDeadAirfield:
                    slot = airfieldLocomotorSlot(
                        rule.returnForAmmoLocomotorType,
                        game::LocomotorSetSlot::Normal);
                    break;
                default:
                    if (attacking) {
                        slot = airfieldLocomotorSlot(
                            rule.attackLocomotorType,
                            game::LocomotorSetSlot::Normal);
                    }
                    break;
                }
                const uint8_t projected = static_cast<uint8_t>(slot);
                if (runtime.locomotorProjectedSlot == projected) continue;
                container::Vector<game::FrozenLocomotorTemplate> profiles =
                    object_simulation_detail::collectRuntimeLocomotors(
                        type.archetype->templateData, *context.content,
                        slot);
                if (profiles.empty()) continue;
                locomotion.profiles = std::move(profiles);
                object_simulation_detail::applyLocomotorTemplate(
                    locomotion, locomotion.profiles.front());
                runtime.locomotorProjectedSlot = projected;
            }
            const size_t count = std::min(
                airfield.spectreGunships.size(),
                airfield.plan->spectreGunships.size());
            for (size_t index = 0; index < count; ++index) {
                ObjectSpectreGunshipRuntime& runtime =
                    airfield.spectreGunships[index];
                if (runtime.phase == ObjectSpectreGunshipPhase::Idle ||
                    runtime.locomotorProjectedPhase == runtime.phase) {
                    continue;
                }
                const game::LocomotorSetSlot slot =
                    runtime.phase == ObjectSpectreGunshipPhase::Orbiting
                    ? game::LocomotorSetSlot::Normal
                    : game::LocomotorSetSlot::Panic;
                container::Vector<game::FrozenLocomotorTemplate> profiles =
                    object_simulation_detail::collectRuntimeLocomotors(
                        type.archetype->templateData, *context.content,
                        slot);
                if (profiles.empty()) continue;
                locomotion.profiles = std::move(profiles);
                object_simulation_detail::applyLocomotorTemplate(
                    locomotion, locomotion.profiles.front());
                locomotion.ultraAccurate = true;
                runtime.locomotorProjectedPhase = runtime.phase;
            }
        }
    }
    for (const ObjectAirfieldServiceRequest& request :
         airfieldServiceRequests) {
        if (!request.aircraft) continue;
        if (request.reloadCountermeasures) {
            static_cast<void>(object_simulation_detail::state(*this).m_countermeasures.reload(
                registry, lifecycle, request.aircraft, confirmedTick));
        }
        if (!request.reloadWeapons || !context.content) continue;
        const std::optional<ecs::entity> aircraft =
            lifecycle.entityFromId(request.aircraft);
        if (!aircraft) continue;
        const uint64_t total = request.reloadCompleteTick >
                request.reloadStartedTick
            ? request.reloadCompleteTick - request.reloadStartedTick
            : 1u;
        const uint64_t elapsed = confirmedTick > request.reloadStartedTick
            ? confirmedTick - request.reloadStartedTick : 0u;
        static_cast<void>(applyObjectAirfieldReloadProgress(
            registry, *aircraft, *context.content, elapsed, total,
            confirmedTick));
    }
    for (ObjectDamageRequest& request : damage) {
        queueDamage(std::move(request));
    }
}

void ObjectSimulation::updateSupplyEconomyPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {

    // Economy timers are confirmed-frame state and visit only objects that
    // actually own AutoDepositUpdate runtime components. PlayerRegistry is
    // the sole cash writer; presentation/score work leaves as detached facts.
    object_simulation_detail::state(*this).m_autoDeposit.update(
        registry, lifecycle, context.players,
        object_simulation_detail::state(*this).m_rules, confirmedTick,
        object_simulation_detail::state(*this).m_autoDepositEvents,
        context.content ? context.content->upgradeCatalog() : nullptr);
    object_simulation_detail::state(*this).m_economy.updateAutoFindHealing(registry, lifecycle, context.players, object_simulation_detail::state(*this).m_rules,
                                    confirmedTick);
    auto& damage = object_simulation_detail::state(*this).m_damageScratch;
    damage.clear();
    if (context.players) {
        object_simulation_detail::state(*this).m_economy.updateSupplyTrucks(
            registry, lifecycle, *context.players,
            context.content,
            object_simulation_detail::state(*this).m_rules, confirmedTick,
            object_simulation_detail::state(*this).m_supplyEvents,
            damage);
        if (context.content) {
            object_simulation_detail::state(*this).m_economy.updateHackInternet(
                registry, lifecycle, *context.players, *context.content,
                object_simulation_detail::state(*this).m_rules, confirmedTick);
        }
    }
    for (ObjectDamageRequest& request : damage)
        queueDamage(std::move(request));
}

void ObjectSimulation::updateBaseRegenerationPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {

    // Base regeneration is the global, base-only recovery path. It stays
    // separate from upgrade-driven AutoHealBehavior but crosses the same
    // authoritative fixed-point Body transaction boundary.
    auto& damage = object_simulation_detail::state(*this).m_damageScratch;
    damage.clear();
    object_simulation_detail::state(*this).m_baseRegenerate.update(registry, lifecycle, object_simulation_detail::state(*this).m_rules, confirmedTick,
                            damage);
    for (ObjectDamageRequest& request : damage) queueDamage(std::move(request));
}

void ObjectSimulation::updateAutoHealPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {

    // Self-heal wakes only after this frame's already-admitted damage has
    // committed. Its requests re-enter the same authoritative Body barrier,
    // so a positive Heal never writes ObjectHealthComponent directly.
    auto& damage = object_simulation_detail::state(*this).m_damageScratch;
    damage.clear();
    object_simulation_detail::state(*this).m_autoHeal.update(
        registry, lifecycle, context.players, object_simulation_detail::state(*this).m_rules, confirmedTick,
        damage, object_simulation_detail::state(*this).m_autoHealParticleEvents);
    for (ObjectDamageRequest& request : damage) queueDamage(std::move(request));
}

} // namespace engine
