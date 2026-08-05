#include "game/session/frame/GameSessionNavigationPresentationRules.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/lifecycle/GameSessionWorldMaintenanceService.h"
#include "game/session/frame/GameSessionDynamicGeometryEventPublisher.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/transaction/GameSessionNavigationTransactions.h"
#include "game/session/transaction/GameSessionNavigationFootprintTransactions.h"
#include "game/session/transaction/GameSessionLifecycleCascadeTransactions.h"
#include "game/session/presentation/GameSessionObjectAmbientAudioLifecycle.h"
#include "game/session/frame/GameSessionDeletePostambleTransactions.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "debug/debug.h"

namespace engine {

GameSessionDeletePostambleTransactions::
GameSessionDeletePostambleTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionObjectEventState& objectEvents,
    GameSessionNavigationTransactions navigation,
    GameSessionGameplayPublicationPort publication) noexcept
    : m_content(content),
      m_world(world),
      m_presentation(presentation),
      m_objectEvents(objectEvents),
      m_navigation(std::move(navigation)),
      m_publication(publication),
      m_ambientAudio(content, world, presentation, publication) {}

void GameSessionDeletePostambleTransactions::consume() {
    container::Vector<ObjectDeletePostambleEvent> events =
        m_world.m_objectSimulation
            .takeObjectDeletePostambleEvents();
    if (events.empty()) return;

    for (const ObjectDeletePostambleEvent& event : events) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromIdIncludingPending(
                event.object);
        if (entity &&
            (session_navigation::blocksGround(
                 m_world.m_registry, *entity) ||
             session_navigation::blocksAircraft(
                 m_world.m_registry, *entity)) &&
            !m_navigation.submitBuildingState(
                event.object, event.confirmedTick,
                navigation::NavigationDynamicEventReason::BuildingDestroyed,
                navigation::NavigationBuildingState::Absent, false)) {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Navigation,
                .code = SimulationFaultCode::AtomicCommitFailed,
                .confirmedTick = event.confirmedTick,
                .subject = event.object.value,
            }));
        }

        // These stable indexes and projections are destroy postamble work.
        // Authored onDelete handlers and their child transactions have
        // already completed, while the pending entity still owns the value
        // components needed by cleanup.
        m_world.m_ownership.apply({
            .kind = ObjectLifecycleEventKind::DestroyRequested,
            .object = event.object,
            .confirmedTick = event.confirmedTick,
        });
        m_world.m_objectTeams.removeObject(event.object);
        m_presentation.m_scriptObjects
            .notifyObjectDestroyed(event.object);
        m_presentation.m_scriptObjectPresentation
            .forgetObject(event.object);
        m_presentation.m_objectSelectionFlashes.erase(
            event.object);
        // Jet/Spectre afterburner is a gameplay-owned loop rather than a
        // Drawable ambient channel, so ObjectAmbientAudioLifecycle cannot
        // retire it.  The pending entity still owns its frozen recipe at this
        // postamble point; explicitly stop the only authored loop identity
        // before physical deletion makes that lookup impossible.
        if (entity) {
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry, *entity);
            if (type && type->archetype) {
                const container::StringView afterburner =
                    type->archetype->templateData.perUnitSound("Afterburner");
                if (!afterburner.empty()) {
                    static_cast<void>(m_publication.emitAudioControlEvent({
                        .kind = game::GameAudioControlKind::
                            SetObjectLoopingSoundEnabled,
                        .enabled = false,
                        .eventName = container::String{afterburner},
                        .object = event.object,
                    }));
                }
            }
        }
        m_ambientAudio.stop(event.object);
    }

    m_world.m_objectProduction.cancelPendingDestroyed(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_players);
    GameSessionWorldMaintenanceService{
        m_content, m_world}
        .refreshObjectDerivedPlayerAggregates(
            events.back().confirmedTick);
    GameSessionDynamicGeometryEventPublisher{
        m_world,
        m_publication}
        .publish();
}

