#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/frame/GameSessionDynamicGeometryEventPublisher.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/core/GameSession.h"
#include "game/session/presentation/GameSessionObjectAmbientAudioLifecycle.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/transaction/GameSessionAIAttackOrderTransactions.h"
#include "game/session/transaction/GameSessionNavigationFootprintTransactions.h"
#include "game/session/transaction/GameSessionNavigationTransactions.h"
#include "game/session/frame/GameSessionObjectDeathFeedbackPublisher.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/session/frame/GameSessionHealthEventPublisher.h"
#include "game/session/frame/GameSessionNavigationPresentationRules.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/simulation/movement/ObjectDynamicGeometry.h"
#include "game/navigation/integration/NavigationFootprintRasterizer.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>

namespace engine {
namespace {

// Radar::RADAR_EVENT_UNDER_ATTACK. The render extraction already derives the
// red type-3 blip from ObjectHealthComponent::lastDamage*, so this session-side
// consumer reuses the number only to key its own suppression history.
constexpr int32_t kRadarEventUnderAttack = 3;

[[nodiscard]] bool playableUnderAttackAudio(
    container::StringView eventName) noexcept {
    return !eventName.empty() &&
        !container::asciiEqualIgnoreCase(eventName, "NoSound");
}

[[nodiscard]] bool hasUnderAttackKind(
    const ObjectKindOfComponent* kinds, game::ObjectKindOf kind) noexcept {
    return kinds && game::objectHasKind(kinds->mask, kind);
}

} // namespace

GameSessionHealthEventPublisher::GameSessionHealthEventPublisher(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionObjectEventState& objectEvents,
    GameSessionGameplayPublicationPort publication) noexcept
    : m_content(content),
      m_world(world),
      m_ai(ai),
      m_presentation(presentation),
      m_objectEvents(objectEvents),
      m_publication(publication),
      m_attackOrders(content, world, ai, presentation),
      m_ambientAudio(content, world, presentation, publication),
      m_deathFeedback(content, presentation, publication) {}

void GameSessionHealthEventPublisher::consume() {
    auto& events = m_objectEvents.m_healthDrainScratch;
    m_world.m_objectSimulation.drainHealthEvents(events);
    // Guard retaliation is a gameplay consequence of this exact Body
    // journal. Commit it before any FX/audio/EVA projection below; a missing
    // presentation consumer must never suppress the order.
    m_attackOrders.produceGuardRetaliationOrders(events);
    auto& frameEvents = m_objectEvents.m_frameHealthEvents;
    frameEvents.reserve(frameEvents.size() + events.size());
    for (const ObjectHealthEvent& event : events) {
        if (event.kind == ObjectHealthEventKind::DamageStateChanged) {
            m_ambientAudio.refresh(event.object, event.currentState);
        } else if (event.kind == ObjectHealthEventKind::Died) {
            m_ambientAudio.stop(event.object);
        }
        if (event.kind == ObjectHealthEventKind::Damaged &&
            !event.damageFxListName.empty()) {
            game::FxInvocationAnchor primary =
                session_fx::snapshotAnchor(
                    m_world.m_registry,
                    m_world.m_objects, event.object)
                    .value_or(session_fx::worldAnchor(
                        {event.victimPositionFixed.x.to_float(),
                         event.victimPositionFixed.y.to_float(),
                         event.victimPositionFixed.z.to_float()},
                        event.object));
            primary.objectBoundingCircleRadius =
                event.victimBoundingCircleRadiusFixed.to_float();
            game::FxInvocationEvent invocation{
                .fxListName = event.damageFxListName,
                .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                .primary = primary,
            };
            if (event.source) {
                invocation.secondary = session_fx::snapshotAnchor(
                    m_world.m_registry,
                    m_world.m_objects, event.source)
                    .value_or(session_fx::worldAnchor(
                        {event.sourcePositionFixed.x.to_float(),
                         event.sourcePositionFixed.y.to_float(),
                         event.sourcePositionFixed.z.to_float()},
                        event.source));
                invocation.secondary->objectBoundingCircleRadius =
                    event.sourceBoundingSphereRadiusFixed.to_float();
            }
            static_cast<void>(m_publication.emitFxInvocationEvent(std::move(invocation)));
            // FxRuntime expands the same typed FXList into particle and Sound
            // nuggets, and AudioSubsystem receives those sound commands once.
            // Do not also emit the catalog's transitive Sound projection here
            // or every DamageFX sound would be played twice.
        }
        if (!event.damageStateAudioEventName.empty()) {
            // ActiveBody binds SoundOnDamaged/ReallyDamaged to the victim
            // ObjectId. Missing AudioEvent definitions degrade later in the
            // audio catalog without changing the confirmed simulation.
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = event.damageStateAudioEventName,
                .emitter = event.object,
                .owner = event.object,
            }));
        }
        if (!event.voiceFearAudioEventName.empty()) {
            // VoiceFear is selected by the authoritative RNG in the Body
            // transaction. Publish only the chosen value event here; audio
            // variation selection remains presentation-owned.
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = event.voiceFearAudioEventName,
                .emitter = event.object,
                .owner = event.object,
                .position = math::vec3{
                    event.victimPositionFixed.x.to_float(),
                    event.victimPositionFixed.y.to_float(),
                    event.victimPositionFixed.z.to_float()},
            }));
        }
        if (event.kind == ObjectHealthEventKind::Damaged) {
            publishUnderAttackFeedback(event);
        }
        if (event.kind == ObjectHealthEventKind::Died) {
            m_deathFeedback.publish(event);
            // Score/loss mutation already happened at the synchronous Body
            // lethal edge, before authored DeathWalk callbacks. This phase is
            // deliberately presentation/diagnostic-only for Died facts.
            // Team death hook production belongs to DeathWalk's fixed
            // postamble, before reconstructing remap work. This consumer only
            // fans out the already-queued health fact; DELETE has no Died edge.
        }
        frameEvents.push_back(event);
    }
    events.clear();
}

