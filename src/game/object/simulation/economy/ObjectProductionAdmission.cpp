#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/economy/ObjectProductionDetail.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
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

bool ObjectProductionSystem::hasQueueCapacityForProduct(
    const ecs::registry& registry, ecs::entity producer,
    const ObjectProductionComponent& production,
    const game::ObjectArchetype& product) const noexcept {
    return hasAirfieldQueueCapacity(
        registry, producer, production, product);
}

ObjectProductionRequestResult ObjectProductionSystem::queueUnit(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, PlayerRegistry& players,
    const GameContentSnapshot& content,
    const game::CommandBarOverrideState& commandBarOverrides,
    ObjectId producer, PlayerId requester, container::SharedPtr<const game::ObjectArchetype> product,
    uint64_t confirmedTick, uint32_t sourceSequence, uint32_t framesPerSecond,
    const EnergySimulationRules& energyRules,
    bool ignorePrerequisites, ObjectTeamId targetTeam,
    const std::optional<ObjectProductionRoutePoint>& targetRallyPoint,
    uint32_t targetTeamRosterIndex) const {
    if (!producer) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    if (lifecycle.isPendingDestroy(producer)) {
        return rejected(ObjectProductionRejectionReason::ProducerPendingDestroy);
    }
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(producer);
    if (!entity) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
    if (!owner || owner->player != requester) {
        return rejected(ObjectProductionRejectionReason::Unauthorized);
    }
    ObjectProductionComponent* component =
        ecs::try_get<ObjectProductionComponent>(registry, *entity);
    if (!component || !component->plan) {
        return rejected(ObjectProductionRejectionReason::NotAProducer);
    }
    if (producerIsSold(registry, *entity)) {
        return rejected(ObjectProductionRejectionReason::ProducerDisabled);
    }
    if (isObjectDisabled(registry, *entity, confirmedTick,
                         component->plan->disabledTypesToProcess)) {
        return rejected(ObjectProductionRejectionReason::ProducerDisabled);
    }
    if (!component->exitPlan) {
        return rejected(ObjectProductionRejectionReason::UnsupportedExit);
    }
    if (!product) return rejected(ObjectProductionRejectionReason::ProductNotFound);
    const PlayerState* player = players.get(requester);
    if (!player) return rejected(ObjectProductionRejectionReason::Unauthorized);
    const bool productIsStructure = game::objectHasKind(
        product->kindOfMask, game::ObjectKindOf::Structure);
    if ((productIsStructure && !player->constructionPolicy.baseConstructionEnabled) ||
        (!productIsStructure && !player->constructionPolicy.unitConstructionEnabled)) {
        return rejected(ObjectProductionRejectionReason::ProductNotAvailable);
    }
    // ThingTemplate::IsTrainable belongs exclusively to ExperienceTracker.
    // RefCode freely produces non-trainable helpers, civilians and structures;
    // command-set authorization and buildability rules decide production.
    if (!producerCanBuildUnit(registry, *entity, content, commandBarOverrides,
                              players, requester, *product,
                              ignorePrerequisites)) {
        return rejected(ObjectProductionRejectionReason::ProductNotAvailable);
    }
    if (!ignorePrerequisites &&
        !playerSatisfiesObjectProductionPrerequisites(
            registry, players, content, requester, *product)) {
        return rejected(ObjectProductionRejectionReason::PrerequisitesNotMet);
    }
    if (!ignorePrerequisites &&
        !playerCanBuildMoreOfObjectType(registry, requester, *product)) {
        return rejected(
            ObjectProductionRejectionReason::MaximumSimultaneousReached);
    }
    if (component->jobs.size() >= component->plan->maxQueueEntries) {
        return rejected(ObjectProductionRejectionReason::QueueFull);
    }
    if (!hasAirfieldQueueCapacity(registry, *entity, *component,
                                  *product)) {
        return rejected(ObjectProductionRejectionReason::ParkingPlacesFull);
    }
    const int64_t paidCost = calculateUnitCost(
        *product, *player, registry, lifecycle);
    const uint32_t requiredFrames = calculateLiveUnitBuildFrames(
        *product, *player, registry, content, framesPerSecond, energyRules,
        confirmedTick);
    const uint32_t quantityTotal = quantityFor(*component->plan, *product);
    // Keep unit and PLAYER-upgrade admission equally transactional under an
    // allocation failure: all potential vector growth happens before cash is
    // withdrawn, and the later append is only a noexcept move.
    ObjectProductionJob queuedJob{
        .product = product,
        .paidCost = paidCost,
        .payer = requester,
        .sourceSequence = sourceSequence,
        .targetTeam = targetTeam,
        .targetTeamRosterIndex = targetTeamRosterIndex,
        // RefCode clears the WorkOrder's factory correlation after the first
        // product in a QuantityModifier batch. Remaining free products still
        // spawn, score and enter the player's default Team.
        .targetTeamQuantityLimit = targetTeam ? 1u : 0u,
        .targetRallyX = targetRallyPoint ? targetRallyPoint->x : math::q32_32{},
        .targetRallyY = targetRallyPoint ? targetRallyPoint->y : math::q32_32{},
        .targetRallyZ = targetRallyPoint ? targetRallyPoint->z : math::q32_32{},
        .hasTargetRallyPoint = targetRallyPoint.has_value(),
        .queuedAtTick = confirmedTick,
        .lastRequiredFrames = requiredFrames,
        .quantityTotal = quantityTotal,
    };
    try {
        component->jobs.reserve(component->jobs.size() + 1);
    } catch (const std::bad_alloc&) {
        return rejected(ObjectProductionRejectionReason::QueueAllocationFailed);
    }
    if (paidCost > 0 && !players.trySpend(requester, paidCost)) {
        return rejected(ObjectProductionRejectionReason::InsufficientFunds);
    }
    const uint32_t productionId = allocateProductionId(*component);
    if (productionId == 0) {
        if (paidCost > 0) static_cast<void>(players.adjustCash(requester, paidCost));
        return rejected(ObjectProductionRejectionReason::ProductionIdExhausted);
    }
    queuedJob.productionId = productionId;
    component->jobs.push_back(std::move(queuedJob));
    ++component->revision;
    return {.accepted = true, .rejection = ObjectProductionRejectionReason::None,
            .productionId = productionId};
}

