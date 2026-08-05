#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "game/object/simulation/combat/ObjectHistoricWeaponTypes.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

struct ObjectHistoricWeaponDamageEntry final {
    LogicFixedVec3 position{};
    uint64_t confirmedTick = 0;
};

struct ObjectHistoricWeaponDamageBucket final {
    container::Vector<ObjectHistoricWeaponDamageEntry> entries;
};

// Mutable, simulation-owned counterpart to immutable WeaponTemplate data.
// Buckets are indexed by session-local WeaponContentId, matching RefCode's
// per-WeaponTemplate history shared across every firing object.
struct ObjectHistoricWeaponLedgerState final {
    container::Vector<ObjectHistoricWeaponDamageBucket> buckets;
};

void resetHistoricWeaponLedger(ecs::registry& registry) noexcept;

void processHistoricWeaponImpact(
    ecs::registry& registry, const game::WeaponTemplate& weapon,
    game::WeaponContentId weaponContent, ObjectId source,
    const LogicFixedVec3& position, uint32_t sourceSequence,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectHistoricBonusWeaponFire>& output);

} // namespace engine