void GameSessionHealthEventPublisher::publishUnderAttackFeedback(
    const ObjectHealthEvent& event) {
    // Object::onDamage requires real damage and deliberately excludes the two
    // damage types the player already inflicted on itself knowingly, plus any
    // hit whose source shares the victim's player.
    if (event.actualDamageDealtFixed <= ObjectHealthComponent::Scalar{} ||
        event.damageType == game::DamageType::PENALTY ||
        event.damageType == game::DamageType::HEALING ||
        !event.victimPlayer.isValid() ||
        event.victimPlayer == event.sourcePlayer) {
        return;
    }
    const PlayerState* observer = m_content.m_players.localPlayer();
    if (!observer) return;
    // Only the observed player's own or allied losses produce a warning.
    // RefCode reaches Radar::tryUnderAttackEvent through isLocallyControlled(),
    // so its AllyUnderAttack branch is only live for an observing spectator;
    // deriving the relationship here gives the authored ally announcement a
    // real trigger without changing the own-base case.
    const bool ownVictim = event.victimPlayer == observer->id;
    const bool alliedVictim = !ownVictim &&
        m_content.m_players.relationship(observer->id, event.victimPlayer) ==
            PlayerRelationship::Allies;
    if (!ownVictim && !alliedVictim) return;

    const std::optional<ecs::entity> victimEntity =
        m_world.m_objects.entityFromId(event.object);
    if (!victimEntity) return;
    // Object::m_radarData stays null for a template whose authored
    // RadarPriority is INVALID or NOT_ON_RADAR, and onDamage skips the whole
    // warning for those objects.
    const ThingTemplateComponent* victimType =
        ecs::try_get<ThingTemplateComponent>(
            m_world.m_registry, *victimEntity);
    if (!victimType || !victimType->archetype) return;
    const game::ObjectRadarPriority radarPriority =
        victimType->archetype->templateData.radarPriority;
    if (radarPriority == game::ObjectRadarPriority::Invalid ||
        radarPriority == game::ObjectRadarPriority::NotOnRadar) {
        return;
    }

    // Radar::tryEvent admission. Retail forces the "is close" test to succeed
    // for under-attack events, which makes the ten-second window map-wide
    // instead of local to 250 world units: without that, transport aircraft
    // taking fire re-announce constantly.
    const uint64_t framesPerSecond = static_cast<uint64_t>(
        std::max(1, m_content.m_startInfo.gameSpeedFPS));
    auto& history = m_presentation.m_underAttackRadarFeedbackHistory;
    const uint64_t suppressionWindow = framesPerSecond * 10u;
    std::erase_if(history, [&](
            const ObjectStealthRadarFeedbackHistoryEvent& prior) {
        return event.confirmedTick >= prior.confirmedTick &&
            event.confirmedTick - prior.confirmedTick >= suppressionWindow;
    });
    if (!history.empty()) return;
    history.push_back({
        .position = event.victimPositionFixed,
        .eventType = kRadarEventUnderAttack,
        .confirmedTick = event.confirmedTick,
    });

    // The red type-3 blip is already derived render-side from the victim's
    // ObjectHealthComponent::lastDamage* window, so this consumer deliberately
    // does not append a second radar event: doing so would double the triangle.
    const ObjectKindOfComponent* victimKinds =
        ecs::try_get<ObjectKindOfComponent>(
            m_world.m_registry, *victimEntity);
    const RenderObjectFeedbackGameData& feedbackSettings =
        m_presentation.m_renderGameDataSettings.visual.objectFeedback;
    const container::String* warningSound = nullptr;
    container::StringView caption;
    std::optional<audio::EvaEventType> evaType;
    if (hasUnderAttackKind(victimKinds, game::ObjectKindOf::Infantry) ||
        hasUnderAttackKind(victimKinds, game::ObjectKindOf::Vehicle)) {
        if (hasUnderAttackKind(victimKinds, game::ObjectKindOf::Harvester)) {
            caption = "RADAR:HarvesterUnderAttack";
            warningSound =
                &feedbackSettings.radarHarvesterUnderAttackAudioEvent;
        } else {
            caption = "RADAR:UnitUnderAttack";
            warningSound = &feedbackSettings.radarUnitUnderAttackAudioEvent;
        }
    } else if (hasUnderAttackKind(
                   victimKinds, game::ObjectKindOf::Structure) &&
               hasUnderAttackKind(
                   victimKinds, game::ObjectKindOf::MpCountForVictory)) {
        // Only a victory-counting structure earns the spoken announcement.
        evaType = ownVictim ? audio::EvaEventType::BaseUnderAttack
                            : audio::EvaEventType::AllyUnderAttack;
        caption = "RADAR:StructureUnderAttack";
        warningSound = &feedbackSettings.radarStructureUnderAttackAudioEvent;
    } else {
        caption = "RADAR:UnderAttack";
        warningSound = &feedbackSettings.radarUnderAttackAudioEvent;
    }

    if (warningSound && playableUnderAttackAudio(*warningSound)) {
        // A 2D warning beep for the observer, not a world-positioned impact:
        // RefCode addresses it to the local player index with no emitter.
        static_cast<void>(m_publication.emitAudioEvent({
            .eventName = *warningSound,
            .sourcePlayer = observer->id,
        }));
    }
    m_publication.emitScriptSessionEvent({
        .kind = script::ScriptSessionEventKind::Text,
        .confirmedTick = event.confirmedTick,
        .text = container::String{caption},
        .localized = true,
    });
    if (!evaType) return;
    GameSessionEvaEventPublisher{m_content, m_publication}.publish(
        *evaType, event.confirmedTick,
        (static_cast<uint64_t>(event.object.value) << 32u) ^
            static_cast<uint64_t>(event.source.value));
}