uint32_t ObjectProductionSystem::pendingUnitCountForTeam(
    const ecs::registry& registry, ObjectTeamId team,
    container::StringView productTemplate,
    uint32_t targetTeamRosterIndex) const noexcept {
    if (!team) return 0;
    uint64_t count = 0;
    const auto view = ecs::view<const ObjectProductionComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectProductionComponent& component =
            view.template get<const ObjectProductionComponent>(entity);
        for (const ObjectProductionJob& job : component.jobs) {
            if (job.kind != ObjectProductionJobKind::Unit ||
                job.targetTeam != team || !job.product ||
                (targetTeamRosterIndex != UINT32_MAX &&
                 job.targetTeamRosterIndex != targetTeamRosterIndex) ||
                (!productTemplate.empty() &&
                 job.product->name != productTemplate)) {
                continue;
            }
            const uint32_t quota = std::min(
                job.quantityTotal, job.targetTeamQuantityLimit);
            count += quota > job.quantityProduced
                ? quota - job.quantityProduced : 0u;
            if (count >= std::numeric_limits<uint32_t>::max())
                return std::numeric_limits<uint32_t>::max();
        }
    }
    return static_cast<uint32_t>(count);
}

uint32_t ObjectProductionSystem::cancelTeamProduction(
    ecs::registry& registry, PlayerRegistry& players,
    ObjectTeamId team) const noexcept {
    if (!team) return 0;
    uint32_t cancelled = 0;
    const auto view = ecs::view<ObjectProductionComponent>(registry);
    for (const ecs::entity entity : view) {
        ObjectProductionComponent& component =
            view.template get<ObjectProductionComponent>(entity);
        const auto first = std::remove_if(
            component.jobs.begin(), component.jobs.end(),
            [&players, team, &cancelled](ObjectProductionJob& job) {
                if (job.kind != ObjectProductionJobKind::Unit ||
                    job.targetTeam != team) return false;
                refundJob(players, job);
                if (cancelled != std::numeric_limits<uint32_t>::max())
                    ++cancelled;
                return true;
            });
        if (first == component.jobs.end()) continue;
        component.jobs.erase(first, component.jobs.end());
        ++component.revision;
    }
    return cancelled;
}

