#include "game/object/simulation/combat/ObjectProjectileSystemDetail.h"

#include "core/container/string_utils.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

namespace engine {

std::optional<LogicFixedVec3> ObjectProjectileSystem::resolveBridgeLayerImpact(
    ObjectProjectileComponent& projectile,
    const game::terrain::TerrainLogic& terrain,
    const LogicFixedVec3& destination) noexcept {
    return object_projectile_detail::bridgeLayerImpact(
        projectile, terrain, destination);
}

void ObjectProjectileSystem::reset() noexcept {
    m_events.clear();
    m_gameplayTransactions.clear();
    m_activeProjectileIds.clear();
    container::Vector<ObjectId>{}.swap(m_collisionCandidateScratch);
    m_activeProjectileStoreInitialized = false;
    m_projectileStreamTargets.clear();
}

void ObjectProjectileSystem::trackActiveProjectile(ObjectId object) {
    if (!object) return;
    const auto position = std::lower_bound(
        m_activeProjectileIds.begin(), m_activeProjectileIds.end(), object);
    if (position == m_activeProjectileIds.end() || *position != object) {
        m_activeProjectileIds.insert(position, object);
    }
    m_activeProjectileStoreInitialized = true;
}
uint32_t ObjectProjectileSystem::resolveProjectileStreamChain(
    const ObjectProjectileSpawnRequest& request, bool streamEnabled) {
    if (!streamEnabled || !request.launcher || !request.detonationWeapon) {
        return 0;
    }

    const ProjectileStreamOwnerKey key{
        .launcher = request.launcher,
        .weapon = request.detonationWeapon,
        .ownerGeneration = request.projectileStreamOwnerGeneration,
        .slot = request.launchSlot,
    };
    ProjectileStreamTargetState& state = m_projectileStreamTargets[key];
    const bool sameTarget = state.initialized && state.target == request.intendedTarget &&
        (request.intendedTarget ||
         (state.position.x.raw() == request.targetPosition.x.raw() &&
          state.position.y.raw() == request.targetPosition.y.raw() &&
          state.position.z.raw() == request.targetPosition.z.raw()));
    if (!sameTarget) {
        ++state.chainIdentity;
        if (state.chainIdentity == 0) ++state.chainIdentity;
        state.target = request.intendedTarget;
        state.position = request.targetPosition;
        state.initialized = true;
    }
    return state.chainIdentity;
}

} // namespace engine
