#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/frame/GameSessionDynamicGeometryEventPublisher.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/session/core/GameSession.h"
#include "game/session/core/GameSessionDomainComposition.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/weapon/GameSessionGameplayTransactionDrain.h"

#include "game/audio/EvaEventCatalog.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/simulation/status/ObjectEmpUpdate.h"
#include "game/object/simulation/combat/ObjectFireUpdates.h"
#include "game/object/simulation/combat/ObjectLeafletDrop.h"
#include "game/object/simulation/lifecycle/ObjectHeightDie.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/component/ObjectDirty.h"
#include "game/render/VisualAnimationState.h"
#include "game/script/bridge/ScriptSessionEvents.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace engine {
namespace {

constexpr int32_t kRadarEventStealthDiscovered = 8;
constexpr int32_t kRadarEventStealthNeutralized = 9;

[[nodiscard]] bool hasStealthFeedbackKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf kind) noexcept {
    return kinds && game::objectHasKind(kinds->mask, kind);
}

[[nodiscard]] uint64_t saturatingRadarTickAdd(
    uint64_t value, uint64_t delta) noexcept {
    return value > std::numeric_limits<uint64_t>::max() - delta
        ? std::numeric_limits<uint64_t>::max() : value + delta;
}

} // namespace

void GameSession::updatePostCommandSimulationEvents() {
    container::Vector<ObjectHeightDiePresentationEvent> heightDieEvents =
        domainState().worldState().m_objectSimulation
            .takeHeightDiePresentationEvents();
    for (const ObjectHeightDiePresentationEvent& event : heightDieEvents) {
        static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
            .control = game::FxInvocationControlKind::
                StopAllAttachedParticles,
            .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
            .primary = game::FxInvocationAnchor{.object = event.object},
        }));
    }
    dynamicGeometryEventPublisher().publish();
    for (const ObjectDeployStyleManualFrameEvent& event :
         domainState().worldState().m_objectSimulation
             .takeDeployStyleManualFrameEvents()) {
        const std::optional<ecs::entity> entity =
            domainState().worldState().m_objects
                .entityFromIdIncludingPending(event.object);
        if (!entity) continue;
        RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(
                domainState().worldState().m_registry, *entity);
        if (!visual) continue;
        setVisualAnimationFrame(*visual, event.frame);
        markObjectDirty(
            domainState().worldState().m_registry, *entity,
            ObjectDirtyDomain::RenderExtraction);
    }
    for (ObjectBattlePlanPresentationEvent& event :
         domainState().worldState().m_objectSimulation
             .takeBattlePlanPresentationEvents()) {
        if (!event.transitionSound.empty()) {
            static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                .eventName = std::move(event.transitionSound),
                .emitter = event.source,
                .owner = event.source,
            }));
        }
        if (event.phase == ObjectBattlePlanPresentationPhase::Active &&
            !event.idleLoopSound.empty()) {
            static_cast<void>(gameplayPublicationPort().emitAudioControlEvent({
                .kind = game::GameAudioControlKind::
                    SetObjectLoopingSoundEnabled,
                .enabled = true,
                .eventName = event.idleLoopSound,
                .object = event.source,
            }));
        } else if (event.phase ==
                       ObjectBattlePlanPresentationPhase::Packing &&
                   !event.idleLoopSound.empty()) {
            static_cast<void>(gameplayPublicationPort().emitAudioControlEvent({
                .kind = game::GameAudioControlKind::
                    SetObjectLoopingSoundEnabled,
                .enabled = false,
                .eventName = event.idleLoopSound,
                .object = event.source,
            }));
        }
        if (!event.messageLabel.empty()) {
            gameplayPublicationPort().emitScriptSessionEvent({
                .kind = script::ScriptSessionEventKind::Text,
                .confirmedTick = event.confirmedTick,
                .ordinal = event.authoredOrder,
                .text = std::move(event.messageLabel),
                .localized = true,
            });
        }
        if (!event.announcement.empty()) {
            static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                .eventName = std::move(event.announcement),
                .emitter = event.source,
                .owner = event.source,
            }));
        }
    }
    for (ObjectTacticalPresentationEvent& event :
         domainState().worldState().m_objectSimulation
             .takeTacticalPresentationEvents()) {
        if (event.kind == ObjectTacticalPresentationEventKind::DeployStarted ||
            event.kind == ObjectTacticalPresentationEventKind::UndeployStarted) {
            const std::optional<ecs::entity> source =
                domainState().worldState().m_objects.entityFromId(
                    event.source);
            const ThingTemplateComponent* type = source
                ? ecs::try_get<ThingTemplateComponent>(
                    domainState().worldState().m_registry, *source)
                : nullptr;
            if (type && type->archetype) {
                const container::StringView cue =
                    type->archetype->templateData.perUnitSound(
                        event.kind ==
                                ObjectTacticalPresentationEventKind::
                                    DeployStarted
                            ? "Deploy" : "Undeploy");
                if (!cue.empty()) {
                    static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                        .eventName = container::String{cue},
                        .emitter = event.source,
                        .owner = event.source,
                    }));
                }
            }
            continue;
        }
        if (event.kind ==
            ObjectTacticalPresentationEventKind::CapturePulse) {
            auto& presentation = domainState().presentationState();
            if (presentation.m_objectFeedbackOrdinal !=
                std::numeric_limits<uint64_t>::max()) {
                ++presentation.m_objectFeedbackOrdinal;
            }
            if (presentation.m_objectFeedbackOrdinal == 0)
                presentation.m_objectFeedbackOrdinal = 1;
            const uint64_t duration = std::max<uint64_t>(
                1u, domainState().contentState().m_objectSimulationRules
                        .logicFramesPerSecond / 3u);
            presentation.m_objectSelectionFlashes[event.target] = {
                .identity = presentation.m_objectFeedbackOrdinal,
                .startTick = event.confirmedTick,
                .expireTick = saturatingRadarTickAdd(
                    event.confirmedTick, duration),
            };
            const container::String& tickSound = presentation
                .m_renderGameDataSettings.visual.objectFeedback
                .defectorTimerTickAudioEvent;
            if (!tickSound.empty()) {
                static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                    .eventName = tickSound,
                    .emitter = event.target,
                    .owner = event.target,
                }));
            }
            continue;
        }
        if (event.kind ==
            ObjectTacticalPresentationEventKind::CaptureCompleted) {
            const PlayerState* localPlayer =
                domainState().contentState().m_players.localPlayer();
            const bool sourceLocallyControlled = localPlayer &&
                localPlayer->isCommandPlayer() &&
                domainState().worldState().m_ownership.ownerOf(event.source) ==
                    std::optional<PlayerId>{localPlayer->id};
            const std::optional<ecs::entity> source =
                domainState().worldState().m_objects.entityFromId(
                    event.source);
            const ThingTemplateComponent* type = source
                ? ecs::try_get<ThingTemplateComponent>(
                    domainState().worldState().m_registry, *source)
                : nullptr;
            if (sourceLocallyControlled && type && type->archetype) {
                const game::ThingTemplate& templateData =
                    type->archetype->templateData;
                const container::StringView cue =
                    event.specialPowerType ==
                            game::SpecialPowerType::
                                BlackLotusCaptureBuilding
                        ? templateData.perUnitSound(
                              "VoiceCaptureBuildingComplete")
                        : templateData.resolveUnitSound(
                              templateData.voiceTaskComplete,
                              "VoiceTaskComplete");
                if (!cue.empty()) {
                    static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                        .eventName = container::String{cue},
                        .emitter = event.source,
                        .owner = event.source,
                    }));
                }
            }
            continue;
        }
        if (event.kind == ObjectTacticalPresentationEventKind::
                SpecialAbilityCompleted) {
            const PlayerState* localPlayer =
                domainState().contentState().m_players.localPlayer();
            const bool sourceLocallyControlled = localPlayer &&
                localPlayer->isCommandPlayer() &&
                domainState().worldState().m_ownership.ownerOf(event.source) ==
                    std::optional<PlayerId>{localPlayer->id};
            const std::optional<ecs::entity> source =
                domainState().worldState().m_objects.entityFromId(
                    event.source);
            const ThingTemplateComponent* type = source
                ? ecs::try_get<ThingTemplateComponent>(
                    domainState().worldState().m_registry, *source)
                : nullptr;
            if (sourceLocallyControlled && type && type->archetype) {
                const game::ThingTemplate& templateData =
                    type->archetype->templateData;
                const container::StringView cue =
                    event.specialPowerType ==
                            game::SpecialPowerType::
                                BlackLotusDisableVehicleHack
                        ? templateData.perUnitSound(
                              "VoiceDisableVehicleComplete")
                        : event.specialPowerType ==
                            game::SpecialPowerType::BlackLotusStealCashHack
                        ? templateData.perUnitSound(
                              "VoiceStealCashComplete")
                        : container::StringView{};
                if (!cue.empty()) {
                    static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                        .eventName = container::String{cue},
                        .emitter = event.source,
                        .owner = event.source,
                    }));
                }
            }
            continue;
        }
        if (event.kind ==
            ObjectTacticalPresentationEventKind::PropagandaPulse) {
            const std::optional<game::FxInvocationAnchor> anchor =
                session_fx::snapshotAnchor(
                    domainState().worldState().m_registry,
                    domainState().worldState().m_objects, event.source);
            if (anchor && !event.fxList.empty()) {
                static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
                    .fxListName = std::move(event.fxList),
                    .anchorKind =
                        game::FxInvocationAnchorKind::ObjectAttachment,
                    .primary = *anchor,
                }));
            }
            continue;
        }
        const std::optional<game::FxInvocationAnchor> requester =
            session_fx::snapshotAnchor(
                domainState().worldState().m_registry,
                domainState().worldState().m_objects, event.source);
        const std::optional<game::FxInvocationAnchor> assisted =
            session_fx::snapshotAnchor(
                domainState().worldState().m_registry,
                domainState().worldState().m_objects, event.assisted);
        const std::optional<game::FxInvocationAnchor> target =
            session_fx::snapshotAnchor(
                domainState().worldState().m_registry,
                domainState().worldState().m_objects, event.target);
        const auto emitLaser = [&](container::String resource,
                                   const auto& from, const auto& to) {
            if (resource.empty() || !from || !to) return;
            static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
                .directBeam = game::FxDirectBeamRequest{
                    .objectTemplate = std::move(resource)},
                .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                .primary = *from,
                .secondary = *to,
            }));
        };
        emitLaser(std::move(event.primaryResource), requester, assisted);
        emitLaser(std::move(event.secondaryResource), assisted, target);
    }
    for (ObjectDisguisePresentationEvent& event :
         domainState().worldState().m_objectSimulation
             .takeDisguisePresentationEvents()) {
        const std::optional<ecs::entity> entity =
            domainState().worldState().m_objects.entityFromId(event.object);
        const ThingTemplateComponent* type = entity
            ? ecs::try_get<ThingTemplateComponent>(
                domainState().worldState().m_registry, *entity)
            : nullptr;
        if (type && type->archetype) {
            const container::StringView cue =
                event.kind == ObjectDisguisePresentationEventKind::
                                  DisguiseStarted
                    ? type->archetype->templateData.perUnitSound(
                          "DisguiseStarted")
                    : event.kind == ObjectDisguisePresentationEventKind::
                                        DisguiseRevealedSuccess
                    ? type->archetype->templateData.perUnitSound(
                          "DisguiseRevealedSuccess")
                    : type->archetype->templateData.perUnitSound(
                          "DisguiseRevealedFailure");
            if (!cue.empty()) {
                static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                    .eventName = container::String{cue},
                    .emitter = event.object,
                    .owner = event.object,
                }));
            }
        }
        const std::optional<game::FxInvocationAnchor> anchor =
            session_fx::snapshotAnchor(
                domainState().worldState().m_registry,
                domainState().worldState().m_objects, event.object);
        if (anchor && !event.fxList.empty()) {
            static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
                .fxListName = std::move(event.fxList),
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = *anchor,
            }));
        }
    }
    for (ObjectRailroadPresentationEvent& event :
         domainState().worldState().m_objectSimulation
             .takeRailroadPresentationEvents()) {
        if (event.eventName.empty()) continue;
        if (event.kind ==
                ObjectRailroadPresentationEventKind::RunningLoopStarted ||
            event.kind ==
                ObjectRailroadPresentationEventKind::RunningLoopStopped) {
            static_cast<void>(gameplayPublicationPort().emitAudioControlEvent({
                .kind = game::GameAudioControlKind::
                    SetObjectLoopingSoundEnabled,
                .enabled = event.kind ==
                    ObjectRailroadPresentationEventKind::RunningLoopStarted,
                .eventName = std::move(event.eventName),
                .object = event.object,
            }));
        } else {
            static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                .eventName = std::move(event.eventName),
                .emitter = event.object,
                .owner = event.object,
                .volumeScale = std::clamp(
                    event.volumeScale.to_float(), 0.0f, 1.0f),
            }));
        }
    }
    for (ObjectStickyBombPresentationEvent& event :
         domainState().worldState().m_objectSimulation.takeStickyBombPresentationEvents()) {
        const math::vec3 position{
            event.position.x.to_float(), event.position.y.to_float(),
            event.position.z.to_float()};
        if (event.kind ==
                sticky_bomb::PresentationKind::GeometryDamageFx) {
            if (event.resource.empty()) continue;
            static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
                .fxListName = std::move(event.resource),
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = {.position = position},
                .overrideRadius = event.overrideRadius.to_float(),
            }));
            continue;
        }
        if (!event.resource.empty()) {
            static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                .eventName = std::move(event.resource),
                .emitter = event.bomb,
                .owner = event.bomb,
                .position = position,
            }));
        }
    }
    for (ObjectGrantStealthPulseEvent& event :
         domainState().worldState().m_objectSimulation.takeGrantStealthPulseEvents()) {
        if (event.radiusParticleSystem.empty()) continue;
        const std::optional<game::FxInvocationAnchor> anchor =
            session_fx::snapshotAnchor(domainState().worldState().m_registry, domainState().worldState().m_objects,
                                       event.grantor);
        if (!anchor) continue;
        static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
            .directParticle = game::FxDirectParticleRequest{
                .particleSystemName =
                    std::move(event.radiusParticleSystem),
                .emitterCount = 1,
                .systemLifetimeFrames = event.particleLifetimeFrames,
                .attachToObject = false,
            },
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = *anchor,
        }));
    }
    // StealthDetectorUpdate publishes one compact pulse per scan. Gameplay
    // already extended DETECTED deadlines; this observer-local adapter owns
    // radar/UI/EVA/audio feedback and IR particles without feeding client
    // visibility or presentation timing back into deterministic simulation.
    const PlayerState* localStealthObserver =
        domainState().contentState().m_players.localPlayer();
    const uint64_t stealthFeedbackFramesPerSecond = static_cast<uint64_t>(
        std::max(1, domainState().contentState().m_startInfo.gameSpeedFPS));
    const auto publishStealthRadarEvent =
        [this, stealthFeedbackFramesPerSecond](
            int32_t eventType, const LogicFixedVec3& position,
            uint64_t confirmedTick, uint32_t authoredOrder,
            bool suppressNearbyRecent) {
            auto& presentation = domainState().presentationState();
            auto& history = presentation.m_stealthRadarFeedbackHistory;
            const uint64_t duplicateWindow =
                stealthFeedbackFramesPerSecond * 10u;
            std::erase_if(history, [&](
                    const ObjectStealthRadarFeedbackHistoryEvent& prior) {
                return confirmedTick >= prior.confirmedTick &&
                    confirmedTick - prior.confirmedTick >= duplicateWindow;
            });
            if (suppressNearbyRecent) {
                const math::q32_32 closeDistance{int32_t{250}};
                const math::q32_32 closeDistanceSquared =
                    closeDistance * closeDistance;
                const bool duplicate = std::any_of(
                    history.begin(), history.end(),
                    [&](const ObjectStealthRadarFeedbackHistoryEvent& prior) {
                        if (prior.eventType != eventType ||
                            confirmedTick < prior.confirmedTick ||
                            confirmedTick - prior.confirmedTick >=
                                duplicateWindow) {
                            return false;
                        }
                        const math::q32_32 dx =
                            position.x - prior.position.x;
                        const math::q32_32 dy =
                            position.y - prior.position.y;
                        return dx * dx + dy * dy <= closeDistanceSquared;
                    });
                if (duplicate) return false;
            }
            if (history.size() >=
                script::ScriptMapPresentationState::kMaximumRadarEvents) {
                history.erase(history.begin());
            }
            history.push_back({
                .position = position,
                .eventType = eventType,
                .confirmedTick = confirmedTick,
            });

            if (presentation.m_scriptPresentationSequence !=
                std::numeric_limits<uint64_t>::max()) {
                ++presentation.m_scriptPresentationSequence;
            }
            if (presentation.m_scriptPresentationSequence == 0)
                presentation.m_scriptPresentationSequence = 1;
            const uint64_t lifetime =
                stealthFeedbackFramesPerSecond * 4u;
            const uint64_t dieTick = saturatingRadarTickAdd(
                confirmedTick, lifetime);
            presentation.m_scriptMapPresentation.appendRadarEvent({
                .position = {
                    position.x.to_float(), position.y.to_float(),
                    position.z.to_float()},
                .eventType = eventType,
                .stamp = {
                    .presentationEpoch =
                        presentation.m_scriptPresentationEpoch,
                    .sequence = presentation.m_scriptPresentationSequence,
                    .confirmedTick = confirmedTick,
                    .sourceScriptId = 0,
                    .ordinal = authoredOrder,
                },
                .fadeTick = dieTick -
                    std::min<uint64_t>(
                        dieTick, stealthFeedbackFramesPerSecond / 2u),
                .dieTick = dieTick,
            });
            return true;
        };
    for (ObjectStealthDetectorPulseEvent& event :
         domainState().worldState().m_objectSimulation.takeStealthDetectorPulseEvents()) {
        const std::optional<game::FxInvocationAnchor> detectorAnchor =
            session_fx::snapshotAnchor(domainState().worldState().m_registry, domainState().worldState().m_objects,
                                       event.detector);
        if (!event.pingSound.empty()) {
            static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                .eventName = std::move(event.pingSound),
                .emitter = event.detector,
            }));
        }
        const auto emitAttachedParticle = [this, &event, &detectorAnchor](
                container::String particleSystem) {
            if (!detectorAnchor || particleSystem.empty()) return;
            static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
                .directParticle = game::FxDirectParticleRequest{
                    .particleSystemName = std::move(particleSystem),
                    .emitterCount = 1,
                    .attachToObject = true,
                },
                .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                .primary = *detectorAnchor,
                .boneName = event.particleBone,
            }));
        };
        emitAttachedParticle(std::move(event.particleSystem));
        emitAttachedParticle(std::move(event.beaconParticleSystem));

        if (!event.gridParticleSystem.empty() && detectorAnchor) {
            for (const ObjectId target : event.gridTargets) {
                std::optional<game::FxInvocationAnchor> gridAnchor =
                    session_fx::snapshotAnchor(domainState().worldState().m_registry, domainState().worldState().m_objects, target);
                if (!gridAnchor) continue;
                float& x = gridAnchor->position[0];
                float& y = gridAnchor->position[1];
                if (std::isfinite(x) &&
                    x >= static_cast<float>(std::numeric_limits<int32_t>::min()) &&
                    x <= static_cast<float>(std::numeric_limits<int32_t>::max())) {
                    x -= static_cast<float>(static_cast<int32_t>(x) % 12);
                }
                if (std::isfinite(y) &&
                    y >= static_cast<float>(std::numeric_limits<int32_t>::min()) &&
                    y <= static_cast<float>(std::numeric_limits<int32_t>::max())) {
                    y -= static_cast<float>(static_cast<int32_t>(y) % 12);
                }
                gridAnchor->position[2] =
                    detectorAnchor->position.z() + 17.0f;
                static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
                    .directParticle = game::FxDirectParticleRequest{
                        .particleSystemName = event.gridParticleSystem,
                        .emitterCount = 1,
                        .attachToObject = false,
                    },
                    .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                    .primary = *gridAnchor,
                }));
            }
        }

        if (!localStealthObserver || event.newlyDetectedTargets.empty())
            continue;
        const std::optional<ecs::entity> detectorEntity =
            domainState().worldState().m_objects.entityFromId(event.detector);
        const OwnerComponent* detectorOwner = detectorEntity
            ? ecs::try_get<OwnerComponent>(
                  domainState().worldState().m_registry, *detectorEntity)
            : nullptr;
        if (!detectorEntity || !detectorOwner) continue;
        const RenderObjectFeedbackGameData& feedbackSettings =
            domainState().presentationState().m_renderGameDataSettings.
                visual.objectFeedback;
        for (const ObjectId target : event.newlyDetectedTargets) {
            const std::optional<ecs::entity> targetEntity =
                domainState().worldState().m_objects.entityFromId(target);
            const OwnerComponent* targetOwner = targetEntity
                ? ecs::try_get<OwnerComponent>(
                      domainState().worldState().m_registry, *targetEntity)
                : nullptr;
            const TransformComponent* targetTransform = targetEntity
                ? ecs::try_get<TransformComponent>(
                      domainState().worldState().m_registry, *targetEntity)
                : nullptr;
            if (!targetEntity || !targetOwner || !targetTransform) continue;
            if (domainState().contentState().m_players.relationship(
                    detectorOwner->player, targetOwner->player) ==
                PlayerRelationship::Allies) {
                continue;
            }
            const LogicFixedVec3 targetPosition =
                readAuthoritativeObjectPosition(
                    domainState().worldState().m_registry, *targetEntity,
                    *targetTransform);
            const ObjectStealthComponent* stealth =
                ecs::try_get<ObjectStealthComponent>(
                    domainState().worldState().m_registry, *targetEntity);
            const uint64_t variationKey =
                (static_cast<uint64_t>(event.detector.value) << 32u) ^
                static_cast<uint64_t>(target.value);

            if (localStealthObserver->id == detectorOwner->player &&
                publishStealthRadarEvent(
                    kRadarEventStealthDiscovered, targetPosition,
                    event.confirmedTick, event.authoredOrder, true)) {
                if (!feedbackSettings.stealthDiscoveredAudioEvent.empty()) {
                    static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                        .eventName = feedbackSettings.
                            stealthDiscoveredAudioEvent,
                        .sourcePlayer = localStealthObserver->id,
                    }));
                }
                gameplayPublicationPort().emitScriptSessionEvent({
                    .kind = script::ScriptSessionEventKind::Text,
                    .confirmedTick = event.confirmedTick,
                    .ordinal = event.authoredOrder,
                    .text = "MESSAGE:StealthDiscovered",
                    .localized = true,
                });
                if (stealth && stealth->plan) {
                    if (const std::optional<audio::EvaEventType> eva =
                            game::parseEvaEventType(
                                stealth->plan->enemyDetectionEva)) {
                        evaEventPublisher().publish(
                            *eva, event.confirmedTick, variationKey);
                    }
                }
            }

            if (localStealthObserver->id != targetOwner->player) continue;
            const ObjectKindOfComponent* targetKinds =
                ecs::try_get<ObjectKindOfComponent>(
                    domainState().worldState().m_registry, *targetEntity);
            const bool suppressNearbyRecent =
                hasStealthFeedbackKind(
                    targetKinds, game::ObjectKindOf::Mine) ||
                hasStealthFeedbackKind(
                    targetKinds, game::ObjectKindOf::BoobyTrap) ||
                hasStealthFeedbackKind(
                    targetKinds, game::ObjectKindOf::Demotrap);
            if (!publishStealthRadarEvent(
                    kRadarEventStealthNeutralized, targetPosition,
                    event.confirmedTick, event.authoredOrder,
                    suppressNearbyRecent)) {
                continue;
            }
            if (!feedbackSettings.stealthNeutralizedAudioEvent.empty()) {
                static_cast<void>(gameplayPublicationPort().emitAudioEvent({
                    .eventName =
                        feedbackSettings.stealthNeutralizedAudioEvent,
                    .sourcePlayer = localStealthObserver->id,
                }));
            }
            gameplayPublicationPort().emitScriptSessionEvent({
                .kind = script::ScriptSessionEventKind::Text,
                .confirmedTick = event.confirmedTick,
                .ordinal = event.authoredOrder,
                .text = "MESSAGE:StealthNeutralized",
                .localized = true,
            });
            if (stealth && stealth->plan) {
                if (const std::optional<audio::EvaEventType> eva =
                        game::parseEvaEventType(
                            stealth->plan->ownDetectionEva)) {
                    evaEventPublisher().publish(
                        *eva, event.confirmedTick,
                        variationKey ^ 0x4e45555452414cull);
                }
            }
        }
    }
    // AutoHealBehavior creates one persistent radius emitter per authored
    // module and detached per-target pulse systems. Keep the persistent
    // handle in presentation under a stable attachment group; simulation
    // only publishes Begin/End edges and value anchors.
    for (ObjectAutoHealParticleEvent& event :
         domainState().worldState().m_objectSimulation.takeObjectAutoHealParticleEvents()) {
        uint64_t attachmentGroup =
            static_cast<uint64_t>(event.source.value) *
                0x9e3779b97f4a7c15ull ^
            (static_cast<uint64_t>(event.authoredOrder) +
             0x4155544f4845414cull); // "AUTOHEAL"
        if (attachmentGroup == 0) attachmentGroup = 1;
        if (event.kind == ObjectAutoHealParticleEventKind::RadiusEnd) {
            static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
                .control = game::FxInvocationControlKind::
                    StopAttachedParticleGroup,
                .anchorKind = game::FxInvocationAnchorKind::
                    ObjectAttachment,
                .primary = {.object = event.source},
                .attachmentGroup = attachmentGroup,
            }));
            continue;
        }
        const ObjectId anchorObject =
            event.kind == ObjectAutoHealParticleEventKind::RadiusBegin
                ? event.source : event.target;
        const std::optional<game::FxInvocationAnchor> anchor =
            session_fx::snapshotAnchor(domainState().worldState().m_registry, domainState().worldState().m_objects, anchorObject);
        if (!anchor || event.particleSystem.empty()) continue;
        const bool persistent =
            event.kind == ObjectAutoHealParticleEventKind::RadiusBegin;
        static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
            .directParticle = game::FxDirectParticleRequest{
                .particleSystemName = std::move(event.particleSystem),
                .emitterCount = 1,
                .attachToObject = persistent,
            },
            .anchorKind = persistent
                ? game::FxInvocationAnchorKind::ObjectAttachment
                : game::FxInvocationAnchorKind::WorldPosition,
            .primary = *anchor,
            .attachmentGroup = persistent ? attachmentGroup : 0,
            .localVisibilityRetryFrames = persistent
                ? std::numeric_limits<uint32_t>::max() : 0,
        }));
    }
    // EMPUpdate and LeafletDropBehavior create ParticleSystem instances
    // directly in RefCode rather than through an FXList. Convert their
    // detached confirmed values into the lossless FX stream; presentation
    // owns emitter randomization, attachment and lifetime from here onward.
    for (ObjectSlaveRepairPresentationEvent& event :
         domainState().worldState().m_objectSimulation.takeObjectSlaveRepairPresentationEvents()) {
        const std::optional<game::FxInvocationAnchor> anchor =
            session_fx::snapshotAnchor(domainState().worldState().m_registry, domainState().worldState().m_objects, event.object);
        if (!anchor || event.particleSystem.empty()) continue;
        static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
            .directParticle = game::FxDirectParticleRequest{
                .particleSystemName = std::move(event.particleSystem),
                .emitterCount = 1,
                .systemLifetimeFrames = static_cast<uint32_t>(
                    std::min<uint64_t>(
                        event.lifetimeTicks,
                        std::numeric_limits<uint32_t>::max())),
                .attachToObject = !event.boneName.empty(),
            },
            .anchorKind = !event.boneName.empty()
                ? game::FxInvocationAnchorKind::ObjectAttachment
                : game::FxInvocationAnchorKind::WorldPosition,
            .primary = *anchor,
            .boneName = std::move(event.boneName),
        }));
    }
    for (ObjectTensileFormationEvent& event :
         domainState().worldState().m_objectSimulation.takeObjectTensileFormationEvents()) {
        if (event.kind != ObjectTensileFormationEventKind::CrackSound ||
            event.resource.empty()) {
            continue;
        }
        static_cast<void>(gameplayPublicationPort().emitAudioEvent({
            .eventName = std::move(event.resource),
            .emitter = event.object,
            .owner = event.object,
        }));
    }
    for (ObjectEmpParticleEvent& event :
         domainState().worldState().m_objectSimulation.takeObjectEmpParticleEvents()) {
        const std::optional<game::FxInvocationAnchor> anchor =
            session_fx::snapshotAnchor(domainState().worldState().m_registry, domainState().worldState().m_objects, event.target);
        if (!anchor || event.particleSystem.empty() ||
            event.emitterCount == 0) {
            continue;
        }
        static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
            .directParticle = game::FxDirectParticleRequest{
                .particleSystemName = std::move(event.particleSystem),
                .emitterCount = event.emitterCount,
                .systemLifetimeFrames = event.systemLifetimeFrames,
                .footprintMajorRadius = event.footprintMajorRadius,
                .footprintMinorRadius = event.footprintMinorRadius,
                .maximumHeight = event.maximumHeight,
                .initialDelayMinimumFrames = 1,
                .initialDelayMaximumFrames = 100,
                .boxFootprint = event.boxFootprint,
                .attachToObject = true,
            },
            .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
            .primary = *anchor,
        }));
    }
    for (ObjectLeafletParticleEvent& event :
         domainState().worldState().m_objectSimulation.takeObjectLeafletParticleEvents()) {
        const std::optional<game::FxInvocationAnchor> anchor =
            session_fx::snapshotAnchor(domainState().worldState().m_registry, domainState().worldState().m_objects, event.source);
        if (!anchor || event.particleSystem.empty()) continue;
        static_cast<void>(gameplayPublicationPort().emitFxInvocationEvent({
            .directParticle = game::FxDirectParticleRequest{
                .particleSystemName = std::move(event.particleSystem),
                .emitterCount = 1,
                .attachToObject = true,
            },
            .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
            .primary = *anchor,
        }));
    }
    for (ObjectFireAudioCommand& command :
         domainState().worldState().m_objectSimulation.takeObjectFireAudioCommands()) {
        static_cast<void>(gameplayPublicationPort().emitAudioControlEvent({
            .kind = game::GameAudioControlKind::SetObjectLoopingSoundEnabled,
            .enabled = command.kind == ObjectFireAudioCommandKind::StartLoop,
            .eventName = std::move(command.eventName),
            .object = command.object,
            .emitterKeyOverride = command.emitterKeyOverride,
        }));
    }
    domain().drainGameplayTransactions();

}

} // namespace engine
