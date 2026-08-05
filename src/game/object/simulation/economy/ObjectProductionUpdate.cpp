#include "core/container/container_types.h"
#include "game/object/definition/ObjectArchetype.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/economy/ObjectProductionDetail.h"

#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/component/ObjectDirty.h"
#include "game/player/FactionTemplate.h"
#include "game/player/PlayerRegistry.h"
#include "game/command/CommandBarOverrides.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <utility>


namespace engine {

using namespace production_detail;

namespace {

[[nodiscard]] bool needsBoundAirfieldDoor(
    const ObjectProductionComponent& production,
    const ObjectProductionJob& job) noexcept {
    if (!production.exitPlan || !job.product ||
        game::objectHasKind(job.product->kindOfMask,
                           game::ObjectKindOf::ProducedAtHelipad)) {
        return false;
    }
    return production.exitPlan->kind ==
            game::ObjectProductionExitKind::AirfieldParking ||
        production.exitPlan->kind ==
            game::ObjectProductionExitKind::FlightDeck;
}

[[nodiscard]] std::optional<uint8_t> firstAvailableAirfieldDoor(
    const ObjectAirfieldComponent& airfield,
    game::ObjectProductionExitKind exitKind, size_t doorCount) noexcept {
    size_t doorIndex = 0;
    const auto inspect = [&](const auto& modules)
        -> std::optional<uint8_t> {
        for (const auto& module : modules) {
            for (const ObjectId occupant : module.spaces) {
                if (doorIndex >= doorCount ||
                    doorIndex >= kObjectProductionDoorCount) {
                    return std::nullopt;
                }
                const size_t current = doorIndex++;
                if (!occupant) return static_cast<uint8_t>(current);
            }
        }
        return std::nullopt;
    };
    if (exitKind == game::ObjectProductionExitKind::FlightDeck) {
        const bool hasSpace = std::any_of(
            airfield.flightDecks.begin(), airfield.flightDecks.end(),
            [](const ObjectAirfieldFlightDeckRuntime& runtime) {
                return std::any_of(runtime.spaces.begin(),
                                   runtime.spaces.end(),
                                   [](ObjectId occupant) {
                                       return !occupant;
                                   });
            });
        // FlightDeckBehavior::reserveDoorForExit always returns DOOR_1;
        // the deck chooses its concrete free space only when the aircraft is
        // committed by exitObjectViaDoor.
        return hasSpace && doorCount != 0
            ? std::optional<uint8_t>{0u} : std::nullopt;
    }
    return inspect(airfield.parkingPlaces);
}

} // namespace

void ObjectProductionSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, PlayerRegistry& players,
    const GameContentSnapshot& content,
    const game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    uint32_t framesPerSecond, const EnergySimulationRules& energyRules,
    container::Vector<ObjectProductionSpawnIntent>& outSpawns,
    container::Vector<ObjectProductionUpgradeCompletionIntent>& outUpgrades) const {
    const container::Vector<Candidate> candidates = orderedCandidates(registry, lifecycle);
    for (const Candidate& candidate : candidates) {
        ObjectProductionComponent& component =
            ecs::get<ObjectProductionComponent>(registry, candidate.entity);
        if (lifecycle.isPendingDestroy(candidate.id)) {
            refundAndClear(players, component);
            continue;
        }
        if (!component.plan) continue;
        // ProductionUpdate returns immediately for OBJECT_STATUS_SOLD. Sale
        // admission has already refunded and drained the queue; this guard
        // prevents malformed/restored state from advancing during descent.
        if (producerIsSold(registry, candidate.entity)) continue;
        if (isObjectDisabled(registry, candidate.entity, confirmedTick,
                             component.plan->disabledTypesToProcess)) {
            continue;
        }
        if (advanceProductionPresentation(component, confirmedTick,
                                          framesPerSecond)) {
            ++component.revision;
            markObjectDirty(
                registry, candidate.entity,
                ObjectDirtyDomain::ModelCondition);
        }
        if (component.jobs.empty()) continue;

        const OwnerComponent& owner = ecs::get<const OwnerComponent>(registry, candidate.entity);
        const PlayerState* ownerState = players.get(owner.player);
        ObjectProductionExitComponent* exitRuntime =
            ecs::try_get<ObjectProductionExitComponent>(registry,
                                                         candidate.entity);
        ObjectProductionJob& job = component.jobs.front();
        if (job.kind == ObjectProductionJobKind::Unit &&
            (!component.exitPlan || !exitRuntime || !exitRuntime->plan ||
             !job.product || job.quantityTotal == 0 || !ownerState)) {
            // A unit needs an exit and its live producer owner for faction and
            // power-sensitive build time.  Research needs neither.
            refundJob(players, job);
            component.jobs.erase(component.jobs.begin());
            ++component.revision;
            continue;
        }
        if (job.kind == ObjectProductionJobKind::PlayerUpgrade &&
            (!job.payer || !job.upgrade || job.upgradeName.empty() ||
              !players.get(job.payer))) {
            // Do not leave a global PLAYER reservation permanently stuck if
            // a malformed tool/save queue reaches this runtime boundary.
            releasePlayerUpgradeReservation(players, job);
            refundJob(players, job);
            component.jobs.erase(component.jobs.begin());
            ++component.revision;
            continue;
        }
        if (job.kind == ObjectProductionJobKind::ObjectUpgrade &&
            (!job.payer || !job.upgrade || job.upgradeName.empty() ||
             !players.get(job.payer) || owner.player != job.payer)) {
            // OBJECT work is bound to this exact producer. A malformed or
            // uncancelled ownership transition must refund and drain it rather
            // than grant local technology to a different controller.
            refundJob(players, job);
            component.jobs.erase(component.jobs.begin());
            ++component.revision;
            continue;
        }
        if (job.kind != ObjectProductionJobKind::Unit &&
            job.kind != ObjectProductionJobKind::PlayerUpgrade &&
            job.kind != ObjectProductionJobKind::ObjectUpgrade) {
            refundJob(players, job);
            component.jobs.erase(component.jobs.begin());
            ++component.revision;
            continue;
        }

        if (job.kind == ObjectProductionJobKind::Unit) {
            // Player::allowedToBuild is re-evaluated by the legacy
            // ProductionUpdate on every tick.  A script can therefore make
            // an already-paid queue entry illegal after admission.  Match
            // that transaction here: discard and refund the current entry,
            // except for an already-queued Dozer (the original deliberately
            // kept Dozers so a player could not be left permanently unable
            // to rebuild).  New Dozer requests are still rejected by the
            // admission gate above while unit construction is disabled.
            const bool structure = game::objectHasKind(
                job.product->kindOfMask, game::ObjectKindOf::Structure);
            const bool allowed = structure
                ? ownerState->constructionPolicy.baseConstructionEnabled
                : ownerState->constructionPolicy.unitConstructionEnabled;
            const bool dozer = game::objectHasKind(
                job.product->kindOfMask, game::ObjectKindOf::Dozer);
            if (!allowed && !dozer) {
                refundJob(players, job);
                component.jobs.erase(component.jobs.begin());
                ++component.revision;
                continue;
            }
        }

        if (!job.constructionComplete) {
            if (job.framesUnderConstruction == 0) job.firstConstructionTick = confirmedTick;
            if (job.framesUnderConstruction != std::numeric_limits<uint32_t>::max()) {
                ++job.framesUnderConstruction;
            }
            if (job.kind == ObjectProductionJobKind::Unit) {
                // Source recalculates calcTimeToBuild each update, so later
                // player production modifiers can affect a queued job.  The
                // staged calculator uses session-frozen faction, current
                // PlayerEnergy and live MultipleFactory terms. Scenario
                // Handicap remains a Player/session-launch owner and defaults
                // to the source's neutral 1.0 until that input is projected.
                job.lastRequiredFrames = calculateLiveUnitBuildFrames(
                    *job.product, *ownerState, registry, content,
                    framesPerSecond, energyRules, confirmedTick);
            } else {
                // UpgradeTemplate::calcTimeToBuild has no power/faction
                // adjustment in RefCode.  Freeze its fixed-frame result at
                // admission so the queue never reaches mutable content again.
                job.lastRequiredFrames = std::max<uint32_t>(1, job.lastRequiredFrames);
            }
            if (job.framesUnderConstruction >= job.lastRequiredFrames) {
                job.constructionComplete = true;
                ++component.revision;
            }
        }
        if (!job.constructionComplete) continue;

        if (job.kind == ObjectProductionJobKind::PlayerUpgrade ||
            job.kind == ObjectProductionJobKind::ObjectUpgrade) {
            outUpgrades.push_back({
                .producer = candidate.id,
                .payer = job.payer,
                .upgrade = job.upgrade,
                .type = job.kind == ObjectProductionJobKind::PlayerUpgrade
                    ? UpgradeDefinitionType::Player
                    : UpgradeDefinitionType::Object,
                .paidCost = job.paidCost,
                .sourceSequence = job.sourceSequence,
            });
            continue;
        }

        if (needsBoundAirfieldDoor(component, job) &&
            !job.exitDoorAssigned) {
            const ObjectAirfieldComponent* airfield =
                ecs::try_get<ObjectAirfieldComponent>(registry,
                                                       candidate.entity);
            const size_t doorCount = std::min<size_t>(
                component.plan->numberOfDoorAnimations,
                component.doors.size());
            const std::optional<uint8_t> door = airfield
                ? firstAvailableAirfieldDoor(
                      *airfield, component.exitPlan->kind, doorCount)
                : std::nullopt;
            if (!door) continue;
            job.exitDoorIndex = *door;
            job.exitDoorAssigned = true;
            ++component.revision;
        }

        bool presentationChanged = false;
        if (!prepareCompletedUnitExit(component, job, confirmedTick,
                                      presentationChanged)) {
            if (presentationChanged) {
                ++component.revision;
                markObjectDirty(
                    registry, candidate.entity,
                    ObjectDirtyDomain::ModelCondition);
            }
            continue;
        }
        if (presentationChanged) {
            ++component.revision;
            markObjectDirty(
                registry, candidate.entity,
                ObjectDirtyDomain::ModelCondition);
        }

        const TransformComponent& producerProjection =
            ecs::get<const TransformComponent>(registry, candidate.entity);
        const LogicFixedVec3 producerPosition = readAuthoritativeObjectPosition(
            registry, candidate.entity, producerProjection);
        const math::q32_32 producerYaw = readAuthoritativeObjectYaw(
            registry, candidate.entity, producerProjection);
        // A parking/flight-deck job binds its concrete door only after the
        // preceding aircraft has atomically claimed that parking slot.  The
        // acknowledgement clears `exitDoorAssigned` for the next item, so
        // emitting two intents from this same snapshot would copy one door
        // into both.  The second object would then be created, fail the slot
        // claim, and be destroyed just to retry next tick.  Emit one such
        // aircraft per confirmed tick; normal Queue/SpawnPoint batch output
        // remains unchanged.
        const uint32_t nextQuantity =
            job.quantityProduced < job.quantityTotal
            ? job.quantityProduced + 1u
            : job.quantityProduced;
        const uint32_t quantityEnd = needsBoundAirfieldDoor(component, job)
            ? nextQuantity
            : job.quantityTotal;
        for (uint32_t quantityIndex = job.quantityProduced;
             quantityIndex < quantityEnd; ++quantityIndex) {
            const std::optional<ObjectProductionExitReservation> reservation =
                reserveExitRuntime(registry, lifecycle, content,
                                   candidate.entity, confirmedTick,
                                   *exitRuntime);
            if (!reservation) break;
            outSpawns.push_back(makeSpawnIntent(candidate.id, owner,
                                                 producerPosition, producerYaw,
                                                 *exitRuntime,
                                                 *reservation, job,
                                                 quantityIndex, terrain,
                                                 framesPerSecond, registry,
                                                 candidate.entity));
        }
    }
}

} // namespace engine