uint32_t ObjectProductionSystem::detachTeamProduction(
    ecs::registry& registry, ObjectTeamId team) const noexcept {
    if (!team) return 0;
    uint32_t detached = 0;
    const auto view = ecs::view<ObjectProductionComponent>(registry);
    for (const ecs::entity entity : view) {
        ObjectProductionComponent& component =
            view.template get<ObjectProductionComponent>(entity);
        bool changed = false;
        for (ObjectProductionJob& job : component.jobs) {
            if (job.kind != ObjectProductionJobKind::Unit ||
                job.targetTeam != team) continue;
            job.targetTeam = INVALID_OBJECT_TEAM_ID;
            job.targetTeamRosterIndex = UINT32_MAX;
            job.targetTeamQuantityLimit = 0;
            job.hasTargetRallyPoint = false;
            changed = true;
            if (detached != std::numeric_limits<uint32_t>::max())
                ++detached;
        }
        if (changed) ++component.revision;
    }
    return detached;
}

ObjectProductionRequestResult ObjectProductionSystem::queuePlayerUpgrade(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, PlayerRegistry& players,
    const GameContentSnapshot& content,
    const game::CommandBarOverrideState& commandBarOverrides,
    ObjectId producer, PlayerId requester,
    const UpgradeDefinition& requestedUpgrade, uint64_t confirmedTick,
    uint32_t sourceSequence, uint32_t framesPerSecond,
    ObjectUpgradeProductionAdmission admission) const {
    if (!producer) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    if (lifecycle.isPendingDestroy(producer)) {
        return rejected(ObjectProductionRejectionReason::ProducerPendingDestroy);
    }
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(producer);
    if (!entity) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
    if (!owner || owner->player != requester) {
        return rejected(ObjectProductionRejectionReason::Unauthorized);
    }
    ObjectProductionComponent* component =
        ecs::try_get<ObjectProductionComponent>(registry, *entity);
    if (!component || !component->plan) {
        return rejected(ObjectProductionRejectionReason::NotAProducer);
    }
    if (producerIsSold(registry, *entity)) {
        return rejected(ObjectProductionRejectionReason::ProducerDisabled);
    }
    if (isObjectDisabled(registry, *entity, confirmedTick,
                         component->plan->disabledTypesToProcess)) {
        return rejected(ObjectProductionRejectionReason::ProducerDisabled);
    }

    const UpgradeDefinition* upgrade = frozenUpgrade(content, requestedUpgrade);
    if (!upgrade) return rejected(ObjectProductionRejectionReason::UpgradeNotFound);
    if (component->jobs.size() >= component->plan->maxQueueEntries) {
        return rejected(ObjectProductionRejectionReason::QueueFull);
    }
    const PlayerState* playerState = players.get(requester);
    if (!playerState) return rejected(ObjectProductionRejectionReason::Unauthorized);

    const bool playerScoped = upgrade->type == UpgradeDefinitionType::Player;
    if (playerScoped && players.hasUpgradeComplete(requester, upgrade->id)) {
        return rejected(ObjectProductionRejectionReason::UpgradeAlreadyComplete);
    }
    if (playerScoped && players.hasUpgradeInProgress(requester, upgrade->id)) {
        return rejected(ObjectProductionRejectionReason::UpgradeAlreadyInProgress);
    }
    if (!playerScoped) {
        const ObjectUpgradeSystem mux;
        if (mux.hasObjectUpgrade(registry, *entity, upgrade->id)) {
            return rejected(ObjectProductionRejectionReason::UpgradeAlreadyComplete);
        }
        const bool alreadyQueued = std::any_of(component->jobs.begin(), component->jobs.end(),
            [upgrade](const ObjectProductionJob& job) {
                return job.kind == ObjectProductionJobKind::ObjectUpgrade && job.upgrade == upgrade->id;
            });
        if (alreadyQueued) return rejected(ObjectProductionRejectionReason::UpgradeAlreadyInProgress);
        if (!mux.canReceiveObjectUpgrade(registry, *entity,
                                         playerState->upgrades.completed,
                                         upgrade->id)) {
            return rejected(ObjectProductionRejectionReason::UpgradeNotAvailable);
        }
    }
    if (!producerCanResearchUpgrade(registry, *entity, content, commandBarOverrides,
                                    players, requester, *upgrade, admission)) {
        return rejected(ObjectProductionRejectionReason::UpgradeNotAvailable);
    }

    // Allocate/copy everything which can throw before state changes. PLAYER
    // jobs reserve their cross-factory state below; OBJECT jobs deliberately
    // have only this producer-local queue entry.
    ObjectProductionJob queuedJob;
    try {
        queuedJob.kind = playerScoped ? ObjectProductionJobKind::PlayerUpgrade
                                      : ObjectProductionJobKind::ObjectUpgrade;
        queuedJob.upgrade = upgrade->id;
        queuedJob.upgradeName = upgrade->name;
        queuedJob.paidCost = upgrade->buildCost;
        queuedJob.payer = requester;
        queuedJob.sourceSequence = sourceSequence;
        queuedJob.queuedAtTick = confirmedTick;
        queuedJob.lastRequiredFrames = calculateUpgradeBuildFrames(*upgrade, framesPerSecond);
        component->jobs.reserve(component->jobs.size() + 1);
    } catch (const std::bad_alloc&) {
        return rejected(ObjectProductionRejectionReason::QueueAllocationFailed);
    }

    // Reserve cross-factory state only for PLAYER technology. An OBJECT
    // upgrade is exclusive only within this producer's own FIFO.
    if (playerScoped && !players.beginQueuedPlayerUpgrade(requester, upgrade->id)) {
        return players.hasUpgradeComplete(requester, upgrade->id)
            ? rejected(ObjectProductionRejectionReason::UpgradeAlreadyComplete)
            : rejected(ObjectProductionRejectionReason::UpgradeAlreadyInProgress);
    }
    const int64_t paidCost = queuedJob.paidCost;
    if (paidCost > 0 && !players.trySpend(requester, paidCost)) {
        if (playerScoped) {
            static_cast<void>(players.cancelQueuedPlayerUpgrade(requester, upgrade->id));
        }
        return rejected(ObjectProductionRejectionReason::InsufficientFunds);
    }
    const uint32_t productionId = allocateProductionId(*component);
    if (productionId == 0) {
        if (paidCost > 0) static_cast<void>(players.adjustCash(requester, paidCost));
        if (playerScoped) {
            static_cast<void>(players.cancelQueuedPlayerUpgrade(requester, upgrade->id));
        }
        return rejected(ObjectProductionRejectionReason::ProductionIdExhausted);
    }

    queuedJob.productionId = productionId;
    component->jobs.push_back(std::move(queuedJob));
    ++component->revision;
    return {.accepted = true, .rejection = ObjectProductionRejectionReason::None,
            .productionId = productionId};
}

