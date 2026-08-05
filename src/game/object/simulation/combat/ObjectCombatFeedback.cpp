#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/contracts/ObjectOrderClassification.h"
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

namespace engine::object_combat_detail {

[[nodiscard]] bool isOrdinaryAIAttackOrder(
    const ObjectOrderIntent* order) noexcept {
    return isCombatDirectAttackOrder(order);
}

[[nodiscard]] bool isAIAttackMoveOrder(
    const ObjectOrderIntent* order) noexcept {
    return order && order->kind == ObjectOrderKind::Move &&
           order->attackMove &&
           order->source != ObjectOrderSource::System &&
           order->systemPurpose == ObjectOrderSystemPurpose::Generic;
}

[[nodiscard]] bool isAITacticalAttackOrder(
    const ObjectOrderIntent* order) noexcept {
    return isCombatTacticalAttackOrder(order);
}

[[nodiscard]] ai::AIAsyncOrderIdentity attackOrderIdentity(
    ObjectId subject, const ObjectOrderQueueComponent& queue,
    const ObjectOrderIntent& order) noexcept {
    return {
        .subject = subject,
        .queueRevision = queue.revision,
        .externalRevision = queue.externalRevision,
        .issuedTick = order.issuedTick,
        .sourceSequence = order.sourceSequence,
        .sourceScriptId = order.sourceScriptId,
        .systemPurposeInstance = order.systemPurposeInstance,
        .source = static_cast<uint8_t>(order.source),
        .systemPurpose = static_cast<uint8_t>(order.systemPurpose),
    };
}

[[nodiscard]] bool isActiveAICombatPhase(ai::AIAttackPhase phase) noexcept {
    return phase == ai::AIAttackPhase::Aim ||
           phase == ai::AIAttackPhase::Fire;
}

[[nodiscard]] bool commandPhaseMatchesKind(
    const ai::AIAttackCommand& command) noexcept {
    switch (command.kind) {
    case ai::AIAttackCommandKind::BeginAim:
    case ai::AIAttackCommandKind::EndAim:
        return command.correlation.phase == ai::AIAttackPhase::Aim;
    case ai::AIAttackCommandKind::BeginFire:
    case ai::AIAttackCommandKind::Fire:
    case ai::AIAttackCommandKind::EndFire:
        return command.correlation.phase == ai::AIAttackPhase::Fire;
    }
    return false;
}

void clearAICombatOperation(ecs::registry& registry, ecs::entity entity,
                            ObjectWeaponComponent& weapons) {
    resetAttackLock(weapons);
    if (ecs::try_get<ObjectAICombatOperationComponent>(registry, entity)) {
        ecs::remove<ObjectAICombatOperationComponent>(registry, entity);
    }
}

void appendAIAttackFeedbackOnce(
    container::Vector<ai::AIAttackFeedback>& output,
    const ai::AIAttackFeedback& feedback) {
    const bool duplicate = std::any_of(
        output.begin(), output.end(), [&feedback](const auto& existing) {
            return existing.kind == feedback.kind &&
                   existing.confirmedTick == feedback.confirmedTick &&
                   existing.correlation == feedback.correlation;
        });
    if (!duplicate) output.push_back(feedback);
}

ScopedAIAttackSnapshot::ScopedAIAttackSnapshot(
    container::Vector<ai::AIAttackFeedback>& output,
    ai::AIAttackFeedback& feedback, bool enabled) noexcept
    : m_output(output), m_feedback(feedback), m_enabled(enabled) {}

ScopedAIAttackSnapshot::~ScopedAIAttackSnapshot() noexcept {
    if (m_enabled) appendAIAttackFeedbackOnce(m_output, m_feedback);
}

void ScopedAIAttackSnapshot::cancel() noexcept { m_enabled = false; }

void appendWeaponDamage(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                        const ObjectSpatialIndex* spatialIndex,
                        const PlayerRegistry* players, ObjectId sourceId,
                        ecs::entity sourceEntity, ObjectId primaryTarget,
                        const game::WeaponTemplate& weapon,
                        const game::WeaponBonus& bonus, uint32_t shotSequence,
                        uint64_t tick, container::Vector<ObjectDamageRequest>& outDamage) {
    const std::optional<ecs::entity> primaryEntity = lifecycle.entityFromId(primaryTarget);
    const TransformComponent* sourceTransform =
        ecs::try_get<TransformComponent>(registry, sourceEntity);
    const TransformComponent* primaryTransform = primaryEntity
        ? ecs::try_get<TransformComponent>(registry, *primaryEntity) : nullptr;
    if (!sourceTransform || (!weapon.damageDealtAtSelfPosition && !primaryTransform)) return;
    const ecs::entity impactEntity = weapon.damageDealtAtSelfPosition
        ? sourceEntity : *primaryEntity;
    const TransformComponent& resolvedImpact = weapon.damageDealtAtSelfPosition
        ? *sourceTransform : *primaryTransform;
    const LogicFixedVec3 fixedImpact = readAuthoritativeObjectPosition(
        registry, impactEntity, resolvedImpact);
    appendWeaponImpactDamage(registry, lifecycle, spatialIndex, players, {
        .filterSource = sourceId,
        .damageCredit = sourceId,
        .filterSourceEntity = sourceEntity,
        .primaryTarget = weapon.damageDealtAtSelfPosition ? INVALID_OBJECT_ID : primaryTarget,
        .impactPosition = fixedImpact,
        .weapon = &weapon,
        .bonus = bonus,
        .sourceSequence = shotSequence,
        .confirmedTick = tick,
    }, outDamage);
}

} // namespace engine::object_combat_detail