void GameSessionLifecycleCascadeTransactions::projectPresentation(
    container::Span<const ObjectLifecycleEvent> events) {
    GameSessionObjectAmbientAudioLifecycle ambientAudio{
        m_content, m_world, m_presentation, m_publication};
    for (const ObjectLifecycleEvent& event : events) {
        if (event.kind != ObjectLifecycleEventKind::Created) continue;
        ambientAudio.start(event.object);

        // ProductionUpdate owns Object-scope VoiceCreated once for every
        // product and USS VoiceCreate once for the first product in a batch.
        // Both require real factory provenance, not merely a generic
        // Production creation origin: construction sites and script priority
        // builds deliberately share that origin while they are not a factory
        // product and must stay silent here.
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromIdIncludingPending(event.object);
        if (!entity) continue;
        const ObjectLifecycleComponent* lifecycle =
            ecs::try_get<ObjectLifecycleComponent>(
                m_world.m_registry, *entity);
        const ObjectProducedByComponent* producedBy =
            ecs::try_get<ObjectProducedByComponent>(
                m_world.m_registry, *entity);
        if (!lifecycle ||
            lifecycle->origin != ObjectCreationOrigin::Production ||
            !producedBy || !producedBy->producer) {
            continue;
        }
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        if (!type || !type->archetype) continue;
        const game::ThingTemplate& templateData =
            type->archetype->templateData;
        if (!templateData.voiceCreated.empty()) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = templateData.voiceCreated,
                .emitter = event.object,
                .owner = event.object,
            }));
        }
        if (producedBy->quantityIndex == 0) {
            const container::StringView batchVoice =
                templateData.productionBatchVoiceCreate();
            if (!batchVoice.empty()) {
                static_cast<void>(m_publication.emitAudioEvent({
                    .eventName = container::String{batchVoice},
                    .emitter = event.object,
                    .owner = event.object,
                }));
            }
        }
        if (!templateData.soundCreated.empty()) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = templateData.soundCreated,
                .emitter = event.object,
                .owner = event.object,
            }));
        }
    }
}

