#include "game/session/core/GameSessionDomainComposition.h"
#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/frame/GameSessionHealthEventPublisher.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

#include "core/container/string_utils.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/runtime/ObjectDeathEvents.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/structure/ObjectMinefield.h"
#include "game/object/simulation/structure/ObjectMissileLauncherBuilding.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/structure/ObjectParticleUplinkCannon.h"
#include "game/object/simulation/lifecycle/ObjectRebuildHole.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/simulation/movement/ObjectWaveGuide.h"

#include <algorithm>
#include <limits>

namespace engine::detail {

void GameSessionDomainComposition::publishPostCommandHealthAndCollectSimulationEvents() {
    GameSessionHealthEventPublisher{
        m_content,
        m_world,
        m_ai,
        m_presentation,
        m_objectEvents,
        m_publication}
        .consume();
    for (ObjectToppleFxEvent& event :
         m_world.m_objectSimulation.takeToppleFxEvents()) {
        if (event.fxList.empty()) continue;
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .fxListName = std::move(event.fxList),
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = game::FxInvocationAnchor{
                .object = event.object,
                .position = {event.position.x.to_float(),
                             event.position.y.to_float(),
                             event.position.z.to_float()},
                .yawRadians = event.yawRadians.to_float(),
            },
        }));
    }
    for (ObjectSupplyEvent& event :
         m_world.m_objectSimulation.takeSupplyEvents()) {
        std::optional<math::vec3> position;
        const ThingTemplateComponent* truckType = nullptr;
        const OwnerComponent* truckOwner = nullptr;
        if (const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromIdIncludingPending(event.truck)) {
            if (const TransformComponent* transform =
                    ecs::try_get<TransformComponent>(m_world.m_registry, *entity)) {
                position = math::vec3{transform->x, transform->y, transform->z};
            }
            truckType = ecs::try_get<ThingTemplateComponent>(
                m_world.m_registry, *entity);
            truckOwner = ecs::try_get<OwnerComponent>(
                m_world.m_registry, *entity);
        }
        if (event.kind == ObjectSupplyEventKind::SuppliesDepletedVoice &&
            !event.resource.empty()) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = std::move(event.resource),
                .emitter = event.truck,
                .owner = event.truck,
                .position = position,
            }));
            continue;
        }
        // VoiceSupply has no dedicated modern command family: supply trucks
        // choose a dock autonomously.  DockEntered is the confirmed
        // equivalent of RefCode's MSG_DOCK boundary and is emitted once for
        // a real arrival, unlike reservation or every resource-box transfer.
        const PlayerState* localPlayer = m_content.m_players.localPlayer();
        if (event.kind != ObjectSupplyEventKind::DockEntered ||
            !localPlayer || !truckOwner ||
            truckOwner->player != localPlayer->id ||
            !truckType || !truckType->archetype) {
            continue;
        }
        const container::StringView cue =
            truckType->archetype->templateData.perUnitSound("VoiceSupply");
        if (!cue.empty()) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = container::String{cue},
                .emitter = event.truck,
                .owner = event.truck,
                .position = position,
            }));
        }
    }
    // HackInternetAIUpdate plays UnitUnpack/UnitPack on state entry and
    // UnitCashPing only after a successful player cash commit.  The economy
    // runtime is authoritative for all three edges; this presentation cache
    // observes it after the confirmed economy phase and never feeds back.
    struct Hacker final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Hacker> hackers;
    const auto hackerView = ecs::view<
        const ObjectIdentityComponent, const ObjectEconomyComponent>(
        m_world.m_registry);
    for (const ecs::entity entity : hackerView) {
        const ObjectEconomyComponent& economy =
            hackerView.template get<const ObjectEconomyComponent>(entity);
        if (economy.hackInternet.empty()) continue;
        hackers.push_back({
            .object = hackerView.template get<const ObjectIdentityComponent>(
                entity).id,
            .entity = entity,
        });
    }
    std::sort(hackers.begin(), hackers.end(),
              [](const Hacker& left, const Hacker& right) {
                  return left.object < right.object;
              });
    // The cache is presentation-only, but ObjectIds are session-local and
    // hacker modules may be destroyed or replaced. Prune before observing the
    // next edge so it neither grows with a long match nor leaks an old phase
    // into a future object that reuses an id after a lifecycle reset.
    std::erase_if(m_presentation.m_hackInternetAudio,
                  [&hackers](const auto& entry) {
                      const auto found = std::lower_bound(
                          hackers.begin(), hackers.end(), entry.first,
                          [](const Hacker& hacker, ObjectId object) {
                              return hacker.object < object;
                          });
                      return found == hackers.end() ||
                          found->object != entry.first;
                  });
    for (const Hacker& hacker : hackers) {
        const ObjectEconomyComponent& economy =
            ecs::get<ObjectEconomyComponent>(m_world.m_registry,
                                             hacker.entity);
        auto [historyEntry, inserted] =
            m_presentation.m_hackInternetAudio.try_emplace(hacker.object);
        auto& history = historyEntry->second;
        if (inserted || history.size() != economy.hackInternet.size()) {
            history.clear();
            history.reserve(economy.hackInternet.size());
            for (const ObjectHackInternetRuntime& runtime :
                 economy.hackInternet) {
                history.push_back({
                    .phase = runtime.phase,
                    .revision = runtime.revision,
                });
            }
            continue;
        }
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry,
                                                  hacker.entity);
        for (size_t index = 0; index < economy.hackInternet.size(); ++index) {
            const ObjectHackInternetRuntime& runtime =
                economy.hackInternet[index];
            ObjectHackInternetAudioPresentationState& previous =
                history[index];
            const bool revisionAdvanced = runtime.revision > previous.revision;
            const uint64_t revisionDelta = revisionAdvanced
                ? runtime.revision - previous.revision : 0;
            const bool skippedUnpacking =
                previous.phase == ObjectHackInternetRuntimePhase::Idle &&
                runtime.phase == ObjectHackInternetRuntimePhase::Hacking &&
                revisionDelta >= 2u;
            const bool skippedPacking =
                previous.phase == ObjectHackInternetRuntimePhase::Hacking &&
                runtime.phase == ObjectHackInternetRuntimePhase::Idle &&
                revisionDelta >= 2u;
            container::StringView cue;
            if ((runtime.phase == ObjectHackInternetRuntimePhase::Unpacking &&
                 previous.phase != ObjectHackInternetRuntimePhase::Unpacking) ||
                skippedUnpacking) {
                cue = "UnitUnpack";
            } else if ((runtime.phase == ObjectHackInternetRuntimePhase::Packing &&
                        previous.phase != ObjectHackInternetRuntimePhase::Packing) ||
                       skippedPacking) {
                cue = "UnitPack";
            } else if (previous.phase == ObjectHackInternetRuntimePhase::Hacking &&
                       runtime.phase == ObjectHackInternetRuntimePhase::Hacking &&
                       revisionAdvanced) {
                cue = "UnitCashPing";
            }
            if (!cue.empty() && type && type->archetype) {
                const container::StringView eventName =
                    type->archetype->templateData.perUnitSound(cue);
                if (!eventName.empty()) {
                    static_cast<void>(m_publication.emitAudioEvent({
                        .eventName = container::String{eventName},
                        .emitter = hacker.object,
                        .owner = hacker.object,
                    }));
                }
            }
            previous = {
                .phase = runtime.phase,
                .revision = runtime.revision,
            };
        }
    }
    for (const ObjectSpecialAbilityFacingRequest& request :
         m_world.m_objectSimulation.takeSpecialAbilityFacingRequests()) {
        if (!m_world.m_objectSimulation.acceptsSpecialAbilityFacingRequest(
                m_world.m_registry, m_world.m_objects, request)) {
            continue;
        }
        std::optional<ai::AIFixedPosition> targetPosition;
        if (!request.target && request.hasTargetPosition) {
            targetPosition = ai::AIFixedPosition{
                request.targetPosition.x.raw(),
                request.targetPosition.y.raw(),
                request.targetPosition.z.raw()};
        }
        const ai::ObjectAIFacingTransitionResult staged =
            m_ai.m_objectAI.stageFacingState(
                request.source, request.target, targetPosition,
                request.confirmedTick);
        const bool terminalFailure =
            staged.status == ai::ObjectAIFacingTransitionStatus::InvalidSubject ||
            staged.status == ai::ObjectAIFacingTransitionStatus::InvalidTarget;
        static_cast<void>(
            m_world.m_objectSimulation
                .acknowledgeSpecialAbilityFacingRequest(
                    m_world.m_registry, m_world.m_objects, request,
                    staged.succeeded(), terminalFailure,
                    staged.request.issuedTick, staged.request.sequence));
    }

    m_ai.m_objectAI.transients().discardMovementCommands();
    auto& movementFeedback =
        m_ai.m_movementFeedbackDrainScratch;
    m_world.m_objectSimulation.drainAIMovementFeedback(
        movementFeedback);
    for (const ai::MovementFeedback& feedback : movementFeedback) {
        const ai::ObjectAITransientStatus staged =
            m_ai.m_objectAI.transients().stage(feedback);
        if (staged != ai::ObjectAITransientStatus::Success) {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Feedback,
                .code = staged == ai::ObjectAITransientStatus::CapacityExceeded
                    ? SimulationFaultCode::CapacityExceeded
                    : SimulationFaultCode::InvalidEvent,
                .confirmedTick = m_presentation.m_confirmedTick,
                .subject = feedback.correlation.subject.value,
            }));
            TD_LOG_ERROR(
                "[GameSession] Object AI movement feedback overflow: "
                "subject={} tick={} status={}",
                feedback.correlation.subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(staged));
            movementFeedback.clear();
            return;
        }
    }
    movementFeedback.clear();
    auto& facingFeedback =
        m_ai.m_facingFeedbackDrainScratch;
    m_world.m_objectSimulation.drainAIFacingFeedback(
        facingFeedback);
    for (const ai::AIFacingFeedback& feedback : facingFeedback) {
        ai::ObjectAITransientStore& transients = m_ai.m_objectAI.transients();
        const ai::ObjectAITransientStatus staged = transients.stage(feedback);
        if (staged != ai::ObjectAITransientStatus::Success) {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Feedback,
                .code = staged == ai::ObjectAITransientStatus::CapacityExceeded
                    ? SimulationFaultCode::CapacityExceeded
                    : SimulationFaultCode::InvalidEvent,
                .confirmedTick = m_presentation.m_confirmedTick,
                .subject = feedback.subject.value,
            }));
            TD_LOG_ERROR(
                "[GameSession] Object AI facing feedback overflow: "
                "subject={} tick={} status={}",
                feedback.subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(staged));
            facingFeedback.clear();
            return;
        }
        if (feedback.status == ai::AIFacingFeedbackStatus::Completed ||
            feedback.status == ai::AIFacingFeedbackStatus::TargetLost ||
            feedback.status == ai::AIFacingFeedbackStatus::Unsupported) {
            static_cast<void>(
                m_world.m_objectSimulation
                    .acknowledgeSpecialAbilityFacingFeedback(
                        m_world.m_registry, m_world.m_objects,
                        feedback.subject, feedback.request.issuedTick,
                        feedback.request.sequence,
                        feedback.status ==
                            ai::AIFacingFeedbackStatus::Completed));
            static_cast<void>(transients.removeFacingCommand(
                feedback.subject, feedback.request));
        }
    }
    facingFeedback.clear();
    for (ObjectChinookRopePresentationEvent& ropeEvent :
         m_world.m_objectSimulation.takeChinookRopePresentationEvents()) {
        // RefCode creates the rope Drawable only when RopeName resolves to a
        // ThingTemplate. Keep gameplay timing alive when Mod content is
        // missing the visual template, but do not invent a generic line.
        if (ropeEvent.control != ObjectChinookRopePresentationControl::End &&
            (ropeEvent.ropeName.empty() ||
             !m_content.m_contentSnapshot.findObjectArchetype(ropeEvent.ropeName))) {
            continue;
        }
        const auto control = [&ropeEvent]() {
            switch (ropeEvent.control) {
            case ObjectChinookRopePresentationControl::Update:
                return game::FxDirectRopeControl::Update;
            case ObjectChinookRopePresentationControl::End:
                return game::FxDirectRopeControl::End;
            case ObjectChinookRopePresentationControl::Begin:
            default:
                return game::FxDirectRopeControl::Begin;
            }
        }();
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .directRope = game::FxDirectRopeRequest{
                .control = control,
                .ropeIdentity = ropeEvent.ropeIdentity,
                .maximumLength = ropeEvent.maximumLength,
                .currentLength = ropeEvent.currentLength,
                .width = ropeEvent.width,
                .color = ropeEvent.color,
                .wobbleLength = ropeEvent.wobbleLength,
                .wobbleAmplitude = ropeEvent.wobbleAmplitude,
                .wobbleRatePerFrame = ropeEvent.wobbleRatePerFrame,
                .wobblePhase = ropeEvent.wobblePhase,
                .verticalOffset = ropeEvent.verticalOffset,
                .currentSpeedPerFrame = ropeEvent.currentSpeedPerFrame,
                .maximumSpeedPerFrame = ropeEvent.maximumSpeedPerFrame,
                .accelerationPerFrame = ropeEvent.accelerationPerFrame,
            },
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = game::FxInvocationAnchor{
                .object = ropeEvent.object,
                .position = {ropeEvent.anchor.x.to_float(),
                             ropeEvent.anchor.y.to_float(),
                             ropeEvent.anchor.z.to_float()},
            },
        }));
    }
    m_debris.publish();
    for (ObjectMinefieldFxEvent& event :
         m_world.m_objectSimulation.takeMinefieldFxEvents()) {
        if (event.fxList.empty()) continue;
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .fxListName = std::move(event.fxList),
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = game::FxInvocationAnchor{
                .object = event.source,
                .position = math::vec3{event.position.x.to_float(),
                                       event.position.y.to_float(),
                                       event.position.z.to_float()},
            },
        }));
    }
    for (ObjectNeutronMissilePresentationEvent& event :
         m_world.m_objectSimulation.takeNeutronMissilePresentationEvents()) {
        if (event.kind ==
                ObjectNeutronMissilePresentationEventKind::InitialFx) {
            if (event.fxList.empty()) continue;
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .fxListName = std::move(event.fxList),
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = game::FxInvocationAnchor{
                    .object = event.source,
                    .position = {event.position.x.to_float(),
                                 event.position.y.to_float(),
                                 event.position.z.to_float()},
                    },
                }));
        } else if (event.size > math::q32_32{}) {
            // ZH's NeutronMissileSlowDeathBehavior places SCORCH_1 exactly
            // once, on the first blast that actually damages an object.
            // Deliver it through the same confirmed FX stream as all other
            // terrain scorches; retaining it in a session-local journal left
            // the event without a presentation consumer.
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .directScorch = game::FxDirectScorchRequest{
                    .type = game::FxDirectScorchType::Scorch1,
                    .radius = event.size.to_float(),
                },
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = game::FxInvocationAnchor{
                    .object = event.source,
                    .position = {event.position.x.to_float(),
                                 event.position.y.to_float(),
                                 event.position.z.to_float()},
                },
            }));
        }
    }
    for (ObjectWaveGuideEvent& event :
         m_world.m_objectSimulation.takeWaveGuideEvents()) {
        if (event.kind == ObjectWaveGuideEventKind::RandomSplashSound) {
            if (event.effect.empty()) continue;
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = std::move(event.effect),
                .emitter = event.source,
                .owner = event.source,
                .position = math::vec3{event.position.x.to_float(),
                                       event.position.y.to_float(),
                                       event.position.z.to_float()},
            }));
            continue;
        }
        if (event.kind == ObjectWaveGuideEventKind::FrontAdvanced) {
            static_cast<void>(m_world.m_clientTerrainObjects.applyWaveFront({
                .center = {event.position.x.to_float(),
                           event.position.y.to_float(),
                           event.position.z.to_float()},
                .yawRadians = event.rotationRadians.to_float(),
                .ySize = event.ySize.to_float(),
                .bendMagnitude = event.bendMagnitude.to_float(),
                .damageRadius = event.damageRadius.to_float(),
                .toppleForce = event.toppleForce.to_float(),
                .preferredHeight = event.preferredHeight.to_float(),
            }));
            continue;
        }
        if (event.kind == ObjectWaveGuideEventKind::Finished &&
            event.attachmentGroup != 0) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .control = game::FxInvocationControlKind::
                    StopAttachedParticleGroup,
                .anchorKind = game::FxInvocationAnchorKind::
                    ObjectAttachment,
                .primary = {.object = event.source},
                .attachmentGroup = event.attachmentGroup,
            }));
        }
        if (event.effect.empty()) continue;
        if (event.kind == ObjectWaveGuideEventKind::FrontSpray) {
            const std::optional<game::FxInvocationAnchor> anchor =
                session_fx::snapshotAnchor(
                    m_world.m_registry, m_world.m_objects, event.source);
            if (!anchor) continue;
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .directParticle = game::FxDirectParticleRequest{
                    .particleSystemName = std::move(event.effect),
                    .emitterCount = 1,
                    .attachToObject = true,
                },
                .anchorKind = game::FxInvocationAnchorKind::
                    ObjectAttachment,
                .primary = *anchor,
                .attachmentLocalOffset = {
                    event.position.x.to_float(),
                    event.position.y.to_float(),
                    event.position.z.to_float()},
                .attachmentGroup = event.attachmentGroup,
            }));
            continue;
        }
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .directParticle = game::FxDirectParticleRequest{
                .particleSystemName = std::move(event.effect),
                .emitterCount = 1,
            },
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = {
                .object = event.source,
                .position = {event.position.x.to_float(),
                             event.position.y.to_float(),
                             event.position.z.to_float()},
                .yawRadians = event.rotationRadians.to_float(),
            },
        }));
    }
    for (ObjectMissileLauncherFxEvent& event :
         m_world.m_objectSimulation.takeMissileLauncherFxEvents()) {
        if (event.fxList.empty()) continue;
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .fxListName = std::move(event.fxList),
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = game::FxInvocationAnchor{
                .object = event.object,
                .position = {event.position.x.to_float(),
                             event.position.y.to_float(),
                             event.position.z.to_float()},
            },
        }));
    }
    const auto particleUplinkKey = [](ObjectId object, uint32_t order,
                                      uint64_t lane) noexcept {
        uint64_t value = object.value ^
            (static_cast<uint64_t>(order) << 32u) ^ lane;
        value += 0x9e3779b97f4a7c15ull;
        value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
        value ^= value >> 31u;
        return value == 0 ? uint64_t{1} : value;
    };
    for (ObjectParticleUplinkPhaseEvent& event :
         m_world.m_objectSimulation.takeParticleUplinkPhaseEvents()) {
        const std::optional<game::FxInvocationAnchor> anchor =
            session_fx::snapshotAnchor(
                m_world.m_registry, m_world.m_objects, event.object);
        const game::FxInvocationAnchor controlAnchor = anchor
            ? *anchor : game::FxInvocationAnchor{.object = event.object};
        const uint64_t particleGroup = particleUplinkKey(
            event.object, event.authoredOrder, 0x50554347524f5550ull);
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .control = game::FxInvocationControlKind::StopAttachedParticleGroup,
            .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
            .primary = controlAnchor,
            .attachmentGroup = particleGroup,
        }));

        const uint32_t outerCount = std::min<uint32_t>(
            event.outerBoneCount, 64u);
        for (uint32_t index = 0; index < outerCount; ++index) {
            const uint64_t connectorIdentity = particleUplinkKey(
                event.object, event.authoredOrder,
                0x434f4e4e00000000ull + index);
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .directBeam = game::FxDirectBeamRequest{
                    .control = game::FxDirectBeamControl::End,
                    .beamIdentity = connectorIdentity,
                },
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = {},
            }));
        }
        if (!anchor) continue;

        const auto emitAttachedParticle = [&](
            container::StringView particle, container::StringView bone,
            uint32_t ordinal, bool prefixFallback) {
            if (particle.empty() || bone.empty()) return;
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .directParticle = game::FxDirectParticleRequest{
                    .particleSystemName = container::String{particle},
                    .emitterCount = 1,
                    .attachToObject = true,
                },
                .anchorKind = game::FxInvocationAnchorKind::BonePosition,
                .primary = *anchor,
                .boneName = container::String{bone},
                .boneNameIsPrefix = ordinal != 0,
                .boneNameSequenceOrdinal = ordinal,
                .boneNamePrefixFallsBackToBare = prefixFallback,
                .attachmentGroup = particleGroup,
            }));
        };
        if (!event.outerParticleSystem.empty()) {
            for (uint32_t index = 0; index < outerCount; ++index) {
                emitAttachedParticle(
                    event.outerParticleSystem, event.outerBonePrefix,
                    index + 1u, true);
            }
        }
        emitAttachedParticle(event.connectorFlare,
                             event.connectorBone, 0, false);
        emitAttachedParticle(event.laserBaseParticleSystem,
                             event.fireBone, 0, false);

        if (!event.connectorLaser.empty() &&
            !event.outerBonePrefix.empty()) {
            for (uint32_t index = 0; index < outerCount; ++index) {
                const uint64_t connectorIdentity = particleUplinkKey(
                    event.object, event.authoredOrder,
                    0x434f4e4e00000000ull + index);
                static_cast<void>(m_publication.emitFxInvocationEvent({
                    .directBeam = game::FxDirectBeamRequest{
                        .objectTemplate = event.connectorLaser,
                        .control = game::FxDirectBeamControl::Begin,
                        .beamIdentity = connectorIdentity,
                    },
                    .anchorKind = game::FxInvocationAnchorKind::BonePosition,
                    .primary = *anchor,
                    .secondary = *anchor,
                    .boneName = event.outerBonePrefix,
                    .boneNameIsPrefix = true,
                    .boneNameSequenceOrdinal = index + 1u,
                    .boneNamePrefixFallsBackToBare = true,
                    .secondaryBoneName = event.connectorBone,
                }));
            }
        }
    }
    for (ObjectParticleUplinkBeamEvent& event :
         m_world.m_objectSimulation.takeParticleUplinkBeamEvents()) {
        const bool end = event.control ==
            ObjectParticleUplinkBeamControl::End;
        const game::FxDirectBeamControl control = end
            ? game::FxDirectBeamControl::End
            : event.control == ObjectParticleUplinkBeamControl::Begin
                ? game::FxDirectBeamControl::Begin
                : game::FxDirectBeamControl::Update;
        game::FxInvocationAnchor primary{
            .object = event.object,
            .position = {event.sourcePosition.x.to_float(),
                         event.sourcePosition.y.to_float(),
                         event.sourcePosition.z.to_float()},
        };
        game::FxInvocationAnchor secondary{
            .position = {event.targetPosition.x.to_float(),
                         event.targetPosition.y.to_float(),
                         event.targetPosition.z.to_float()},
        };
        const bool groundToOrbit = event.lane ==
            ObjectParticleUplinkBeamLane::GroundToOrbit &&
            !event.sourceBone.empty();
        container::String beamBone = std::move(event.sourceBone);
        if (groundToOrbit) {
            secondary.object = event.object;
        }
        game::FxInvocationEvent invocation{
            .directBeam = game::FxDirectBeamRequest{
                .objectTemplate = end ? container::String{}
                                      : std::move(event.beamTemplate),
                .control = control,
                .beamIdentity = event.identity,
                .sizeDeltaFrames = event.control ==
                        ObjectParticleUplinkBeamControl::Begin
                    ? static_cast<int32_t>(std::min<uint32_t>(
                          event.widthGrowFrames, INT32_MAX))
                    : event.control ==
                            ObjectParticleUplinkBeamControl::BeginDecay
                        ? -static_cast<int32_t>(std::min<uint32_t>(
                              event.widthGrowFrames, INT32_MAX))
                        : 0,
                .decayFrames = event.control ==
                        ObjectParticleUplinkBeamControl::BeginDecay
                    ? event.widthGrowFrames : 0,
            },
            .anchorKind = beamBone.empty()
                ? game::FxInvocationAnchorKind::WorldPosition
                : game::FxInvocationAnchorKind::BonePosition,
            .primary = primary,
            .secondary = end
                ? std::optional<game::FxInvocationAnchor>{}
                : std::optional<game::FxInvocationAnchor>{secondary},
            .boneName = end ? container::String{}
                            : beamBone,
            .secondaryBoneName = end || !groundToOrbit
                ? container::String{} : beamBone,
            .secondaryWorldOffset = end || !groundToOrbit
                ? math::vec3{} : math::vec3{0.0f, 0.0f, 3500.0f},
        };
        if (end) invocation.anchorKind =
            game::FxInvocationAnchorKind::WorldPosition;
        static_cast<void>(m_publication.emitFxInvocationEvent(std::move(invocation)));
    }
    for (ObjectParticleUplinkFxEvent& event :
         m_world.m_objectSimulation.takeParticleUplinkFxEvents()) {
        if (event.fxList.empty()) continue;
        game::FxInvocationAnchor anchor{
            .object = event.object,
            .position = {event.position.x.to_float(),
                         event.position.y.to_float(),
                         event.position.z.to_float()},
        };
        if (!event.boneName.empty()) {
            if (const std::optional<game::FxInvocationAnchor> objectAnchor =
                    session_fx::snapshotAnchor(
                        m_world.m_registry, m_world.m_objects, event.object)) {
                anchor = *objectAnchor;
            }
        }
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .fxListName = std::move(event.fxList),
            .anchorKind = event.boneName.empty()
                ? game::FxInvocationAnchorKind::WorldPosition
                : game::FxInvocationAnchorKind::BonePosition,
            .primary = anchor,
            .boneName = std::move(event.boneName),
            .inheritResolvedAnchorOrientation = false,
        }));
    }
    for (ObjectParticleUplinkScorchEvent& event :
         m_world.m_objectSimulation.takeParticleUplinkScorchEvents()) {
        const math::vec3 position{
            event.position.x.to_float(), event.position.y.to_float(),
            event.position.z.to_float()};
        if (event.radius > math::q32_32{}) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .directScorch = game::FxDirectScorchRequest{
                    .type = game::FxDirectScorchType::Random,
                    .radius = event.radius.to_float(),
                },
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = game::FxInvocationAnchor{
                    .object = event.object,
                    .position = position,
                },
            }));
        }
        if (!event.groundHitFx.empty()) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .fxListName = std::move(event.groundHitFx),
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = game::FxInvocationAnchor{
                    .object = event.object,
                    .position = position,
                },
            }));
        }
    }
}

} // namespace engine::detail
