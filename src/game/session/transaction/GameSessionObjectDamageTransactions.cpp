#include "game/session/transaction/GameSessionObjectDamageTransactions.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/session/weapon/GameSessionGameplayTransactionDrain.h"

#include "debug/debug.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <utility>

namespace engine {

GameSessionObjectDamageTransactions::GameSessionObjectDamageTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort barrier) noexcept
    : m_content(content),
      m_world(world),
      m_presentation(presentation),
      m_barrier(barrier) {}

bool GameSessionObjectDamageTransactions::queueObjectDamage(
    ObjectDamageRequest request) {
    if (!m_content.m_active || !request.target) return false;
    if (request.confirmedTick == 0) {
        request.confirmedTick = m_presentation.m_confirmedTick;
    }
    // Do not accept a damage transaction for an entity already hidden by a
    // previous deferred death in this frame. ObjectSimulation will also
    // defend this boundary, but rejecting here gives all callers one clear
    // session-level contract.
    if (!m_world.m_objects.entityFromId(request.target)) return false;
    m_world.m_objectSimulation.queueDamage(std::move(request));
    return true;
}

void GameSessionObjectDamageTransactions::resolveQueuedObjectDamage() {
    if (!m_content.m_active || !m_barrier) return;
    if (!m_content.m_drainingGameplayWork) {
        // Establish the confirmed gameplay dispatcher before Body enters its
        // authored Die walk. The drain admits each Body request as one Damage
        // transaction and closes handler children depth-first. A completed
        // Die may publish DestroyRequested, whose authored DeleteWalk can in
        // turn destroy riders/spawn children; keep alternating the two typed
        // journals until no new DeleteWalk was admitted.
        while (true) {
            m_barrier.drainGameplayTransactions();
            m_barrier.publishObjectFxEvents();
            m_barrier.publishTechBuildingEvents();
            if (!m_barrier.consumeObjectLifecycleEvents()) break;
            const FrameCommitResult* frameResult =
                m_barrier.frameCommitResult();
            if (frameResult && frameResult->faulted()) break;
        }
        return;
    }
    if (m_content.m_gameplayDrain) {
        while (true) {
            m_content.m_gameplayDrain->closeCurrentReaction();
            if (!m_barrier.consumeObjectLifecycleEvents()) break;
            const FrameCommitResult* frameResult =
                m_barrier.frameCommitResult();
            if (frameResult && frameResult->faulted()) break;
        }
        return;
    }
    // m_drainingGameplayWork and the barrier are installed and removed by
    // one scope guard. A draining session without its owner would have no
    // legal place to append this causal suffix; never fall back to a second,
    // independently ordered Body resolver.
    TD_ASSERT(false);
}

void GameSessionObjectDamageTransactions::resolveTerrainWaterDamage() {
    if (!m_content.m_active || !m_barrier) return;
    container::Vector<game::terrain::TerrainWaterDamagePulse> pulses =
        m_content.m_terrain.takeWaterDamagePulses();
    if (pulses.empty()) return;

    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
    };

    uint32_t pulseOrdinal = 0;
    for (const game::terrain::TerrainWaterDamagePulse& pulse : pulses) {
        if (!pulse.triggerId || pulse.damageAmountRaw <= 0 ||
            pulse.currentHeightRaw <= pulse.previousHeightRaw) {
            ++pulseOrdinal;
            continue;
        }

        const game::terrain::PolygonTriggerRecord* trigger =
            m_content.m_terrain.triggerById(pulse.triggerId);
        if (!trigger || trigger->points.empty()) {
            ++pulseOrdinal;
            continue;
        }

        // TerrainLogic::setWaterHeight scans a 2D circle centered on the
        // changed water table's AABB, with a radius equal to the full AABB
        // diagonal, then asks isUnderwater() for each candidate.  Preserve
        // that broad-phase shape rather than merely testing the changed
        // polygon: nearby overlapping water areas are observably part of the
        // old query contract.
        int32_t minimumX = trigger->points.front().x;
        int32_t maximumX = minimumX;
        int32_t minimumY = trigger->points.front().y;
        int32_t maximumY = minimumY;
        for (const math::int3& point : trigger->points) {
            minimumX = std::min(minimumX, point.x);
            maximumX = std::max(maximumX, point.x);
            minimumY = std::min(minimumY, point.y);
            maximumY = std::max(maximumY, point.y);
        }
        const math::q32_32 centerX =
            math::q32_32{minimumX} + math::q32_32::from_fraction(
                static_cast<int64_t>(maximumX) - minimumX, 2);
        const math::q32_32 centerY =
            math::q32_32{minimumY} + math::q32_32::from_fraction(
                static_cast<int64_t>(maximumY) - minimumY, 2);
        const math::q32_32 width = math::q32_32::from_fraction(
            static_cast<int64_t>(maximumX) - minimumX, 1);
        const math::q32_32 height = math::q32_32::from_fraction(
            static_cast<int64_t>(maximumY) - minimumY, 1);
        const math::q32_32 radiusSquared = width * width + height * height;

        container::Vector<Candidate> candidates;
        const auto objects = ecs::view<const ObjectIdentityComponent,
                                       const TransformComponent,
                                       const ObjectLifecycleComponent>(
            m_world.m_registry);
        candidates.reserve(objects.size_hint());
        for (const ecs::entity entity : objects) {
            const auto& identity =
                objects.template get<const ObjectIdentityComponent>(entity);
            const auto& transform =
                objects.template get<const TransformComponent>(entity);
            const auto& lifecycle =
                objects.template get<const ObjectLifecycleComponent>(entity);
            if (!identity.id ||
                lifecycle.phase != ObjectLifecyclePhase::Alive) {
                continue;
            }
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                m_world.m_registry, entity, transform);
            const math::q32_32 dx = position.x - centerX;
            const math::q32_32 dy = position.y - centerY;
            if (dx * dx + dy * dy > radiusSquared) continue;
            if (!m_content.m_terrain.isUnderwaterLegacyRaw(
                    position.x.raw(), position.y.raw())) {
                continue;
            }
            candidates.push_back({.object = identity.id});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& left, const Candidate& right) {
                      return left.object < right.object;
                  });

        for (const Candidate& candidate : candidates) {
            // Water damage is ordinary damage, not ScriptActions::NAMED_KILL:
            // armor/body exceptions and the normal die event stay authoritative
            // in ObjectSimulation. INVALID source matches TerrainLogic's
            // DamageInfo; pulseOrdinal makes multiple same-tick water tables
            // deterministic before ObjectId tie-breaking.
            const bool queued = queueObjectDamage({
                .target = candidate.object,
                .source = INVALID_OBJECT_ID,
                .sourceSequence = pulseOrdinal,
                .amount = math::q32_32::from_raw(pulse.damageAmountRaw),
                .damageType = game::DamageType::WATER,
                .deathType = game::DeathType::NORMAL,
                .confirmedTick = m_presentation.m_confirmedTick,
            });
            if (!queued) continue;
            // ZH calls Object::attemptDamage inside the water iterator. Close
            // this object's Body/Die/Delete descendants before advancing to
            // the next stable ObjectId or the next water pulse; batching all
            // targets would flatten naturally nested death consequences.
            resolveQueuedObjectDamage();
            const FrameCommitResult* frameResult =
                m_barrier.frameCommitResult();
            if (frameResult && frameResult->faulted()) return;
        }
        ++pulseOrdinal;
    }
}

} // namespace engine
