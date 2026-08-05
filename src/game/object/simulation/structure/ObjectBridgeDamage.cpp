#include "game/object/simulation/structure/ObjectBridgeDetail.h"

#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

namespace engine {

namespace {
using Fixed = math::q32_32;
} // namespace

void ObjectBridgeSystem::propagateHealthRequest(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectDamageRequest& request, math::q32_32 authoredAmount,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    if (!request.target || authoredAmount <= math::q32_32{} ||
        request.forceKill || request.emitZeroDamageFeedback) {
        return;
    }
    const std::optional<ecs::entity> targetEntity =
        lifecycle.entityFromIdIncludingPending(request.target);
    if (!targetEntity) return;
    const ObjectHealthComponent* targetHealth =
        ecs::try_get<ObjectHealthComponent>(registry, *targetEntity);
    if (!targetHealth || targetHealth->maximumFixed <= math::q32_32{}) return;

    const std::optional<ecs::entity> sourceEntity =
        lifecycle.entityFromIdIncludingPending(request.source);
    const bool sourceIsBridge = sourceEntity &&
        ecs::try_get<ObjectBridgeComponent>(registry, *sourceEntity);
    const bool sourceIsTower = sourceEntity &&
        ecs::try_get<ObjectBridgeTowerComponent>(registry, *sourceEntity);
    ObjectBridgeComponent* targetBridge =
        ecs::try_get<ObjectBridgeComponent>(registry, *targetEntity);
    ObjectBridgeTowerComponent* targetTower =
        ecs::try_get<ObjectBridgeTowerComponent>(registry, *targetEntity);
    if (!targetBridge && !targetTower) return;

    const auto nearestBridgeFor = [&](ecs::entity towerEntity) {
        const TransformComponent* towerTransform =
            ecs::try_get<TransformComponent>(registry, towerEntity);
        if (!towerTransform) return INVALID_OBJECT_ID;
        const LogicFixedVec3 towerPosition =
            readAuthoritativeObjectPosition(
                registry, towerEntity, *towerTransform);
        ObjectId best = INVALID_OBJECT_ID;
        Fixed bestDistance =
            Fixed::from_raw(std::numeric_limits<int64_t>::max());
        const auto bridges = ecs::view<ObjectIdentityComponent,
                                       ObjectBridgeComponent,
                                       TransformComponent>(registry);
        for (const ecs::entity bridgeEntity : bridges) {
            const ObjectIdentityComponent& identity =
                bridges.template get<ObjectIdentityComponent>(bridgeEntity);
            if (!identity.id ||
                !lifecycle.entityFromIdIncludingPending(identity.id)) {
                continue;
            }
            const TransformComponent& transform =
                bridges.template get<TransformComponent>(bridgeEntity);
            const LogicFixedVec3 bridgePosition =
                readAuthoritativeObjectPosition(
                    registry, bridgeEntity, transform);
            const Fixed candidate = detail::distanceSquared(
                towerPosition, bridgePosition);
            if (!best || candidate < bestDistance ||
                (candidate == bestDistance && identity.id < best)) {
                best = identity.id;
                bestDistance = candidate;
            }
        }
        return best;
    };
    if (targetTower && !targetTower->bridge) {
        targetTower->bridge = nearestBridgeFor(*targetEntity);
        if (targetTower->bridge) ++targetTower->revision;
    }

    // RefCode uses the incoming DamageInfo amount divided by this object's
    // maximum health. Armor and current-health clipping remain private to
    // each destination Body transaction.
    const math::q32_32 fraction = authoredAmount / targetHealth->maximumFixed;
    if (fraction <= math::q32_32{}) return;

    const auto stableTowersFor = [&](ObjectId bridge) {
        container::Vector<std::pair<ObjectId, ecs::entity>> result;
        const auto towers = ecs::view<ObjectIdentityComponent,
                                      ObjectBridgeTowerComponent>(registry);
        result.reserve(towers.size_hint());
        for (const ecs::entity entity : towers) {
            const ObjectIdentityComponent& identity =
                towers.template get<ObjectIdentityComponent>(entity);
            ObjectBridgeTowerComponent& tower =
                towers.template get<ObjectBridgeTowerComponent>(entity);
            if (!tower.bridge) {
                tower.bridge = nearestBridgeFor(entity);
                if (tower.bridge) ++tower.revision;
            }
            if (identity.id && tower.bridge == bridge &&
                lifecycle.entityFromIdIncludingPending(identity.id)) {
                result.emplace_back(identity.id, entity);
            }
        }
        std::sort(result.begin(), result.end(),
                  [](const auto& left, const auto& right) {
                      return left.first < right.first;
                  });
        return result;
    };
    const auto appendScaled = [&](ObjectId target, ecs::entity entity,
                                  ObjectId source, uint32_t ordinal) {
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if (!health || health->maximumFixed <= math::q32_32{}) return;
        outDamage.push_back({
            .target = target,
            .source = source,
            .sourceSequence = request.sourceSequence >
                    std::numeric_limits<uint32_t>::max() - ordinal
                ? std::numeric_limits<uint32_t>::max()
                : request.sourceSequence + ordinal,
            .amount = fraction * health->maximumFixed,
            .damageType = request.damageType,
            .damageStatusMask = request.damageStatusMask,
            .damageFxOverride = request.damageFxOverride,
            .deathType = request.deathType,
            .confirmedTick = request.confirmedTick,
        });
    };

    if (targetBridge) {
        // A request propagated from any tower must stop here or the bridge
        // would fan it back out recursively.
        if (sourceIsTower) return;
        uint32_t ordinal = 1;
        for (const auto& [towerId, towerEntity] :
             stableTowersFor(request.target)) {
            appendScaled(towerId, towerEntity, request.target, ordinal++);
        }
        return;
    }

    // Tower callbacks accept only an external source. A sibling tower or the
    // bridge itself marks an already-propagated request.
    if (sourceIsBridge || sourceIsTower || !targetTower->bridge) return;
    const ObjectId bridgeId = targetTower->bridge;
    uint32_t ordinal = 1;
    for (const auto& [towerId, towerEntity] : stableTowersFor(bridgeId)) {
        if (towerId == request.target) continue;
        appendScaled(towerId, towerEntity, request.target, ordinal++);
    }
    if (const std::optional<ecs::entity> bridgeEntity =
            lifecycle.entityFromIdIncludingPending(bridgeId)) {
        appendScaled(bridgeId, *bridgeEntity, request.target, ordinal);
    }
}

void ObjectBridgeSystem::propagateDeath(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return;
    if (const ObjectBridgeTowerComponent* tower =
            ecs::try_get<ObjectBridgeTowerComponent>(registry, *entity)) {
        if (tower->bridge) {
            outDamage.push_back({
                .target = tower->bridge,
                .source = object,
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = game::DeathType::NORMAL,
                .forceKill = true,
                .confirmedTick = confirmedTick,
            });
        }
        return;
    }
    if (!ecs::try_get<ObjectBridgeComponent>(registry, *entity)) return;
    container::Vector<ObjectId> towers;
    const auto view = ecs::view<ObjectIdentityComponent,
                                ObjectBridgeTowerComponent>(registry);
    towers.reserve(view.size_hint());
    for (const ecs::entity towerEntity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<ObjectIdentityComponent>(towerEntity);
        const ObjectBridgeTowerComponent& tower =
            view.template get<ObjectBridgeTowerComponent>(towerEntity);
        if (identity.id && tower.bridge == object && identity.id != object &&
            lifecycle.entityFromIdIncludingPending(identity.id)) {
            towers.push_back(identity.id);
        }
    }
    std::sort(towers.begin(), towers.end());
    for (uint32_t index = 0; index < towers.size(); ++index) {
        outDamage.push_back({
            .target = towers[index],
            .source = object,
            .sourceSequence = index + 1u,
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NORMAL,
            .forceKill = true,
            .confirmedTick = confirmedTick,
        });
    }
}

} // namespace engine
