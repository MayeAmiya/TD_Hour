#include "game/object/simulation/combat/ObjectHistoricWeaponLedger.h"

#include <algorithm>
#include <limits>

namespace engine {
namespace {

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t product = static_cast<uint64_t>(milliseconds) *
        std::max<uint32_t>(1, framesPerSecond);
    return std::max<uint64_t>(1, (product + 999u) / 1000u);
}

} // namespace

void resetHistoricWeaponLedger(ecs::registry& registry) noexcept {
    if (ObjectHistoricWeaponLedgerState* ledger =
            registry.ctx().find<ObjectHistoricWeaponLedgerState>()) {
        ledger->buckets.clear();
    }
}

void processHistoricWeaponImpact(
    ecs::registry& registry, const game::WeaponTemplate& weapon,
    game::WeaponContentId weaponContent, ObjectId source,
    const LogicFixedVec3& position, uint32_t sourceSequence,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectHistoricBonusWeaponFire>& output) {
    if (!weaponContent || weapon.historicBonusCount == 0 ||
        !weapon.historicBonusWeapon ||
        weapon.historicBonusWeapon == weaponContent) {
        return;
    }

    ObjectHistoricWeaponLedgerState* ledger =
        registry.ctx().find<ObjectHistoricWeaponLedgerState>();
    if (!ledger) {
        ledger = &registry.ctx().emplace<ObjectHistoricWeaponLedgerState>();
    }
    if (ledger->buckets.size() <= weaponContent.value)
        ledger->buckets.resize(static_cast<size_t>(weaponContent.value) + 1u);
    ObjectHistoricWeaponDamageBucket& bucket =
        ledger->buckets[weaponContent.value];

    const uint64_t window = millisecondsToTicks(
        weapon.historicBonusTimeMilliseconds, logicFramesPerSecond);
    bucket.entries.erase(
        std::remove_if(
            bucket.entries.begin(), bucket.entries.end(),
            [confirmedTick, window](
                const ObjectHistoricWeaponDamageEntry& entry) noexcept {
                return window == 0 || entry.confirmedTick > confirmedTick ||
                    confirmedTick - entry.confirmedTick >= window;
            }),
        bucket.entries.end());

    const uint32_t requiredPrevious = weapon.historicBonusCount - 1u;
    container::Vector<size_t> matched;
    matched.reserve(requiredPrevious);
    const math::q32_32 radius = math::q32_32::max(
        math::q32_32{}, weapon.fixed.historicBonusRadius);
    const math::q32_32 radiusSquared = radius * radius;
    for (size_t index = 0;
         index < bucket.entries.size() && matched.size() < requiredPrevious;
         ++index) {
        const ObjectHistoricWeaponDamageEntry& entry = bucket.entries[index];
        const math::q32_32 dx = position.x - entry.position.x;
        const math::q32_32 dy = position.y - entry.position.y;
        if (dx * dx + dy * dy <= radiusSquared)
            matched.push_back(index);
    }

    if (matched.size() == requiredPrevious) {
        // Remove only the entries consumed by this trigger. This is the fixed
        // RefCode branch's trigger-id behavior and avoids the retail branch's
        // documented multi-firestorm bug without discarding unrelated hits.
        for (auto iterator = matched.rbegin(); iterator != matched.rend();
             ++iterator) {
            bucket.entries.erase(bucket.entries.begin() +
                                 static_cast<ptrdiff_t>(*iterator));
        }
        output.push_back({
            .source = source,
            .content = weapon.historicBonusWeapon,
            .position = position,
            .sourceSequence = sourceSequence,
            .authoredOrder = weaponContent.value,
            .confirmedTick = confirmedTick,
        });
        return;
    }

    bucket.entries.push_back({
        .position = position,
        .confirmedTick = confirmedTick,
    });
}

} // namespace engine
