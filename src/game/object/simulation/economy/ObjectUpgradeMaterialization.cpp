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

#include "game/object/definition/ObjectArchetype.h"
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

using container::asciiEqualIgnoreCase;
using object_upgrade_detail::activateAllEligibleMuxes;
using object_upgrade_detail::processUpgradeRemovals;

container::StringView effectiveObjectCommandSetName(
    const ecs::registry& registry, ecs::entity entity) noexcept
{
    if (const ObjectCommandSetOverrideComponent* overrideState =
            ecs::try_get<ObjectCommandSetOverrideComponent>(registry, entity);
        overrideState && !overrideState->name.empty())
    {
        return overrideState->name;
    }
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    return templateComponent && templateComponent->archetype
               ? container::StringView{templateComponent->archetype->templateData.commandSet}
               : container::StringView{};
}

void ObjectUpgradeSystem::materializeObject(ecs::registry& registry,
                                            ecs::entity entity) const
{
    const ThingTemplateComponent* templateComponent = ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype)
        return;
    const container::SharedPtr<const game::ObjectUpgradePlan>& plan = templateComponent->archetype->objectUpgradePlan;

    if (plan)
    {
        size_t commandSetCapacity = 0;
        bool hasCommandSetRule = false;
        for (const game::ObjectUpgradeRule& rule : plan->rules)
        {
            if (rule.operation != game::ObjectUpgradeOperation::CommandSet)
                continue;
            hasCommandSetRule = true;
            commandSetCapacity = std::max(
                commandSetCapacity, std::max(rule.commandSet.size(), rule.commandSetAlt.size()));
        }
        if (hasCommandSetRule)
        {
            ObjectCommandSetOverrideComponent commandSetOverride;
            // Reserve at structural assembly so the later confirmed upgrade
            // transaction only copies into already-owned storage.
            commandSetOverride.name.reserve(commandSetCapacity);
            if (ObjectCommandSetOverrideComponent* existing =
                    ecs::try_get<ObjectCommandSetOverrideComponent>(registry, entity))
            {
                *existing = std::move(commandSetOverride);
            }
            else
            {
                ecs::emplace<ObjectCommandSetOverrideComponent>(
                    registry, entity, std::move(commandSetOverride));
            }
        }
        ObjectSubObjectVisibilityOverrideComponent subObjects;
        for (const game::ObjectUpgradeRule& rule : plan->rules)
        {
            if (rule.operation != game::ObjectUpgradeOperation::SubObjects)
                continue;
            const auto addName = [&subObjects](const container::String& name)
            {
                if (name.empty()) return;
                const auto found = std::find_if(
                    subObjects.entries.begin(), subObjects.entries.end(),
                    [&name](const ObjectSubObjectVisibilityOverride& entry)
                    {
                        return asciiEqualIgnoreCase(entry.name, name);
                    });
                if (found == subObjects.entries.end())
                {
                    subObjects.entries.push_back({.name = name});
                }
            };
            for (const container::String& name : rule.showSubObjects) addName(name);
            for (const container::String& name : rule.hideSubObjects) addName(name);
        }
        if (!subObjects.entries.empty())
        {
            if (ObjectSubObjectVisibilityOverrideComponent* existing =
                    ecs::try_get<ObjectSubObjectVisibilityOverrideComponent>(registry, entity))
            {
                *existing = std::move(subObjects);
            }
            else
            {
                ecs::emplace<ObjectSubObjectVisibilityOverrideComponent>(
                    registry, entity, std::move(subObjects));
            }
        }
        if (plan->powerPlant)
        {
            ObjectPowerPlantComponent powerPlant{
                .rodsExtendMilliseconds = plan->powerPlant->rodsExtendMilliseconds,
            };
            if (ObjectPowerPlantComponent* existing = ecs::try_get<ObjectPowerPlantComponent>(registry, entity))
            {
                *existing = powerPlant;
            }
            else
            {
                ecs::emplace<ObjectPowerPlantComponent>(registry, entity, powerPlant);
            }
        }
        if (plan->radarUpdate)
        {
            ObjectRadarUpdateComponent radar{
                .plan = plan->radarUpdate,
            };
            if (ObjectRadarUpdateComponent* existing =
                    ecs::try_get<ObjectRadarUpdateComponent>(registry,
                                                             entity))
            {
                *existing = std::move(radar);
            }
            else
            {
                ecs::emplace<ObjectRadarUpdateComponent>(
                    registry, entity, std::move(radar));
            }
        }

        if (!plan->rules.empty())
        {
            ObjectUpgradeComponent component{
                .plan = plan,
            };
            component.instances.resize(plan->rules.size());
            if (ObjectUpgradeComponent* existing = ecs::try_get<ObjectUpgradeComponent>(registry, entity))
            {
                *existing = std::move(component);
            }
            else
            {
                ecs::emplace<ObjectUpgradeComponent>(registry, entity, std::move(component));
            }
        }
    }

    // StartsActive is the legacy constructor's giveSelfUpgrade(), not merely
    // a pre-set boolean. Constructors run in authored module order and each
    // one performs RemovesUpgrades before marking itself executed. Recreate
    // that ordering after every sparse mux component has been assembled but
    // before Create callbacks can grant additional object upgrades.
    enum class StartsActiveConsumer : uint8_t {
        AutoHeal,
        FireWeaponWhenDamaged,
        FxListDie,
        FireWeaponWhenDead,
    };
    struct StartsActiveCandidate final {
        StartsActiveConsumer consumer =
            StartsActiveConsumer::FireWeaponWhenDamaged;
        size_t index = 0;
        uint32_t authoredOrder = 0;
    };
    container::Vector<StartsActiveCandidate> startsActive;
    ObjectAutoHealComponent* autoHeal =
        ecs::try_get<ObjectAutoHealComponent>(registry, entity);
    if (autoHeal && autoHeal->plan) {
        const size_t count = std::min(autoHeal->plan->rules.size(),
                                      autoHeal->instances.size());
        for (size_t index = 0; index < count; ++index) {
            if (!autoHeal->plan->rules[index].startsActive) continue;
            startsActive.push_back({
                .consumer = StartsActiveConsumer::AutoHeal,
                .index = index,
                .authoredOrder =
                    autoHeal->plan->rules[index].authoredOrder,
            });
        }
    }
    ObjectFireWeaponWhenDamagedComponent* fireDamaged =
        ecs::try_get<ObjectFireWeaponWhenDamagedComponent>(registry, entity);
    if (fireDamaged && fireDamaged->plan) {
        const size_t count = std::min(fireDamaged->plan->rules.size(),
                                      fireDamaged->instances.size());
        for (size_t index = 0; index < count; ++index) {
            if (!fireDamaged->plan->rules[index].startsActive) continue;
            startsActive.push_back({
                .consumer = StartsActiveConsumer::FireWeaponWhenDamaged,
                .index = index,
                .authoredOrder =
                    fireDamaged->plan->rules[index].authoredOrder,
            });
        }
    }
    const ObjectDeathReactionComponent* reactions =
        ecs::try_get<ObjectDeathReactionComponent>(registry, entity);
    ObjectFireWeaponWhenDeadRuntimeComponent* fireDead =
        ecs::try_get<ObjectFireWeaponWhenDeadRuntimeComponent>(registry,
                                                               entity);
    ObjectFxListDieRuntimeComponent* fxListDie =
        ecs::try_get<ObjectFxListDieRuntimeComponent>(registry, entity);
    if (reactions && reactions->plan && fxListDie) {
        const size_t count = std::min(reactions->plan->rules.size(),
                                      fxListDie->rules.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectDeathReactionRule& rule =
                reactions->plan->rules[index];
            if (rule.kind != game::ObjectDeathReactionKind::FxList ||
                !rule.fxListDie || !rule.fxListDie->startsActive) continue;
            startsActive.push_back({
                .consumer = StartsActiveConsumer::FxListDie,
                .index = index,
                .authoredOrder = rule.authoredOrder,
            });
        }
    }
    if (reactions && reactions->plan && fireDead) {
        const size_t count = std::min(reactions->plan->rules.size(),
                                      fireDead->rules.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectDeathReactionRule& rule =
                reactions->plan->rules[index];
            if (rule.kind !=
                    game::ObjectDeathReactionKind::FireWeaponWhenDead ||
                !rule.fireWeaponWhenDead ||
                !rule.fireWeaponWhenDead->startsActive) continue;
            startsActive.push_back({
                .consumer = StartsActiveConsumer::FireWeaponWhenDead,
                .index = index,
                .authoredOrder = rule.authoredOrder,
            });
        }
    }
    std::stable_sort(startsActive.begin(), startsActive.end(),
        [](const StartsActiveCandidate& left,
           const StartsActiveCandidate& right) {
            if (left.authoredOrder != right.authoredOrder) {
                return left.authoredOrder < right.authoredOrder;
            }
            return static_cast<uint8_t>(left.consumer) <
                   static_cast<uint8_t>(right.consumer);
        });
    for (const StartsActiveCandidate& candidate : startsActive) {
        switch (candidate.consumer) {
        case StartsActiveConsumer::AutoHeal: {
            const auto& parameters =
                autoHeal->plan->rules[candidate.index];
            processUpgradeRemovals(registry, entity,
                                   parameters.removesUpgradesMask);
            autoHeal->instances[candidate.index].upgradeActivated = true;
            break;
        }
        case StartsActiveConsumer::FireWeaponWhenDamaged: {
            const auto& parameters =
                fireDamaged->plan->rules[candidate.index];
            processUpgradeRemovals(registry, entity,
                                   parameters.upgradeMux.removesUpgradesMask);
            fireDamaged->instances[candidate.index].upgradeActivated = true;
            break;
        }
        case StartsActiveConsumer::FxListDie: {
            const auto& parameters = *reactions->plan->rules[
                candidate.index].fxListDie;
            processUpgradeRemovals(registry, entity,
                                   parameters.removesUpgradesMask);
            fxListDie->rules[candidate.index].activated = true;
            break;
        }
        case StartsActiveConsumer::FireWeaponWhenDead: {
            const auto& parameters = *reactions->plan->rules[
                candidate.index].fireWeaponWhenDead;
            processUpgradeRemovals(registry, entity,
                                   parameters.upgradeMux.removesUpgradesMask);
            fireDead->rules[candidate.index].upgradeActivated = true;
            break;
        }
        }
    }
}

void ObjectUpgradeSystem::activateEligible(ecs::registry& registry,
                                           ecs::entity entity,
                                           const UpgradeMask& ownerCompletedUpgrades,
                                           const ObjectSimulationRules& rules,
                                           uint64_t confirmedTick,
                                           ObjectUpgradeExecutionContext context)
{
    activateAllEligibleMuxes(registry, entity, ownerCompletedUpgrades, rules, confirmedTick, context);
}

} // namespace engine
