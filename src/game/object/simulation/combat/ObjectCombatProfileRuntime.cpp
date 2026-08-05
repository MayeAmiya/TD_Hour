#include "core/container/container_types.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"

#include <cstddef>
namespace engine
{

void refreshResolvedObjectArmor(ObjectCombatProfileComponent& combat,
                                ObjectArmorComponent& armor) noexcept
{
    armor.damageMultipliersFixed.fill(math::q32_32{int32_t{1}});
    armor.selectedArmorTemplateName.clear();
    armor.selectedDamageFxName.clear();
    if (!combat.profile)
        return;

    const container::Span<const game::ArmorSetProfile> authoredSets = combat.profile->armorSets();
    const game::ArmorSetProfile* selected = combat.profile->findBestArmorSet(combat.armorConditions);
    if (!selected || authoredSets.empty())
        return;
    const size_t index = static_cast<size_t>(selected - authoredSets.data());
    if (index >= armor.sets.size())
        return;

    const ObjectArmorSetRuntime& resolved = armor.sets[index];
    armor.selectedArmorTemplateName = resolved.armorTemplateName;
    armor.selectedDamageFxName = resolved.damageFxName;
    armor.damageMultipliersFixed = resolved.damageMultipliersFixed;
}

} // namespace engine
