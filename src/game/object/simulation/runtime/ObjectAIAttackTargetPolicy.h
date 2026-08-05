#pragma once

namespace engine {

struct ObjectAIAttackTargetPolicyInput final {
    bool forceAttack = false;
    bool containedPassenger = false;
    bool hiddenStealth = false;
    bool sameOwner = false;
    bool allied = false;
    // KINDOF_UNATTACKABLE on the victim. RefCode
    // WeaponSet::getAbleToAttackSpecificObject rejects such a victim at the
    // very top of its VICTIM examination, before the forced-attack exception
    // is consulted, so force-fire cannot override it either.
    bool unattackable = false;
};

[[nodiscard]] constexpr bool objectAIAttackTargetPolicyAllowed(
    const ObjectAIAttackTargetPolicyInput& input) noexcept {
    if (input.unattackable) return false;
    return !input.containedPassenger && !input.hiddenStealth &&
        (input.forceAttack || (!input.sameOwner && !input.allied));
}

} // namespace engine
