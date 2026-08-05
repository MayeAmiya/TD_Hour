#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/economy/ObjectEnergy.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/player/PlayerRegistry.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace engine {
namespace {

struct Candidate final {
    ObjectId id = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

[[nodiscard]] bool producerIsActive(const ecs::registry& registry,
                                    ecs::entity entity,
                                    uint64_t confirmedTick) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    if (status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
            game::objectStatusBit(game::ObjectStatusFlag::Destroyed))) {
        return false;
    }

    // Subdual is the currently migrated normal-object Disabled edge. RefCode
    // removes only *producers* while disabled; callers leave consumers in the
    // aggregate below. EMP/other DisabledMask reasons will feed this same
    // predicate when their ECS state exists.
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return (!health || !health->effectivelyDead) &&
           !isObjectDisabled(registry, entity, confirmedTick);
}

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] int32_t saturatingNonNegative(int64_t value) noexcept {
    if (value <= 0) return 0;
    return value >= std::numeric_limits<int32_t>::max()
        ? std::numeric_limits<int32_t>::max()
        : static_cast<int32_t>(value);
}

[[nodiscard]] uint32_t countBonusSources(ObjectEnergyBonusSourceMask sources) noexcept {
    uint32_t count = 0;
    while (sources != 0) {
        count += static_cast<uint32_t>(sources & ObjectEnergyBonusSourceMask{1});
        sources = static_cast<ObjectEnergyBonusSourceMask>(sources >> 1u);
    }
    return count;
}

} // namespace

void ObjectEnergySystem::initializeObject(ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype) return;

    const game::ThingTemplate& templateData = templateComponent->archetype->templateData;
    const ObjectEnergyComponent value{
        .baseContribution = templateData.energyProduction,
        .bonusProduction = templateData.energyBonus,
    };
    if (value.baseContribution == 0 && value.bonusProduction == 0) {
        if (ecs::has<ObjectEnergyComponent>(registry, entity)) {
            ecs::remove<ObjectEnergyComponent>(registry, entity);
        }
        return;
    }

    if (ObjectEnergyComponent* existing =
            ecs::try_get<ObjectEnergyComponent>(registry, entity)) {
        *existing = value;
    } else {
        ecs::emplace<ObjectEnergyComponent>(registry, entity, value);
    }
}

