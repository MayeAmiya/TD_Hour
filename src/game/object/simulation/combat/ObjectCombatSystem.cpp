#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include "game/object/simulation/combat/ObjectCombatDetail.h"

namespace engine {

using namespace object_combat_detail;

void ObjectCombatSystem::reset() noexcept {
    m_events.clear();
    m_aiAttackFeedback.clear();
    m_historicBonusWeaponFires.clear();
    container::Vector<ObjectId>{}.swap(m_weaponDamageVictimScratch);
}

void ObjectCombatSystem::initializeObject(ecs::registry& registry, ecs::entity entity,
                                          const game::ObjectArchetype& archetype,
                                          const GameContentSnapshot& content,
                                          uint32_t logicFramesPerSecond,
                                          uint64_t confirmedTick) const {
    const game::ThingTemplate& templateData = archetype.templateData;
    if (!ecs::try_get<ObjectWeaponBonusComponent>(registry, entity)) {
        ecs::emplace<ObjectWeaponBonusComponent>(registry, entity);
    }
    ObjectKindOfComponent kinds{.mask = archetype.kindOfMask};
    if (ObjectKindOfComponent* existing = ecs::try_get<ObjectKindOfComponent>(registry, entity)) {
        *existing = std::move(kinds);
    } else {
        ecs::emplace<ObjectKindOfComponent>(registry, entity, std::move(kinds));
    }

    ObjectPointDefenseLaserComponent pointDefense;
    if (archetype.combatInitializationPlan) {
      for (const ObjectPointDefenseLaserRulePlan& compiled :
               archetype.combatInitializationPlan->pointDefenseRules) {
        ObjectPointDefenseLaserRuleRuntime rule;
        rule.weapon = content.findWeaponId(compiled.weaponTemplate);
        rule.primaryTargetKindMask = compiled.primaryTargetKindMask;
        rule.secondaryTargetKindMask = compiled.secondaryTargetKindMask;
        rule.scanRateMilliseconds = compiled.scanRateMilliseconds;
        rule.scanRange = compiled.scanRange;
        rule.predictTargetVelocityFactor =
            compiled.predictTargetVelocityFactor;
        rule.authoredOrder = compiled.authoredOrder;
        if (rule.weapon && rule.scanRange > math::q32_32{}) {
            pointDefense.rules.push_back(std::move(rule));
        }
      }
    }
    if (!pointDefense.rules.empty()) {
        if (ObjectPointDefenseLaserComponent* existing =
                ecs::try_get<ObjectPointDefenseLaserComponent>(
                    registry, entity)) {
            *existing = std::move(pointDefense);
        } else {
            ecs::emplace<ObjectPointDefenseLaserComponent>(
                registry, entity, std::move(pointDefense));
        }
    } else {
        ecs::remove<ObjectPointDefenseLaserComponent>(registry, entity);
    }

    const ObjectCombatProfileComponent* combat =
        ecs::try_get<ObjectCombatProfileComponent>(registry, entity);
    if (!combat || !combat->profile || !combat->profile->hasAnyWeapons()) return;

    ObjectWeaponComponent runtime;
    const container::Span<const game::WeaponSetProfile> authoredSets = combat->profile->weaponSets();
    runtime.sets.reserve(authoredSets.size());
    for (const game::WeaponSetProfile& authoredSet : authoredSets) {
        ObjectWeaponSetRuntime set;
        set.conditions = authoredSet.conditions;
        set.shareWeaponReloadTime = authoredSet.shareWeaponReloadTime;
        set.weaponLockSharedAcrossSets = authoredSet.weaponLockSharedAcrossSets;
        for (size_t index = 0; index < game::kWeaponSlotCount; ++index) {
            const game::WeaponSlotProfile& authoredSlot = authoredSet.slots[index];
            ObjectWeaponSlotRuntime& slot = set.slots[index];
            slot.content = content.findWeaponId(authoredSlot.weaponTemplateName);
            if (const game::WeaponTemplate* definition = content.findWeapon(slot.content);
                definition) {
                rebuildScatterTargets(slot, *definition);
                slot.suspendFxUntilTick = saturatingTickAdd(
                    confirmedTick,
                    millisecondsToFrames(
                        definition->suspendFxDelayMilliseconds,
                        logicFramesPerSecond));
                // Legacy Weapon instances load their initial clip with no
                // delay before their first eligible attack.
                if (definition->clipSize > 0) {
                    slot.ammoInClip =
                        static_cast<uint32_t>(definition->clipSize);
                }
            }
        }
        runtime.sets.push_back(std::move(set));
    }
    initializeTurretRuntime(
        archetype.combatInitializationPlan.get(), runtime);

    if (ObjectWeaponComponent* existing = ecs::try_get<ObjectWeaponComponent>(registry, entity)) {
        *existing = std::move(runtime);
    } else {
        ecs::emplace<ObjectWeaponComponent>(registry, entity, std::move(runtime));
    }
    static_cast<void>(refreshObjectWeaponSet(
        registry, entity, content, logicFramesPerSecond, confirmedTick));
}

container::Vector<ObjectWeaponEvent> ObjectCombatSystem::takeEvents() {
    container::Vector<ObjectWeaponEvent> output = std::move(m_events);
    m_events.clear();
    return output;
}

container::Vector<ObjectHistoricBonusWeaponFire>
ObjectCombatSystem::takeHistoricBonusWeaponFires() {
    container::Vector<ObjectHistoricBonusWeaponFire> result =
        std::move(m_historicBonusWeaponFires);
    m_historicBonusWeaponFires.clear();
    return result;
}

container::Vector<ai::AIAttackFeedback>
ObjectCombatSystem::takeAIAttackFeedback() {
    container::Vector<ai::AIAttackFeedback> output =
        std::move(m_aiAttackFeedback);
    m_aiAttackFeedback.clear();
    return output;
}

} // namespace engine