bool GameSessionLifecycleCascadeTransactions::consume() {
    constexpr size_t kMaximumLifecycleCascade = 262144;
    size_t processed = 0;
    bool queuedDeleteWalk = false;
    while (true) {
        container::Vector<ObjectLifecycleEvent> events =
            m_world.m_objects.takeEvents();
        if (events.empty()) break;
        auto& frameEvents =
            m_objectEvents.m_frameLifecycleEvents;
        frameEvents.reserve(frameEvents.size() + events.size());
        size_t consumedInBatch = 0;
        for (ObjectLifecycleEvent& event : events) {
            if (++processed > kMaximumLifecycleCascade) {
                projectPresentation(
                    {events.data(), consumedInBatch});
                m_ai.m_objectAI.discardMembershipJournal();

                static_cast<void>(m_publication.raiseSimulationFault({
                    .domain = SimulationFaultDomain::Lifecycle,
                    .code = SimulationFaultCode::InvalidEvent,
                    .confirmedTick =
                        m_presentation.m_confirmedTick,
                    .sequence = static_cast<uint32_t>(processed),
                }));
                TD_LOG_ERROR(
                    "[GameSession] Object lifecycle containment cascade exceeded {} events at tick {}",
                    kMaximumLifecycleCascade, m_presentation.m_confirmedTick);
                return queuedDeleteWalk;
            }
            if (event.kind == ObjectLifecycleEventKind::Created) {
                const std::optional<ecs::entity> entity =
                    m_world.m_objects.entityFromId(event.object);
                if (entity) {
                    const ThingTemplateComponent* type =
                        ecs::try_get<ThingTemplateComponent>(m_world.m_registry,
                                                             *entity);
                    if (type && type->archetype &&
                        type->archetype->hasAiUpdate) {
                        const ai::AIObjectMembershipStatus queued =
                            m_ai.m_objectAI.queueMembership({
                                event.object,
                                m_presentation.m_confirmedTick,
                                static_cast<uint32_t>(processed),
                                ai::AIObjectMembershipOperation::Add,
                                type->archetype->initialAiOrderCapabilities,
                            });
                        if (queued != ai::AIObjectMembershipStatus::Success) {
                            static_cast<void>(m_publication.raiseSimulationFault({
                                .domain = SimulationFaultDomain::Membership,
                                .code = queued ==
                                        ai::AIObjectMembershipStatus::
                                            JournalCapacityExceeded
                                    ? SimulationFaultCode::CapacityExceeded
                                    : SimulationFaultCode::InvalidEvent,
                                .confirmedTick = m_presentation.m_confirmedTick,
                                .subject = event.object.value,
                                .sequence = static_cast<uint32_t>(processed),
                            }));
                            TD_LOG_ERROR(
                                "[GameSession] Failed to queue AI creation for object {} at tick {} with status {}",
                                event.object.value, m_presentation.m_confirmedTick,
                                static_cast<uint32_t>(queued));
                        }
                    }
                }
                if (entity && (session_navigation::blocksGround(
                                   m_world.m_registry, *entity) ||
                               session_navigation::blocksAircraft(
                                   m_world.m_registry, *entity))) {
                    const ObjectStatusComponent* status =
                        ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *entity);
                    const bool underConstruction = status && status->hasAny(
                        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
                    const bool blocksGround = session_navigation::blocksGround(
                        m_world.m_registry, *entity);
                    const bool blocksAir = session_navigation::blocksAircraft(
                        m_world.m_registry, *entity);
                    if (!m_navigation.submitBuildingFootprint(
                            event.object,
                            *entity,
                            event.confirmedTick,
                            navigation::NavigationDynamicEventReason::BuildingPlaced,
                            underConstruction
                                ? navigation::NavigationBuildingState::Placed
                                : navigation::NavigationBuildingState::Complete,
                            std::optional<bool>{blocksGround},
                            std::optional<bool>{blocksAir})) {
                        static_cast<void>(m_publication.raiseSimulationFault({
                            .domain = SimulationFaultDomain::Navigation,
                            .code = SimulationFaultCode::AtomicCommitFailed,
                            .confirmedTick = event.confirmedTick,
                            .subject = event.object.value,
                            .sequence = static_cast<uint32_t>(processed),
                        }));
                        TD_LOG_ERROR(
                            "[GameSession] Failed to submit navigation footprint for object {} at tick {}",
                            event.object.value,
                            event.confirmedTick);
                    }
                }
            }
            if (event.kind == ObjectLifecycleEventKind::DestroyRequested ||
                event.kind == ObjectLifecycleEventKind::Destroyed) {
                const ai::AIObjectMembershipStatus queued =
                    m_ai.m_objectAI.queueMembership({
                        event.object,
                        m_presentation.m_confirmedTick,
                        static_cast<uint32_t>(processed),
                        ai::AIObjectMembershipOperation::Remove,
                    });
                if (queued != ai::AIObjectMembershipStatus::Success) {
                    static_cast<void>(m_publication.raiseSimulationFault({
                        .domain = SimulationFaultDomain::Membership,
                        .code = queued ==
                                ai::AIObjectMembershipStatus::
                                    JournalCapacityExceeded
                            ? SimulationFaultCode::CapacityExceeded
                            : SimulationFaultCode::InvalidEvent,
                        .confirmedTick =
                            m_presentation.m_confirmedTick,
                        .subject = event.object.value,
                        .sequence = static_cast<uint32_t>(processed),
                    }));
                    TD_LOG_ERROR(
                        "[GameSession] Failed to queue AI removal for object {} at tick {} with status {}",
                        event.object.value, m_presentation.m_confirmedTick,
                        static_cast<uint32_t>(queued));
                }
            }
            // The original object retains its controlling player throughout
            // Object::onDestroy/onDelete. Keep the modern reverse index until
            // the typed Delete postamble as well; handlers still read the
            // OwnerComponent directly, while child transactions may query
            // the stable ownership service.
            if (event.kind != ObjectLifecycleEventKind::DestroyRequested) {
                m_world.m_ownership.apply(event);
            }
            if (event.kind == ObjectLifecycleEventKind::DestroyRequested) {
                m_world.m_objectSimulation.onObjectDestroyRequested(
                    m_world.m_registry, m_world.m_objects, event.object,
                    event.confirmedTick,
                    {.players = &m_content.m_players,
                     .content = &m_content.m_contentSnapshot,
                     .random = &m_content.m_simulationRandom,
                     .navigation = &m_content.m_navigation,
                     .effects = &m_world.m_objectSimulation});
                queuedDeleteWalk = true;
            }
            frameEvents.push_back(event);
            ++consumedInBatch;
        }
        projectPresentation(events);
    }
    const ai::AIObjectMembershipCommitReport aiMembership =
        m_ai.m_objectAI.commitMembership(m_presentation.m_confirmedTick);
    if (!aiMembership.succeeded()) {
        const bool capacityFailure =
            aiMembership.status ==
                ai::AIObjectMembershipStatus::JournalCapacityExceeded ||
            aiMembership.status ==
                ai::AIObjectMembershipStatus::ActorCapacityExceeded;
        const bool invalidEvent =
            aiMembership.status == ai::AIObjectMembershipStatus::InvalidEvent ||
            aiMembership.status ==
                ai::AIObjectMembershipStatus::ConflictingSequence;
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::Membership,
            .code = capacityFailure
                ? SimulationFaultCode::CapacityExceeded
                : invalidEvent ? SimulationFaultCode::InvalidEvent
                               : SimulationFaultCode::AtomicCommitFailed,
            .confirmedTick =
                m_presentation.m_confirmedTick,
        }));
        TD_LOG_ERROR(
            "[GameSession] Failed to commit object AI membership at tick {} with status {}",
            m_presentation.m_confirmedTick,
            static_cast<uint32_t>(aiMembership.status));
    }
    return queuedDeleteWalk;
}

} // namespace engine
