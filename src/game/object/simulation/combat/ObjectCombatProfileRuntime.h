#pragma once

namespace engine
{

struct ObjectArmorComponent;
struct ObjectCombatProfileComponent;

// Re-resolves the immutable ArmorSet profile into the object's mutable armor
// cache after an ArmorSet condition changes.  Keeping this in one shared
// helper prevents Upgrade, Veterancy and Body transitions from carrying
// subtly different copies of the same selection algorithm.
void refreshResolvedObjectArmor(ObjectCombatProfileComponent& combat,
                                ObjectArmorComponent& armor) noexcept;

} // namespace engine
