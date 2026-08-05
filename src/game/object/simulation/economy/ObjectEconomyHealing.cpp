#include "game/object/simulation/economy/ObjectEconomy.h"

#include "game/object/simulation/economy/ObjectEconomyDetail.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/simulation/lifecycle/ObjectCreate.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/runtime/ObjectHackInternetOrderAdapter.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/structure/ObjectSupplyWarehouseCrippling.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/base/GameBalanceConstants.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

using namespace engine::object_economy_detail;

namespace {

[[nodiscard]] bool isHealingOrder(const engine::ObjectOrderIntent& order,
                                  size_t instance) noexcept {
    return order.source == engine::ObjectOrderSource::System &&
           order.systemPurpose ==
               engine::ObjectOrderSystemPurpose::AutoFindHealing &&
           order.systemPurposeInstance == instance;
}

void clearHealingOrder(engine::ObjectOrderQueueComponent& queue,
                       size_t instance) {
    if (!queue.orders.empty() && isHealingOrder(queue.orders.front(), instance)) {
        queue.orders.erase(queue.orders.begin());
        ++queue.revision;
    }
}

[[nodiscard]] bool replaceHealingOrder(
    engine::ObjectOrderQueueComponent& queue,
    engine::ObjectAutoFindHealingRuntime& runtime, size_t instance,
    engine::PlayerId owner, engine::ObjectId dock,
    const engine::LogicFixedVec3& dockPosition,
    uint64_t confirmedTick) {
    if (!queue.orders.empty() && isHealingOrder(queue.orders.front(), instance)) {
        const engine::ObjectOrderIntent& current = queue.orders.front();
        if (current.targetObject == dock) return true;
        queue.orders.erase(queue.orders.begin());
    } else if (!queue.orders.empty()) {
        return false;
    }

    queue.orders.insert(queue.orders.begin(), engine::ObjectOrderIntent{
        .kind = engine::ObjectOrderKind::Move,
        .source = engine::ObjectOrderSource::System,
        .contextPlayer = owner,
        .issuedTick = confirmedTick,
        .sourceSequence = runtime.nextCommandSequence,
        .targetObject = dock,
        .targetX = dockPosition.x,
        .targetY = dockPosition.y,
        .targetZ = dockPosition.z,
        .hasTargetPosition = true,
        .systemPurpose = engine::ObjectOrderSystemPurpose::AutoFindHealing,
        .systemPurposeInstance = static_cast<uint32_t>(instance),
    });
    ++queue.revision;
    ++runtime.nextCommandSequence;
    if (runtime.nextCommandSequence == 0) ++runtime.nextCommandSequence;
    return true;
}


} // namespace

