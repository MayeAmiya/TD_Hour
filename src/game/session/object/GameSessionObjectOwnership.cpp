#include "game/session/core/GameSession.h"
#include "game/session/object/GameSessionObjectLifecycleDetail.h"
#include "game/session/transaction/GameSessionObjectOwnershipTransactions.h"
#include "game/session/state/GameSessionDomainState.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectDeathEvents.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/runtime/ObjectStatus.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>

namespace engine {
using namespace object_lifecycle_detail;

bool GameSessionObjectOwnershipTransactions::applyDefection(
    const ObjectDefectionRequest& request) {
    container::Vector<ObjectId> visited;
    return applyDefectionRecursive(request, visited);
}

bool GameSessionObjectOwnershipTransactions::applyPilotVehicleTakeover(
    const ObjectPilotVehicleTakeoverRequest& request) {
    if (!request.pilot || !request.vehicle || !request.newOwner ||
        request.confirmedTick !=
            m_presentation.m_confirmedTick ||
        request.pilot == request.vehicle ||
        m_world.m_objects.isPendingDestroy(request.pilot) ||
        m_world.m_objects.isPendingDestroy(
            request.vehicle)) {
        return false;
    }
    const std::optional<ecs::entity> pilotEntity =
        m_world.m_objects.entityFromId(request.pilot);
    const std::optional<ecs::entity> vehicleEntity =
        m_world.m_objects.entityFromId(request.vehicle);
    if (!pilotEntity || !vehicleEntity) return false;
    const ObjectKindOfComponent* pilotKinds =
        ecs::try_get<ObjectKindOfComponent>(
            m_world.m_registry, *pilotEntity);
    const ObjectKindOfComponent* vehicleKinds =
        ecs::try_get<ObjectKindOfComponent>(
            m_world.m_registry, *vehicleEntity);
    const OwnerComponent* pilotOwner = ecs::try_get<OwnerComponent>(
        m_world.m_registry, *pilotEntity);
    const OwnerComponent* vehicleOwner = ecs::try_get<OwnerComponent>(
        m_world.m_registry, *vehicleEntity);
    if (!pilotKinds || !game::objectHasKind(
            pilotKinds->mask, game::ObjectKindOf::Infantry) ||
        !vehicleKinds || !game::objectHasKind(
            vehicleKinds->mask, game::ObjectKindOf::Vehicle) ||
        !pilotOwner || pilotOwner->player != request.newOwner ||
        !isObjectDisabledBy(
            m_world.m_registry, *vehicleEntity,
            ObjectDisabledReason::Unmanned, request.confirmedTick)) {
        return false;
    }
    const bool alreadyOwned =
        vehicleOwner && vehicleOwner->player == request.newOwner;
    if (!alreadyOwned && !applyDefection({
            .source = request.pilot,
            .target = request.vehicle,
            .newOwner = request.newOwner,
            .confirmedTick = request.confirmedTick,
        })) {
        return false;
    }
    static_cast<void>(ObjectDisabledSystem::clear(
        m_world.m_registry, *vehicleEntity,
        ObjectDisabledReason::Unmanned, request.confirmedTick));
    static_cast<void>(ObjectStatusSystem::apply(
        m_world.m_registry, *vehicleEntity,
        {.setMask = game::objectStatusBit(
             game::ObjectStatusFlag::Hijacked),
         .confirmedTick = request.confirmedTick}));
    static_cast<void>(m_presentation
        .m_scriptObjects.transferObjectNames(
            request.pilot, request.vehicle));
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) {
        ++m_presentation.m_scriptPresentationSequence;
    }
    static_cast<void>(m_presentation
        .m_scriptObjectPresentation.transferCustomIndicatorColor(
            request.pilot, request.vehicle,
            {.presentationEpoch = m_presentation
                 .m_scriptPresentationEpoch,
             .sequence = m_presentation
                 .m_scriptPresentationSequence,
             .confirmedTick = request.confirmedTick,
             .sourceScriptId = 0,
             .ordinal = 0}));
    static_cast<void>(m_lifecyclePublisher.requestDestroyObject(
        request.pilot, ObjectDestroyReason::System,
        request.confirmedTick));
    return true;
}