ObjectProductionRequestResult ObjectProductionSystem::cancelUnit(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, PlayerRegistry& players,
    ObjectId producer, PlayerId requester, uint32_t productionId) const {
    if (!producer) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    if (lifecycle.isPendingDestroy(producer)) {
        return rejected(ObjectProductionRejectionReason::ProducerPendingDestroy);
    }
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(producer);
    if (!entity) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
    if (!owner || owner->player != requester) {
        return rejected(ObjectProductionRejectionReason::Unauthorized);
    }
    ObjectProductionComponent* component =
        ecs::try_get<ObjectProductionComponent>(registry, *entity);
    if (!component || !component->plan) {
        return rejected(ObjectProductionRejectionReason::NotAProducer);
    }
    const auto found = std::find_if(component->jobs.begin(), component->jobs.end(),
        [productionId](const ObjectProductionJob& job) {
            return job.kind == ObjectProductionJobKind::Unit && job.productionId == productionId;
        });
    if (found == component->jobs.end()) {
        return rejected(ObjectProductionRejectionReason::ProductionIdNotFound);
    }
    const uint32_t acceptedId = found->productionId;
    refundJob(players, *found);
    component->jobs.erase(found);
    ++component->revision;
    return {.accepted = true, .rejection = ObjectProductionRejectionReason::None,
            .productionId = acceptedId};
}