namespace engine {

void ObjectEconomySystem::updateAutoFindHealing(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry* players,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    struct Patient final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    struct Dock final {
        ObjectId id = INVALID_OBJECT_ID;
        PlayerId owner = INVALID_PLAYER_ID;
        ecs::entity entity = ecs::null;
        TransformComponent transform;
    };

    container::Vector<Dock> docks;
    const auto dockView =
        ecs::view<const ObjectIdentityComponent, const TransformComponent,
                  const ObjectKindOfComponent>(registry);
    for (const ecs::entity entity : dockView) {
        const ObjectKindOfComponent& kinds =
            dockView.template get<const ObjectKindOfComponent>(entity);
        if (!hasKind(&kinds, game::ObjectKindOf::HealPad)) continue;
        const ObjectIdentityComponent& identity =
            dockView.template get<const ObjectIdentityComponent>(entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, entity);
        if (!isAliveObject(registry, lifecycle, entity, identity.id) ||
            hasBlockingStatus(registry, entity, confirmedTick) ||
            !owner || !owner->player) {
            continue;
        }
        docks.push_back({.id = identity.id,
                         .owner = owner->player,
                         .entity = entity,
                         .transform = dockView.template get<
                             const TransformComponent>(entity)});
    }
    std::sort(docks.begin(), docks.end(),
              [](const Dock& left, const Dock& right) {
                  return left.id < right.id;
              });

    container::Vector<Patient> patients;
    const auto patientView =
        ecs::view<const ObjectIdentityComponent, ObjectEconomyComponent>(
            registry);
    for (const ecs::entity entity : patientView) {
        const ObjectIdentityComponent& identity =
            patientView.template get<const ObjectIdentityComponent>(entity);
        if (!isAliveObject(registry, lifecycle, entity, identity.id)) {
            continue;
        }
        patients.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(patients.begin(), patients.end(),
              [](const Patient& left, const Patient& right) {
                  return left.id < right.id;
              });

    for (const Patient& patient : patients) {
        ObjectEconomyComponent& component =
            ecs::get<ObjectEconomyComponent>(registry, patient.entity);
        if (!component.plan) continue;
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, patient.entity);
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, patient.entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, patient.entity);
        const ObjectKindOfComponent* patientKinds =
            ecs::try_get<ObjectKindOfComponent>(registry, patient.entity);
        if (!owner || !transform || !health ||
            health->maximumFixed <= math::q32_32{} ||
            !hasKind(patientKinds, game::ObjectKindOf::Infantry)) {
            continue;
        }
        // RefCode's module is disabled for human-controlled players. Keep a
        // captured/former AI object in this cleanup pass so an already-issued
        // autonomous healing order cannot survive the ownership transition.
        const PlayerState* controllingPlayer = players
            ? players->get(owner->player) : nullptr;
        const bool aiControlled = !players ||
            (controllingPlayer && controllingPlayer->controller ==
                PlayerControllerKind::Ai);
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, patient.entity);
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                registry, patient.entity);
        }
        if (queue->externalRevision != 0) {
            for (ObjectAutoFindHealingRuntime& runtime :
                 component.autoFindHealing) {
                if (runtime.observedExternalOrderRevision !=
                    queue->externalRevision) {
                    runtime.targetDock = INVALID_OBJECT_ID;
                    runtime.observedExternalOrderRevision =
                        queue->externalRevision;
                }
            }
        }

        const size_t count = std::min(component.plan->autoFindHealing.size(),
                                       component.autoFindHealing.size());
        const bool healingUnavailable = !aiControlled || hasBlockingStatus(
            registry, patient.entity, confirmedTick) ||
            ecs::try_get<ObjectContainedByComponent>(
                registry, patient.entity) != nullptr;
        if (healingUnavailable) {
            for (size_t index = 0; index < count; ++index) {
                clearHealingOrder(*queue, index);
                component.autoFindHealing[index].targetDock =
                    INVALID_OBJECT_ID;
            }
            continue;
        }
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectAutoFindHealingRule& rule =
                component.plan->autoFindHealing[index];
            ObjectAutoFindHealingRuntime& runtime =
                component.autoFindHealing[index];
            if (runtime.nextScanTick > confirmedTick) continue;
            runtime.nextScanTick = saturatingAdd(
                confirmedTick,
                millisecondsToTicks(rule.scanRateMilliseconds,
                                    rules.logicFramesPerSecond));

            const math::q32_32 ratio =
                health->currentFixed / health->maximumFixed;
            if (ratio > rule.neverHealRatio) {
                clearHealingOrder(*queue, index);
                runtime.targetDock = INVALID_OBJECT_ID;
                continue;
            }

            // The original controller scans only while idle.  Its own healing
            // order is the one exception here: retaining it lets a later scan
            // validate a vanished or newly-nearer pad without stealing an
            // external Player/Script/System command.
            if (!queue->orders.empty() &&
                !isHealingOrder(queue->orders.front(), index)) {
                runtime.targetDock = INVALID_OBJECT_ID;
                continue;
            }

            const math::q32_32 rangeSq = rule.scanRange * rule.scanRange;
            ObjectId best = INVALID_OBJECT_ID;
            PlayerId bestOwner = INVALID_PLAYER_ID;
            LogicFixedVec3 bestPosition{};
            const LogicFixedVec3 patientPosition =
                readAuthoritativeObjectPosition(
                    registry, patient.entity, *transform);
            math::q32_32 bestDistance =
                math::q32_32::from_raw(std::numeric_limits<int64_t>::max());
            for (const Dock& dock : docks) {
                const std::optional<ecs::entity> dockEntity =
                    lifecycle.entityFromId(dock.id);
                if (!dockEntity) continue;
                const LogicFixedVec3 dockPosition =
                    readAuthoritativeObjectPosition(
                        registry, *dockEntity, dock.transform);
                const math::q32_32 dx =
                    dockPosition.x - patientPosition.x;
                const math::q32_32 dy =
                    dockPosition.y - patientPosition.y;
                const math::q32_32 distance = dx * dx + dy * dy;
                if (distance > rangeSq) {
                    continue;
                }
                if (!best || distance < bestDistance ||
                    (distance == bestDistance && dock.id < best)) {
                    best = dock.id;
                    bestOwner = dock.owner;
                    bestPosition = dockPosition;
                    bestDistance = distance;
                }
            }
            if (!best) {
                clearHealingOrder(*queue, index);
                runtime.targetDock = INVALID_OBJECT_ID;
                continue;
            }
            const bool alliedHealPad = players
                ? players->relationship(owner->player, bestOwner) ==
                    PlayerRelationship::Allies
                : owner->player == bestOwner;
            if (!alliedHealPad) {
                // RefCode first chooses the nearest HEAL_PAD, then
                // canGetHealedAt rejects a non-allied destination instead of
                // silently falling through to a farther pad.
                clearHealingOrder(*queue, index);
                runtime.targetDock = INVALID_OBJECT_ID;
                continue;
            }
            if (replaceHealingOrder(*queue, runtime, index, owner->player,
                                    best, bestPosition, confirmedTick)) {
                runtime.targetDock = best;
            } else {
                runtime.targetDock = INVALID_OBJECT_ID;
            }
            // RefCode returns unconditionally for every busy AI before the
            // authored AlwaysHeal branch, so the value is intentionally frozen
            // for compatibility but has no production effect.
            static_cast<void>(rule.alwaysHealRatio);
        }
    }
}


} // namespace engine
