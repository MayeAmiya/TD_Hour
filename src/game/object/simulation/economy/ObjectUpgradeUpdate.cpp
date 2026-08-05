#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/simulation/status/ObjectAutoHeal.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/combat/ObjectFireWeaponBehavior.h"
#include "game/object/simulation/combat/ObjectFireUpdates.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/terrain/TerrainLogic.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/simulation/economy/ObjectUpgradeDetail.h"

namespace engine
{

using object_upgrade_detail::beginPowerPlantExtension;
using object_upgrade_detail::completePowerPlantExtension;
using object_upgrade_detail::completeRadarExtension;
using object_upgrade_detail::retractPowerPlant;

void ObjectUpgradeSystem::update(ecs::registry& registry,
                                 ObjectLifecycle& lifecycle,
                                 const ObjectSimulationRules& rules,
                                 uint64_t confirmedTick) const
{
    struct Candidate final
    {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent, ObjectPowerPlantComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view)
    {
        const ObjectIdentityComponent& identity = view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) || lifecycle.isPendingDestroy(identity.id))
        {
            continue;
        }
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(),
              candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.id < right.id; });

    for (const Candidate& candidate : candidates)
    {
        ObjectPowerPlantComponent& powerPlant = ecs::get<ObjectPowerPlantComponent>(registry, candidate.entity);
        if (powerPlant.extensionSources == 0)
        {
            if (powerPlant.state != ObjectPowerPlantRodState::Retracted)
            {
                retractPowerPlant(registry, candidate.entity, powerPlant,
                                  confirmedTick);
            }
            continue;
        }
        if (powerPlant.state == ObjectPowerPlantRodState::Retracted)
        {
            beginPowerPlantExtension(registry, candidate.entity, powerPlant, rules, confirmedTick);
        }
        else if (powerPlant.state == ObjectPowerPlantRodState::Extending &&
                 confirmedTick >= powerPlant.extensionCompleteTick)
        {
            completePowerPlantExtension(registry, candidate.entity, powerPlant,
                                        confirmedTick);
        }
    }

    candidates.clear();
    const auto radarView = ecs::view<const ObjectIdentityComponent,
                                     ObjectRadarUpdateComponent>(registry);
    candidates.reserve(radarView.size_hint());
    for (const ecs::entity entity : radarView) {
        const ObjectIdentityComponent& identity = radarView.template get<
            const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.id < right.id;
              });
    for (const Candidate& candidate : candidates) {
        ObjectRadarUpdateComponent& radar =
            ecs::get<ObjectRadarUpdateComponent>(registry,
                                                  candidate.entity);
        // RefCode tests frame > extendDoneFrame, not >=. Radar capability is
        // active immediately; this clock owns only the model transition.
        if (radar.active && !radar.extensionComplete &&
            confirmedTick > radar.extensionCompleteTick) {
            completeRadarExtension(registry, candidate.entity, radar,
                                   confirmedTick);
        }
    }
}

void ObjectUpgradeSystem::updateRadarProviders(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry& players, uint64_t confirmedTick) const {
    struct Totals final {
        uint32_t providers = 0;
        uint32_t disableProof = 0;
    };
    container::Array<Totals, PLAYER_REGISTRY_CAPACITY> totals{};
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                const ObjectRadarProviderComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity = view.template get<
            const ObjectIdentityComponent>(entity);
        const OwnerComponent& owner = view.template get<
            const OwnerComponent>(entity);
        const ObjectRadarProviderComponent& provider = view.template get<
            const ObjectRadarProviderComponent>(entity);
        if (!identity.id || !owner.player ||
            owner.player.value >= PLAYER_REGISTRY_CAPACITY ||
            !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id) ||
            isObjectDisabled(registry, entity, confirmedTick)) {
            continue;
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if (health && health->effectivelyDead) continue;
        Totals& value = totals[owner.player.value];
        value.providers = provider.providerCount >
                std::numeric_limits<uint32_t>::max() - value.providers
            ? std::numeric_limits<uint32_t>::max()
            : value.providers + provider.providerCount;
        value.disableProof = provider.disableProofProviderCount >
                std::numeric_limits<uint32_t>::max() - value.disableProof
            ? std::numeric_limits<uint32_t>::max()
            : value.disableProof + provider.disableProofProviderCount;
    }
    for (const PlayerId player : players.activePlayerIds()) {
        if (!player || player.value >= PLAYER_REGISTRY_CAPACITY) continue;
        const Totals& value = totals[player.value];
        static_cast<void>(players.setRadarProviderTotals(
            player, value.providers, value.disableProof));
    }
}

} // namespace engine