bool GameSessionObjectOwnershipTransactions::applyDefectionRecursive(
    const ObjectDefectionRequest& request,
    container::Vector<ObjectId>& visited) {
    if (!m_content.m_active || !request.source || !request.target || !request.newOwner ||
        request.confirmedTick != m_presentation.m_confirmedTick ||
        !m_content.m_players.get(request.newOwner) ||
        m_world.m_objects.isPendingDestroy(request.target)) {
        return false;
    }
    const std::optional<ecs::entity> targetEntity =
        m_world.m_objects.entityFromId(request.target);
    if (!targetEntity ||
        ecs::try_get<ObjectContainedByComponent>(m_world.m_registry, *targetEntity)) {
        return false;
    }
    const OwnerComponent* targetOwner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *targetEntity);
    // Object::defect compares the old controller's default team with the new
    // controller's default team. In the modern registry that is equivalent to
    // comparing controlling PlayerId, not the target's current scenario team.
    if (!targetOwner || !targetOwner->player ||
        targetOwner->player == request.newOwner) {
        return false;
    }
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *targetEntity);
    const game::ObjectStatusMask blocked =
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
        game::objectStatusBit(game::ObjectStatusFlag::Sold);
    if (status && status->hasAny(blocked)) return false;

    if (std::find(visited.begin(), visited.end(), request.target) !=
        visited.end()) {
        return false;
    }
    visited.push_back(request.target);

    // Freeze cross-object edges before mutating ownership. OverlordContain's
    // carried add-on and ParkingPlace/FlightDeck slots are module-owned
    // indices; the session only commits their value snapshots in stable ID
    // order.
    const container::Vector<ObjectId> captureDependents =
        m_world.m_objectSimulation.containmentCaptureDependents(
            m_world.m_registry, m_world.m_objects, request.target);
    const container::Vector<ObjectAirfieldDefectionEntry> airfieldEntries =
        m_world.m_objectSimulation.airfieldDefectionEntries(
            m_world.m_registry, m_world.m_objects, request.target, request.newOwner);

    const std::optional<ObjectTeamId> destination =
        m_world.m_objectTeams.defaultTeam(request.newOwner);
    if (!destination ||
        !transferObjectToTeam(request.target, *destination,
                              request.confirmedTick)) {
        return false;
    }

    // Object::defect publishes the captured object's authored confirmation
    // after ownership has committed.  Keep the cue in the confirmed gameplay
    // journal: a client-side selection/UI path must never invent a defection
    // sound for a rejected or rolled-back ownership transfer.
    if (m_publication) {
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry,
                                                  *targetEntity);
        if (type && type->archetype &&
            !type->archetype->templateData.voiceDefect.empty()) {
            static_cast<void>(m_publication->emitAudioEvent({
                .eventName = type->archetype->templateData.voiceDefect,
                .emitter = request.target,
                .owner = request.target,
            }));
        }
    }

    // Defection is Object::setTeam, not setTemporaryTeam. Preserve the new
    // owner/team as the restoration baseline so a later temporary capture
    // cannot incorrectly return the object to its pre-defection owner.
    if (OriginalOwnershipComponent* original =
            ecs::try_get<OriginalOwnershipComponent>(m_world.m_registry,
                                                       *targetEntity)) {
        original->owner = request.newOwner;
        original->team = *destination;
    } else {
        ecs::emplace<OriginalOwnershipComponent>(
            m_world.m_registry, *targetEntity,
            OriginalOwnershipComponent{
                .owner = request.newOwner,
                .team = *destination,
            });
    }
    const auto commitPermanentOwnership =
        [this, &destination, &request](ObjectId object) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(object);
            if (!entity) return false;
            const OwnerComponent* owner =
                ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
            const PrimaryTeamComponent* team =
                ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, *entity);
            const bool alreadyTransferred = owner && team &&
                owner->player == request.newOwner &&
                team->team == *destination;
            if (!alreadyTransferred &&
                !transferObjectToTeam(object, *destination,
                                      request.confirmedTick)) {
                return false;
            }
            if (OriginalOwnershipComponent* original =
                    ecs::try_get<OriginalOwnershipComponent>(m_world.m_registry,
                                                               *entity)) {
                original->owner = request.newOwner;
                original->team = *destination;
            } else {
                ecs::emplace<OriginalOwnershipComponent>(
                    m_world.m_registry, *entity,
                    OriginalOwnershipComponent{
                        .owner = request.newOwner,
                        .team = *destination,
                    });
            }
            return true;
        };

    // Object::setTeam invokes OverlordContain::onCapture, which directly
    // transfers the first carried add-on. It is not a recursive defect: the
    // add-on remains contained and does not run production/mine recursion.
    for (const ObjectId dependent : captureDependents) {
        static_cast<void>(commitPermanentOwnership(dependent));
    }
    container::Vector<ObjectId> containmentOwnership = captureDependents;
    containmentOwnership.push_back(request.target);
    for (const ObjectId object : containmentOwnership) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        ObjectContainmentRuntimeComponent* containment = entity
            ? ecs::try_get<ObjectContainmentRuntimeComponent>(m_world.m_registry,
                                                               *entity)
            : nullptr;
        if (!containment) continue;
        if (containment->hasCave &&
            containment->caveHasOriginalOwnership) {
            containment->caveOriginalOwner = request.newOwner;
            containment->caveOriginalTeam = *destination;
        }
        if (containment->garrisonHasOriginalOwnership) {
            containment->garrisonOriginalOwner = request.newOwner;
            containment->garrisonOriginalTeam = *destination;
        }
    }
    static_cast<void>(m_world.m_objectSimulation.ejectContainmentOnCapture(
        m_world.m_registry, m_world.m_objects, request.target, request.confirmedTick,
        &m_content.m_players, &m_content.m_contentSnapshot));

    // RefCode defects aircraft which are parked or in takeoff/landing, but an
    // aircraft physically above terrain outside those transitions only loses
    // this airfield reservation (and its producer edge when applicable).
    for (const ObjectAirfieldDefectionEntry& entry : airfieldEntries) {
        if (entry.action == ObjectAirfieldDefectionAction::Defect) {
            static_cast<void>(applyDefectionRecursive(
                ObjectDefectionRequest{
                    .source = request.target,
                    .target = entry.aircraft,
                    .newOwner = request.newOwner,
                    .detectionDurationTicks =
                        request.detectionDurationTicks,
                    .authoredOrder = request.authoredOrder,
                    .confirmedTick = request.confirmedTick,
                }, visited));
            continue;
        }
        static_cast<void>(m_world.m_objectSimulation.releaseAirfieldReservations(
            m_world.m_registry, m_world.m_objects, request.target, entry.aircraft,
            request.confirmedTick));
        if (entry.clearProducer) {
            const std::optional<ecs::entity> aircraft =
                m_world.m_objects.entityFromId(entry.aircraft);
            if (aircraft) {
                ObjectProducerComponent* producer =
                    ecs::try_get<ObjectProducerComponent>(m_world.m_registry,
                                                           *aircraft);
                if (producer && producer->producer == request.target) {
                    producer->producer = INVALID_OBJECT_ID;
                }
            }
        }
    }
    if (request.detectionDurationTicks != 0) {
        const uint64_t endTick = request.detectionDurationTicks >
                std::numeric_limits<uint64_t>::max() - request.confirmedTick
            ? std::numeric_limits<uint64_t>::max()
            : request.confirmedTick + request.detectionDurationTicks;
        ObjectUndetectedDefectorComponent value{
            .detectionEndTick = endTick,
        };
        if (ObjectUndetectedDefectorComponent* existing =
                ecs::try_get<ObjectUndetectedDefectorComponent>(
                    m_world.m_registry, *targetEntity)) {
            *existing = value;
        } else {
            ecs::emplace<ObjectUndetectedDefectorComponent>(
                m_world.m_registry, *targetEntity, value);
        }
    } else {
        ecs::remove<ObjectUndetectedDefectorComponent>(m_world.m_registry,
                                                        *targetEntity);
    }

    // RefCode permanently transfers mines produced by the defecting object.
    // Freeze the candidates before mutation and visit stable ObjectId order.
    container::Vector<ObjectId> mines;
    const auto mineView = ecs::view<const ObjectIdentityComponent,
                                    const ObjectProducerComponent,
                                    const ObjectKindOfComponent>(m_world.m_registry);
    mines.reserve(mineView.size_hint());
    for (const ecs::entity entity : mineView) {
        const ObjectIdentityComponent& identity =
            mineView.template get<const ObjectIdentityComponent>(entity);
        const ObjectProducerComponent& producer =
            mineView.template get<const ObjectProducerComponent>(entity);
        const ObjectKindOfComponent& kinds =
            mineView.template get<const ObjectKindOfComponent>(entity);
        if (identity.id && producer.producer == request.target &&
            !m_world.m_objects.isPendingDestroy(identity.id) &&
            hasObjectKind(&kinds, game::ObjectKindOf::Mine)) {
            mines.push_back(identity.id);
        }
    }
    std::sort(mines.begin(), mines.end());
    for (const ObjectId mine : mines) {
        const std::optional<ecs::entity> mineEntity =
            m_world.m_objects.entityFromId(mine);
        if (!mineEntity ||
            !transferObjectToTeam(mine, *destination,
                                  request.confirmedTick)) {
            continue;
        }
        if (OriginalOwnershipComponent* original =
                ecs::try_get<OriginalOwnershipComponent>(m_world.m_registry,
                                                           *mineEntity)) {
            original->owner = request.newOwner;
            original->team = *destination;
        } else {
            ecs::emplace<OriginalOwnershipComponent>(
                m_world.m_registry, *mineEntity,
                OriginalOwnershipComponent{
                    .owner = request.newOwner,
                    .team = *destination,
                });
        }
    }
    return true;
}

} // namespace engine