ObjectId GameSession::objectIdFromEntity(ecs::entity entity) const {
    return domainState().worldState().m_objects.objectIdFromEntity(entity);
}

void GameSessionDynamicGeometryEventPublisher::publish() {
    container::Vector<ObjectDynamicGeometryPresentationEvent> events =
        m_world.m_objectSimulation.takeDynamicGeometryPresentationEvents();
    for (const ObjectDynamicGeometryPresentationEvent& event : events) {
        const uint64_t attachmentGroup =
            0x4653544f00000000ull |
            static_cast<uint64_t>(event.authoredOrder);
        game::FxInvocationAnchor objectAnchor =
            session_fx::snapshotAnchor(
                m_world.m_registry, m_world.m_objects, event.object)
                .value_or(session_fx::worldAnchor({
                    event.objectPosition.x.to_float(),
                    event.objectPosition.y.to_float(),
                    event.objectPosition.z.to_float()}, event.object));
        objectAnchor.position = {
            event.objectPosition.x.to_float(),
            event.objectPosition.y.to_float(),
            event.objectPosition.z.to_float(),
        };
        objectAnchor.objectBoundingCircleRadius =
            event.majorRadius.to_float();
        if (event.kind ==
            ObjectDynamicGeometryPresentationEventKind::Stop) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .control =
                    game::FxInvocationControlKind::StopAttachedParticleGroup,
                .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                .primary = objectAnchor,
                .attachmentGroup = attachmentGroup,
            }));
            continue;
        }
        if (event.kind !=
            ObjectDynamicGeometryPresentationEventKind::Start) {
            continue;
        }
        const math::vec3 particleLocalOffset{
            (event.particlePosition.x - event.objectPosition.x).to_float(),
            (event.particlePosition.y - event.objectPosition.y).to_float(),
            (event.particlePosition.z - event.objectPosition.z).to_float(),
        };
        for (const container::String& particle : event.particleSystems) {
            if (particle.empty()) continue;
            // Reuse the renderer-owned attachment group so destruction can
            // stop every slot without exposing emitter handles to gameplay.
            // Firestorm objects are stock IMMOBILE; the detached start
            // position still matches ground+ParticleOffsetZ exactly.
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .directParticle = game::FxDirectParticleRequest{
                    .particleSystemName = particle,
                    .emitterCount = 1,
                    .attachToObject = true,
                },
                .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                .primary = objectAnchor,
                .attachmentLocalOffset = particleLocalOffset,
                .attachmentGroup = attachmentGroup,
            }));
        }
        // RefCode creates ParticleSystem1..16 first and calls FXList::doFXObj
        // only after that loop. Preserve this observable stream order.
        if (!event.fxList.empty()) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .fxListName = event.fxList,
                .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                .primary = objectAnchor,
            }));
        }
    }
}