void ObjectEnergySystem::update(ecs::registry& registry, PlayerRegistry& players,
                                uint64_t confirmedTick) const {
    static_cast<void>(players.clearExpiredPowerSabotage(confirmedTick));

    container::Array<int64_t, PLAYER_REGISTRY_CAPACITY> production{};
    container::Array<int64_t, PLAYER_REGISTRY_CAPACITY> consumption{};
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectLifecycleComponent,
                                const OwnerComponent,
                                const ObjectEnergyComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectLifecycleComponent& lifecycle =
            view.template get<const ObjectLifecycleComponent>(entity);
        const OwnerComponent& owner = view.template get<const OwnerComponent>(entity);
        if (!identity.id || lifecycle.phase != ObjectLifecyclePhase::Alive || !owner.player ||
            owner.player.value >= PLAYER_REGISTRY_CAPACITY || !players.get(owner.player)) {
            continue;
        }
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.id < right.id; });

    for (const Candidate& candidate : candidates) {
        const OwnerComponent& owner = ecs::get<const OwnerComponent>(registry, candidate.entity);
        const ObjectEnergyComponent& energy =
            ecs::get<const ObjectEnergyComponent>(registry, candidate.entity);
        const size_t playerIndex = owner.player.value;
        if (energy.baseContribution > 0) {
            if (producerIsActive(registry, candidate.entity, confirmedTick)) {
                production[playerIndex] += energy.baseContribution;
            }
        } else if (energy.baseContribution < 0) {
            // Disabledness never removes a consumer in RefCode, preventing
            // the old brownout feedback loop. UnderConstruction was filtered
            // by producerIsActive only for producers above; however an object
            // has not entered player influence at all until construction
            // completes, so skip every contribution in that status here.
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
            if (status && status->hasAny(
                    game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
                    game::objectStatusBit(game::ObjectStatusFlag::Destroyed))) {
                continue;
            }
            consumption[playerIndex] -= static_cast<int64_t>(energy.baseContribution);
        }
        // Energy::addPowerBonus() is a separate production mutation in the
        // original engine. Do not accidentally require a positive base value:
        // a modded zero-base/bonus-only plant must work, and a negative bonus
        // deliberately subtracts production rather than becoming consumption.
        const uint32_t activeBonusSources = countBonusSources(energy.bonusProductionSources);
        if (activeBonusSources != 0 && energy.bonusProduction != 0 &&
            producerIsActive(registry, candidate.entity, confirmedTick)) {
            production[playerIndex] += static_cast<int64_t>(energy.bonusProduction) *
                static_cast<int64_t>(activeBonusSources);
        }
    }

    for (const PlayerId player : players.activePlayerIds()) {
        const size_t index = player.value;
        if (index >= PLAYER_REGISTRY_CAPACITY) continue;
        static_cast<void>(players.setEnergyTotals(
            player, saturatingNonNegative(production[index]),
            saturatingNonNegative(consumption[index])));
    }

    // Energy::adjustPower() projects the player aggregate back onto every
    // KINDOF_POWERED object as one independent Disabled reason. Consumers
    // remain in the aggregate while disabled, avoiding a brownout feedback
    // loop; power producers in stock content are FS_POWER, not POWERED.
    container::Vector<Candidate> poweredObjects;
    const auto poweredView = ecs::view<const ObjectIdentityComponent,
                                       const ObjectLifecycleComponent,
                                       const OwnerComponent,
                                       const ObjectKindOfComponent>(registry);
    poweredObjects.reserve(poweredView.size_hint());
    for (const ecs::entity entity : poweredView) {
        const ObjectIdentityComponent& identity =
            poweredView.template get<const ObjectIdentityComponent>(entity);
        const ObjectLifecycleComponent& lifecycle =
            poweredView.template get<const ObjectLifecycleComponent>(entity);
        const ObjectKindOfComponent& kinds =
            poweredView.template get<const ObjectKindOfComponent>(entity);
        if (!identity.id || lifecycle.phase != ObjectLifecyclePhase::Alive ||
            !hasKind(&kinds, game::ObjectKindOf::Powered)) {
            continue;
        }
        poweredObjects.push_back({identity.id, entity});
    }
    std::sort(poweredObjects.begin(), poweredObjects.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.id < right.id;
              });
    for (const Candidate& candidate : poweredObjects) {
        const OwnerComponent& owner =
            ecs::get<const OwnerComponent>(registry, candidate.entity);
        const PlayerState* player = players.get(owner.player);
        const bool underpowered =
            player && !player->energy.hasSufficientPower(confirmedTick);
        static_cast<void>(ObjectDisabledSystem::setUntil(
            registry, candidate.entity, ObjectDisabledReason::Underpowered,
            underpowered ? OBJECT_DISABLED_FOREVER_TICK : 0,
            confirmedTick));
    }
}

bool ObjectEnergySystem::setBonusProductionSource(ecs::registry& registry,
                                                    ecs::entity entity,
                                                    ObjectEnergyBonusSource source,
                                                    bool active) const {
    ObjectEnergyComponent* energy = ecs::try_get<ObjectEnergyComponent>(registry, entity);
    const ObjectEnergyBonusSourceMask sourceBit = objectEnergyBonusSourceBit(source);
    if (!energy || sourceBit == 0 || energy->bonusProduction == 0) {
        return false;
    }
    const ObjectEnergyBonusSourceMask next = active
        ? static_cast<ObjectEnergyBonusSourceMask>(energy->bonusProductionSources | sourceBit)
        : static_cast<ObjectEnergyBonusSourceMask>(energy->bonusProductionSources & ~sourceBit);
    if (next == energy->bonusProductionSources) return false;
    energy->bonusProductionSources = next;
    return true;
}

} // namespace engine