ObjectProductionRequestResult ObjectProductionSystem::cancelPlayerUpgrade(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, PlayerRegistry& players,
    ObjectId producer, PlayerId requester, UpgradeContentId upgrade) const {
    if (!producer) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    if (lifecycle.isPendingDestroy(producer)) {
        return rejected(ObjectProductionRejectionReason::ProducerPendingDestroy);
    }
    if (!upgrade) return rejected(ObjectProductionRejectionReason::UpgradeNotFound);
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(producer);
    if (!entity) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
    if (!owner || owner->player != requester) {
        return rejected(ObjectProductionRejectionReason::Unauthorized);
    }
    ObjectProductionComponent* component =
        ecs::try_get<ObjectProductionComponent>(registry, *entity);
    if (!component || !component->plan) {
        return rejected(ObjectProductionRejectionReason::NotAProducer);
    }
    const auto found = std::find_if(component->jobs.begin(), component->jobs.end(),
        [upgrade](const ObjectProductionJob& job) {
            return (job.kind == ObjectProductionJobKind::PlayerUpgrade ||
                    job.kind == ObjectProductionJobKind::ObjectUpgrade) &&
                   job.upgrade == upgrade;
        });
    if (found == component->jobs.end()) {
        return rejected(ObjectProductionRejectionReason::UpgradeNotInQueue);
    }

    const uint32_t acceptedId = found->productionId;
    releasePlayerUpgradeReservation(players, *found);
    refundJob(players, *found);
    component->jobs.erase(found);
    ++component->revision;
    return {.accepted = true, .rejection = ObjectProductionRejectionReason::None,
            .productionId = acceptedId};
}

bool ObjectProductionSystem::cancelAndRefundForOwnershipTransfer(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, PlayerRegistry& players,
    ObjectId producer) const {
    return cancelAndRefundAll(registry, lifecycle, players, producer);
}

bool ObjectProductionSystem::cancelAndRefundAll(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry& players, ObjectId producer) const {
    if (!producer || lifecycle.isPendingDestroy(producer)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(producer);
    if (!entity) return false;
    ObjectProductionComponent* component =
        ecs::try_get<ObjectProductionComponent>(registry, *entity);
    if (!component || component->jobs.empty()) return false;
    refundAndClear(players, *component);
    return true;
}

bool ObjectProductionSystem::onDie(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry& players, ObjectId producer,
    uint32_t authoredOrder) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(producer);
    if (!entity) return false;
    ObjectProductionComponent* component =
        ecs::try_get<ObjectProductionComponent>(registry, *entity);
    if (!component || !component->plan ||
        component->plan->authoredOrder != authoredOrder) {
        return false;
    }
    refundAndClear(players, *component);
    return true;
}

void ObjectProductionSystem::cancelPendingDestroyed(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry& players) const {
    const container::Vector<Candidate> candidates = orderedCandidates(registry, lifecycle);
    for (const Candidate& candidate : candidates) {
        if (!lifecycle.isPendingDestroy(candidate.id)) continue;
        ObjectProductionComponent& component =
            ecs::get<ObjectProductionComponent>(registry, candidate.entity);
        refundAndClear(players, component);
    }
}

ObjectProductionRequestResult ObjectProductionSystem::setRallyPoint(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId producer,
    PlayerId requester, ObjectProductionRallyPoint rallyPoint) const {
    if (!producer) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    if (lifecycle.isPendingDestroy(producer)) {
        return rejected(ObjectProductionRejectionReason::ProducerPendingDestroy);
    }
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(producer);
    if (!entity) return rejected(ObjectProductionRejectionReason::ProducerNotFound);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
    if (!owner || owner->player != requester) {
        return rejected(ObjectProductionRejectionReason::Unauthorized);
    }
    ObjectProductionExitComponent* exit =
        ecs::try_get<ObjectProductionExitComponent>(registry, *entity);
    if (!exit || !exit->plan) {
        return rejected(ObjectProductionRejectionReason::UnsupportedExit);
    }
    rallyPoint.exists = true;
    exit->rallyPoint = rallyPoint;
    ++exit->revision;
    // Keep the transitional ProductionUpdate mirror synchronized for old
    // snapshot/tool readers; ExitInterface remains the authoritative owner.
    if (ObjectProductionComponent* production =
            ecs::try_get<ObjectProductionComponent>(registry, *entity)) {
        production->rallyPoint = rallyPoint;
        ++production->revision;
    }
    return {.accepted = true, .rejection = ObjectProductionRejectionReason::None};
}

} // namespace engine