bool GameSessionNavigationFootprintTransactions::submitBuildingState(
    ObjectId object, uint64_t confirmedTick,
    navigation::NavigationDynamicEventReason reason,
    navigation::NavigationBuildingState state,
    bool blocksNavigation,
    bool blocksAirNavigation) {
    return GameSessionNavigationTransactions{m_content, m_presentation}
        .submitBuildingState(
            object, confirmedTick, reason, state, blocksNavigation,
            blocksAirNavigation);
}

bool GameSessionNavigationFootprintTransactions::submitBuildingFootprint(
    ObjectId object,
    ecs::entity entity,
    uint64_t confirmedTick,
    navigation::NavigationDynamicEventReason reason,
    navigation::NavigationBuildingState state,
    std::optional<bool> blocksNavigationOverride,
    std::optional<bool> blocksAirNavigationOverride,
    std::optional<bool> rubbleSurfaceOverride) {
    if (!m_content.m_navigation.isInitialized()) return true;
    const ObjectMapStatusComponent* mapStatus =
        ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, entity);
    if (mapStatus && mapStatus->offMap) return true;
    const ObjectFixedTransformComponent* fixedTransform =
        ecs::try_get<ObjectFixedTransformComponent>(
            m_world.m_registry, entity);
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, entity);
    // Optional/missing authored geometry has no safe obstacle to publish.
    // ObjectLifecycle normally guarantees this component, but degraded Mod
    // content must remain a deterministic no-op rather than manufacturing a
    // radius from renderer data.
    if (!geometry) return true;
    if (!fixedTransform || !fixedTransform->authoritative ||
        geometry->majorRadiusFixed < math::q32_32{} ||
        geometry->minorRadiusFixed < math::q32_32{}) {
        return false;
    }

    const LogicFixedVec3 position = fixedTransform->position;
    const math::q32_32 yaw = fixedTransform->yawRadians;
    using Fixed = math::q32_32;
    navigation::NavigationFootprint authored{
        .kind = geometry->shape == ObjectGeometryShape::Box
            ? navigation::NavigationFootprintKind::OrientedBox
            : navigation::NavigationFootprintKind::Circle,
        .center = {
            position.x.raw(),
            position.y.raw(),
            0,
        },
        .halfExtentXRaw =
            geometry->majorRadiusFixed.raw(),
        .halfExtentYRaw =
            geometry->minorRadiusFixed.raw(),
        .yawRaw = yaw.raw(),
    };

    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(
            m_world.m_registry, entity);
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(
            m_world.m_registry, entity);
    const bool rubbleSurface = rubbleSurfaceOverride.value_or(
        session_navigation::isRubbleBlocker(
            m_world.m_registry, entity));
    bool fenceSurface = false;
    if (authored.kind == navigation::NavigationFootprintKind::OrientedBox &&
        kinds && game::objectHasKind(
            kinds->mask, game::ObjectKindOf::LineBuild)) {
        authored.kind = navigation::NavigationFootprintKind::Line;
    }

    // ZH Pathfinder::classifyObjectFootprint gives non-defensive authored
    // fences their own thin, offset line instead of using GeometryInfo.  A
    // defensive wall deliberately keeps its normal object geometry.
    if (type && type->archetype &&
        type->archetype->templateData.fenceWidthFixed > Fixed{} &&
        !(kinds && game::objectHasKind(
            kinds->mask, game::ObjectKindOf::DefensiveWall))) {
        const Fixed halfWidth =
            type->archetype->templateData.fenceWidthFixed /
            Fixed{int32_t{2}};
        const math::q32_32_sincos heading =
            math::fixed_sincos(Fixed::from_raw(authored.yawRaw));
        const Fixed offset =
            type->archetype->templateData.fenceXOffsetFixed;
        const Fixed centerX = Fixed::from_raw(authored.center.xRaw) -
                              heading.cosine * offset;
        const Fixed centerY = Fixed::from_raw(authored.center.yRaw) -
                              heading.sine * offset;
        authored.kind = navigation::NavigationFootprintKind::Fence;
        fenceSurface = !rubbleSurface;
        authored.center.xRaw = centerX.raw();
        authored.center.yRaw = centerY.raw();
        authored.halfExtentXRaw = halfWidth.raw();
        authored.halfExtentYRaw =
            (Fixed::from_raw(
                 m_content.m_navigation.grid().transform().cellSizeRaw) /
             Fixed{int32_t{10}}).raw();
    }

    const uint64_t eventTick = confirmedTick == 0 ? 1 : confirmedTick;
    const auto submitTypedFootprint =
        [this, object, eventTick](
            const navigation::NavigationBuildingEvent& event,
            container::Span<const navigation::NavigationCellId> cells) {
            const navigation::NavigationDynamicOverlayResult submitted =
                m_content.m_navigation.submitBuildingEvent(
                    event, cells);
            if (submitted == navigation::NavigationDynamicOverlayResult::Success)
                return true;
            TD_LOG_ERROR(
                "[GameSession] Navigation footprint transaction rejected: object={} result={} cells={} tick={} reason={} state={} blocksGround={} blocksAir={} rubble={} fence={}",
                object.value, static_cast<uint32_t>(submitted), cells.size(),
                eventTick, static_cast<uint32_t>(event.reason),
                static_cast<uint32_t>(event.state), event.blocksNavigation,
                event.blocksAirNavigation, event.rubbleSurface,
                event.fenceSurface);
            if (submitted ==
                    navigation::NavigationDynamicOverlayResult::
                        FootprintCapacityExceeded ||
                submitted ==
                    navigation::NavigationDynamicOverlayResult::
                        EventCapacityExceeded) {
                static_cast<void>(m_publication.raiseSimulationFault({
                    .domain = SimulationFaultDomain::Navigation,
                    .code = SimulationFaultCode::CapacityExceeded,
                    .confirmedTick = eventTick,
                    .subject = object.value,
                    .sequence = static_cast<uint32_t>(submitted),
                }));
            }
            return false;
        };
    if (geometry->isSmall &&
        authored.kind != navigation::NavigationFootprintKind::Fence) {
        const bool blocksGround = rubbleSurface
            ? false
            : blocksNavigationOverride.value_or(false);
        const bool blocksAir = rubbleSurface
            ? false
            : blocksAirNavigationOverride.value_or(false);
        return submitTypedFootprint(
            {eventTick, object.value, reason, state,
             blocksGround, true, {}, blocksAir,
             rubbleSurface, fenceSurface}, {});
    }

    container::Vector<navigation::NavigationCellId>& footprint =
        m_content.m_navigationFootprintScratch;
    const size_t gridCellCount =
        m_content.m_navigation.grid().cellCount();
    if (footprint.size() < gridCellCount)
        footprint.resize(gridCellCount);
    const navigation::NavigationFootprintRasterResult raster =
        navigation::NavigationFootprintRasterizer::rasterize(
            m_content.m_navigation.grid(),
            authored,
            footprint);
    if (raster.status ==
        navigation::NavigationFootprintRasterStatus::CapacityExceeded) {
        TD_LOG_ERROR(
            "[GameSession] Navigation footprint raster capacity exceeded: object={} requiredAtLeast={} capacity={} kind={} halfExtentsRaw=({}, {})",
            object.value, raster.requiredCount, footprint.size(),
            static_cast<uint32_t>(authored.kind), authored.halfExtentXRaw,
            authored.halfExtentYRaw);
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::Navigation,
            .code = SimulationFaultCode::CapacityExceeded,
            .confirmedTick = eventTick,
            .subject = object.value,
            .sequence = raster.requiredCount,
        }));
        return false;
    }
    if (raster.status != navigation::NavigationFootprintRasterStatus::Success) {
        TD_LOG_ERROR(
            "[GameSession] Navigation footprint raster rejected: object={} status={} kind={} centerRaw=({}, {}) halfExtentsRaw=({}, {}) yawRaw={}",
            object.value, static_cast<uint32_t>(raster.status),
            static_cast<uint32_t>(authored.kind), authored.center.xRaw,
            authored.center.yRaw, authored.halfExtentXRaw,
            authored.halfExtentYRaw, authored.yawRaw);
        return false;
    }

    const bool blocksGround = rubbleSurface
        ? false
        : blocksNavigationOverride.value_or(raster.requiredCount != 0);
    const bool blocksAir = rubbleSurface
        ? false
        : blocksAirNavigationOverride.value_or(false);
    const navigation::NavigationBuildingEvent event{
        eventTick,
        object.value,
        reason,
        state,
        blocksGround,
        true,
        {},
        blocksAir,
        rubbleSurface,
        fenceSurface,
    };
    return submitTypedFootprint(
        event,
        container::Span<const navigation::NavigationCellId>{
            footprint.data(), raster.writtenCount});
}

} // namespace engine
