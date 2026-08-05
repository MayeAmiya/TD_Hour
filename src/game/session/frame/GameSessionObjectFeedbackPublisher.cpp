#include "game/session/frame/GameSessionObjectFeedbackPublisher.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/frame/GameSessionFxAnchorSnapshot.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/simulation/combat/ObjectWeaponBonusUpdate.h"
#include "game/object/simulation/economy/ObjectAutoDeposit.h"
#include "game/object/simulation/runtime/ObjectDeathEvents.h"
#include "game/object/simulation/runtime/ObjectMovementEvents.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/player/FactionTemplate.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

constexpr size_t kMaximumObjectFeedbackEntries = 256u;

[[nodiscard]] bool hasFeedbackObjectKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf kind) noexcept {
    return kinds && game::objectHasKind(kinds->mask, kind);
}

[[nodiscard]] uint64_t saturatingTickAdd(
    uint64_t value, uint64_t delta) noexcept {
    return value > std::numeric_limits<uint64_t>::max() - delta
        ? std::numeric_limits<uint64_t>::max() : value + delta;
}

template <typename Event>
void appendBoundedFeedback(container::Vector<Event>& destination,
                           Event event) {
    if (destination.size() >= kMaximumObjectFeedbackEntries)
        destination.erase(destination.begin());
    destination.push_back(std::move(event));
}

[[nodiscard]] uint64_t mixFeedbackSeed(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] float signedFeedbackUnit(uint64_t seed) noexcept {
    constexpr double denominator = static_cast<double>(uint32_t{0x00ffffffu});
    const double normalized = static_cast<double>(
        static_cast<uint32_t>(mixFeedbackSeed(seed)) & 0x00ffffffu) /
        denominator;
    return static_cast<float>(normalized * 2.0 - 1.0);
}

// Acknowledgement and task speech belongs to the local player only. Reading
// the owner here keeps that gate on the presentation side: nothing about the
// authoritative object changes, and a spectator or replay viewer with no local
// player simply hears no unit speech rather than hearing everyone's.
[[nodiscard]] bool objectOwnedByLocalPlayer(
    const ecs::registry& registry,
    const PlayerRegistry& players,
    ecs::entity entity) noexcept {
    const PlayerState* viewer = players.localPlayer();
    if (!viewer) return false;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, entity);
    return owner && owner->player == viewer->id;
}

[[nodiscard]] bool objectLogicallyVisibleForFeedback(
    const ecs::registry& registry,
    const PlayerRegistry& players,
    const ObjectLifecycle& objects,
    ecs::entity sourceEntity) noexcept {
    ecs::entity visibleEntity = sourceEntity;
    for (uint32_t depth = 0; depth < 8u; ++depth) {
        const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(registry, visibleEntity);
        if (!contained || !contained->container) break;
        const std::optional<ecs::entity> outer =
            objects.entityFromIdIncludingPending(contained->container);
        if (!outer || *outer == visibleEntity) break;
        visibleEntity = *outer;
    }
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, visibleEntity);
    if (hasFeedbackObjectKind(kinds, game::ObjectKindOf::Disguiser))
        return true;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, visibleEntity);
    const bool hiddenStealth = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Stealthed)) &&
        !status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Detected));
    if (!hiddenStealth) return true;
    const PlayerState* viewer = players.localPlayer();
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, visibleEntity);
    return !viewer || viewer->life != PlayerLifeState::Active || !owner ||
        players.relationship(viewer->id, owner->player) ==
            PlayerRelationship::Allies;
}

[[nodiscard]] uint64_t floatingTextFadeFrames(
    uint8_t initialAlpha, float vanishPerSecond,
    uint32_t framesPerSecond) noexcept {
    if (!(vanishPerSecond > 0.0f) || !std::isfinite(vanishPerSecond))
        return std::numeric_limits<uint64_t>::max();
    uint32_t alpha = initialAlpha;
    const float perFrame = vanishPerSecond /
        static_cast<float>(std::max(1u, framesPerSecond));
    for (uint64_t frame = 1; frame <= 4096u; ++frame) {
        const uint32_t amount = static_cast<uint32_t>(std::max(
            0.0f, std::round(static_cast<float>(frame) * perFrame)));
        alpha = amount >= alpha ? 0u : alpha - amount;
        if (alpha == 0) return frame;
    }
    return 4096u;
}

} // namespace

void GameSessionObjectFeedbackPublisher::publish() {
    const uint64_t confirmedTick = m_presentation.m_confirmedTick;
    publishWeaponBonusFeedback(
        m_world.m_objectSimulation.takeWeaponBonusUpdateEvents());
    std::erase_if(m_presentation.m_objectWorldAnimations,
        [confirmedTick](const ObjectWorldAnimationPresentationEvent& event) {
            return confirmedTick >= event.expireTick;
        });
    std::erase_if(m_presentation.m_objectFloatingTexts,
        [confirmedTick](const ObjectFloatingTextPresentationEvent& event) {
            return confirmedTick >= event.expireTick;
        });
    for (auto flash = m_presentation.m_objectSelectionFlashes.begin();
         flash != m_presentation.m_objectSelectionFlashes.end();) {
        if (confirmedTick >= flash->second.expireTick)
            flash = m_presentation.m_objectSelectionFlashes.erase(flash);
        else
            ++flash;
    }

    publishDeathAudio(m_world.m_objectSimulation.takeDeathEvents());
    container::Vector<ObjectExperienceEvent> experienceEvents =
        m_world.m_objectSimulation.takeExperienceEvents();
    bool feedbackChanged = false;
    const RenderObjectFeedbackGameData& feedbackSettings =
        m_presentation.m_renderGameDataSettings.visual.objectFeedback;
    const uint32_t logicFramesPerSecond = static_cast<uint32_t>(
        std::max(1, m_content.m_startInfo.gameSpeedFPS));
    const bool drawIconUi =
        m_presentation.m_scriptClientOptions.drawIconUiEnabled();
    for (const ObjectExperienceEvent& event : experienceEvents) {
        if (event.kind != ObjectExperienceEventKind::VeterancyChanged ||
            !event.provideFeedback ||
            event.currentLevel <= event.previousLevel || !drawIconUi) {
            continue;
        }
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromIdIncludingPending(event.object);
        if (!entity) continue;
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity);
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        if (!transform ||
            hasFeedbackObjectKind(kinds, game::ObjectKindOf::IgnoredInGui) ||
            !objectLogicallyVisibleForFeedback(
                m_world.m_registry, m_content.m_players,
                m_world.m_objects, *entity)) {
            continue;
        }
        if (!feedbackSettings.levelGainAnimationName.empty() &&
            feedbackSettings.levelGainAnimationDisplaySeconds > 0.0f) {
            const uint64_t durationFrames = std::max<uint64_t>(
                1u, static_cast<uint64_t>(
                    feedbackSettings.levelGainAnimationDisplaySeconds *
                    static_cast<float>(logicFramesPerSecond)));
            appendBoundedFeedback(
                m_presentation.m_objectWorldAnimations,
                ObjectWorldAnimationPresentationEvent{
                    .identity = nextFeedbackIdentity(),
                    .object = event.object,
                    .worldAnchor = {transform->x, transform->y, transform->z},
                    .animationName = feedbackSettings.levelGainAnimationName,
                    .startTick = confirmedTick,
                    .expireTick = saturatingTickAdd(
                        confirmedTick, durationFrames),
                    .logicFramesPerSecond = logicFramesPerSecond,
                    .zRisePerSecond =
                        feedbackSettings.levelGainAnimationZRisePerSecond,
                });
            feedbackChanged = true;
        }
        // ActiveBody owns promotion feedback on the promoted object, not as
        // one global "unit promoted" sound.  The template cue wins for the
        // reached rank; the configured global cue remains only the fallback
        // for legacy/default content that authored no per-object event.
        container::StringView promotionCue;
        if (type && type->archetype) {
            const game::ThingTemplate& templateData =
                type->archetype->templateData;
            switch (event.currentLevel) {
            case game::ObjectVeterancyLevel::Veteran:
                promotionCue = templateData.soundPromotedVeteran;
                break;
            case game::ObjectVeterancyLevel::Elite:
                promotionCue = templateData.soundPromotedElite;
                break;
            case game::ObjectVeterancyLevel::Heroic:
                promotionCue = templateData.soundPromotedHero;
                break;
            case game::ObjectVeterancyLevel::Regular:
                break;
            }
        }
        if (!promotionCue.empty()) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = container::String{promotionCue},
                .emitter = event.object,
                .owner = event.object,
            }));
        } else if (!feedbackSettings.unitPromotedAudioEvent.empty()) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = feedbackSettings.unitPromotedAudioEvent,
                .emitter = event.object,
                .owner = event.object,
            }));
        }
    }

    container::Vector<ObjectAutoDepositEvent> autoDepositEvents =
        m_world.m_objectSimulation.takeAutoDepositEvents();
    for (const ObjectAutoDepositEvent& event : autoDepositEvents) {
        if (event.amount <= 0 || !drawIconUi) continue;
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromIdIncludingPending(event.object);
        if (!entity) continue;
        if (event.kind == ObjectAutoDepositEventKind::PeriodicIncome &&
            !objectLogicallyVisibleForFeedback(
                m_world.m_registry, m_content.m_players,
                m_world.m_objects, *entity)) {
            continue;
        }
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        const PlayerState* player = m_content.m_players.get(event.player);
        if (!transform || !player || !m_content.m_ruleset) continue;
        math::vec3 anchor{transform->x, transform->y, transform->z + 10.0f};
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *entity);
        if (geometry &&
            hasFeedbackObjectKind(kinds, game::ObjectKindOf::Structure)) {
            const uint64_t seed =
                static_cast<uint64_t>(event.object.value) |
                (static_cast<uint64_t>(event.authoredOrder) << 32u);
            anchor[0] += signedFeedbackUnit(seed ^ event.confirmedTick) *
                geometry->majorRadiusFixed.to_float() * 0.3f;
            anchor[1] += signedFeedbackUnit(
                seed ^ event.confirmedTick ^ 0xa5a5a5a5a5a5a5a5ull) *
                geometry->minorRadiusFixed.to_float() * 0.3f;
        }
        const PlayerRgbColor playerColor = resolvePlayerPresentationColor(
            *player, *m_content.m_ruleset);
        constexpr uint8_t kFloatingTextAlpha = 230u;
        const uint32_t color =
            (static_cast<uint32_t>(kFloatingTextAlpha) << 24u) |
            (static_cast<uint32_t>(playerColor.red) << 16u) |
            (static_cast<uint32_t>(playerColor.green) << 8u) |
            static_cast<uint32_t>(playerColor.blue);
        uint64_t timeoutFrames =
            feedbackSettings.floatingTextTimeoutMilliseconds == 0u
                ? std::max<uint64_t>(1u, logicFramesPerSecond / 3u)
                : (static_cast<uint64_t>(
                       feedbackSettings.floatingTextTimeoutMilliseconds) *
                       logicFramesPerSecond + 999u) / 1000u;
        timeoutFrames = std::max<uint64_t>(1u, timeoutFrames);
        const uint64_t timeoutTick =
            saturatingTickAdd(confirmedTick, timeoutFrames);
        const uint64_t fadeFrames = floatingTextFadeFrames(
            kFloatingTextAlpha, feedbackSettings.floatingTextVanishPerSecond,
            logicFramesPerSecond);
        const uint64_t expireTick =
            fadeFrames == std::numeric_limits<uint64_t>::max()
                ? std::numeric_limits<uint64_t>::max()
                : saturatingTickAdd(timeoutTick, fadeFrames);
        appendBoundedFeedback(
            m_presentation.m_objectFloatingTexts,
            ObjectFloatingTextPresentationEvent{
                .identity = nextFeedbackIdentity(),
                .object = event.object,
                .worldAnchor = anchor,
                .amount = event.amount,
                .color = color,
                .startTick = confirmedTick,
                .timeoutTick = timeoutTick,
                .expireTick = expireTick,
                .logicFramesPerSecond = logicFramesPerSecond,
                .moveUpPerSecond =
                    feedbackSettings.floatingTextMoveUpPerSecond,
                .vanishPerSecond =
                    feedbackSettings.floatingTextVanishPerSecond,
            });
        feedbackChanged = true;
    }
    if (feedbackChanged &&
        m_presentation.m_scriptPresentationSequence !=
            std::numeric_limits<uint64_t>::max()) {
        ++m_presentation.m_scriptPresentationSequence;
    }

    publishAirfieldFeedback(
        m_world.m_objectSimulation.takeAirfieldEvents());
    publishMovementAudio(m_world.m_objectSimulation.takeMovementEvents());
    // The countermeasure event records themselves carry no authored audio or
    // FX.  The authoritative diversion/flare spawn has already materialized
    // as ordinary projectile/object state and therefore reaches the renderer
    // through the entity snapshot.  Drain this simulation-local ledger here
    // rather than creating an unconsumed diagnostic presentation channel.
    static_cast<void>(m_world.m_objectSimulation.takeCountermeasureEvents());
    for (ObjectCrushDieEvent& event :
         m_world.m_objectSimulation.takeCrushDieEvents()) {
        if (!event.audioEvent) continue;
        static_cast<void>(m_publication.emitAudioEvent({
            .eventName = *event.audioEvent,
            .emitter = event.object,
            .owner = event.object,
            .position = math::vec3{
                event.position.x.to_float(),
                event.position.y.to_float(),
                event.position.z.to_float()},
        }));
    }
}

uint64_t GameSessionObjectFeedbackPublisher::nextFeedbackIdentity() noexcept {
    if (m_presentation.m_objectFeedbackOrdinal !=
        std::numeric_limits<uint64_t>::max()) {
        ++m_presentation.m_objectFeedbackOrdinal;
    }
    if (m_presentation.m_objectFeedbackOrdinal == 0)
        m_presentation.m_objectFeedbackOrdinal = 1;
    return m_presentation.m_objectFeedbackOrdinal;
}

void GameSessionObjectFeedbackPublisher::publishWeaponBonusFeedback(
    container::Vector<ObjectWeaponBonusUpdateEvent> events) {
    // ObjectWeaponBonusComponent is consumed by the object-UI extraction and
    // ObjectTemporaryWeaponBonusComponent by the FRENZY tint extraction. The
    // simulation condition writer already marks the entity RenderExtraction
    // dirty; advancing the presentation sequence additionally prevents a
    // same-frame overlay cache from keeping the former value.
    bool visibleStateChanged = false;
    for (const ObjectWeaponBonusUpdateEvent& event : events) {
        if (!event.target || event.confirmedTick != m_presentation.m_confirmedTick)
            continue;
        const std::optional<ecs::entity> target =
            m_world.m_objects.entityFromIdIncludingPending(event.target);
        if (!target) continue;
        visibleStateChanged =
            ecs::try_get<ObjectWeaponBonusComponent>(m_world.m_registry,
                                                      *target) != nullptr ||
            ecs::try_get<ObjectTemporaryWeaponBonusComponent>(
                m_world.m_registry, *target) != nullptr;
        if (visibleStateChanged) break;
    }
    if (visibleStateChanged &&
        m_presentation.m_scriptPresentationSequence !=
            std::numeric_limits<uint64_t>::max()) {
        ++m_presentation.m_scriptPresentationSequence;
    }
}

void GameSessionObjectFeedbackPublisher::publishAirfieldFeedback(
    container::Vector<ObjectAirfieldEvent> events) {
    for (ObjectAirfieldEvent& event : events) {
        if (event.kind == ObjectAirfieldEventKind::AfterburnerLoopStarted ||
            event.kind == ObjectAirfieldEventKind::AfterburnerLoopStopped ||
            event.kind == ObjectAirfieldEventKind::JetLowFuel ||
            event.kind == ObjectAirfieldEventKind::SpectreHowitzerFired) {
            const std::optional<ecs::entity> source =
                m_world.m_objects.entityFromId(event.object);
            const ThingTemplateComponent* type = source
                ? ecs::try_get<ThingTemplateComponent>(m_world.m_registry,
                                                       *source)
                : nullptr;
            if (!type || !type->archetype) continue;
            const game::ThingTemplate& recipe =
                type->archetype->templateData;
            if (event.kind == ObjectAirfieldEventKind::SpectreHowitzerFired ||
                event.kind == ObjectAirfieldEventKind::JetLowFuel) {
                const container::StringView cue =
                    recipe.perUnitSound(event.kind ==
                        ObjectAirfieldEventKind::SpectreHowitzerFired
                        ? "HowitzerFire" : "VoiceLowFuel");
                // VoiceLowFuel is an acknowledgement for the controlling
                // player, not a world sound emitted by every aircraft in the
                // match.  Keep the Spectre weapon sound globally audible.
                const bool localVoice =
                    event.kind != ObjectAirfieldEventKind::JetLowFuel ||
                    objectOwnedByLocalPlayer(
                        m_world.m_registry, m_content.m_players, *source);
                if (!cue.empty() && localVoice) {
                    static_cast<void>(m_publication.emitAudioEvent({
                        .eventName = container::String{cue},
                        .emitter = event.object,
                        .owner = event.object,
                    }));
                }
            } else {
                const container::StringView cue =
                    recipe.perUnitSound("Afterburner");
                if (!cue.empty()) {
                    static_cast<void>(m_publication.emitAudioControlEvent({
                        .kind = game::GameAudioControlKind::
                            SetObjectLoopingSoundEnabled,
                        .enabled = event.kind ==
                            ObjectAirfieldEventKind::AfterburnerLoopStarted,
                        .eventName = container::String{cue},
                        .object = event.object,
                    }));
                }
            }
            continue;
        }
        const std::optional<game::FxInvocationAnchor> objectAnchor =
            session_fx::snapshotAnchor(
                m_world.m_registry, m_world.m_objects, event.object);
        const bool initialAircraftDeath =
            event.kind == ObjectAirfieldEventKind::AircraftSlowDeathPhase &&
            (event.slowDeathPhase ==
                 ObjectAircraftSlowDeathPhase::InitialDeath ||
             event.slowDeathPhase ==
                 ObjectAircraftSlowDeathPhase::OnGroundDeath);
        if (initialAircraftDeath && !event.audio.empty() && objectAnchor) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = event.audio,
                .emitter = event.object,
                .owner = event.object,
                .position = objectAnchor->position,
            }));
        }
        if (event.particleSystem.empty()) continue;

        if (event.kind == ObjectAirfieldEventKind::FlightDeckCatapultFx &&
            objectAnchor) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .directParticle = game::FxDirectParticleRequest{
                    .particleSystemName = std::move(event.particleSystem),
                    .emitterCount = 1,
                    .attachToObject = true,
                },
                .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
                .primary = *objectAnchor,
                .boneName = std::move(event.boneName),
            }));
            continue;
        }
        if (event.kind == ObjectAirfieldEventKind::SpectreStrafeFx) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .directParticle = game::FxDirectParticleRequest{
                    .particleSystemName = std::move(event.particleSystem),
                    .emitterCount = 1,
                },
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = session_fx::worldAnchor({
                    event.worldPosition.x.to_float(),
                    event.worldPosition.y.to_float(),
                    event.worldPosition.z.to_float()}, event.object),
            }));
            continue;
        }
        if (!initialAircraftDeath || !objectAnchor) continue;
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .directParticle = game::FxDirectParticleRequest{
                .particleSystemName = std::move(event.particleSystem),
                .emitterCount = 1,
                .attachToObject = true,
            },
            .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
            .primary = *objectAnchor,
            .boneName = std::move(event.boneName),
            .attachmentLocalOffset = {
                event.localOffset.x.to_float(),
                event.localOffset.y.to_float(),
                event.localOffset.z.to_float()},
        }));
    }
}

void GameSessionObjectFeedbackPublisher::publishDeathAudio(
    container::Vector<ObjectDeathEvent> events) {
    for (const ObjectDeathEvent& event : events) {
        // Postamble is the fixed Object::onDie suffix that runs once after
        // every authored Die interface has closed, so it is the only edge that
        // fires exactly once per death. Keying off ReactionApplied would play
        // the cue once per matching Die module.
        if (event.kind != ObjectDeathEventKind::Postamble) continue;
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromIdIncludingPending(event.object);
        if (!entity) continue;
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        if (!type || !type->archetype) continue;
        const game::ThingTemplate& templateData =
            type->archetype->templateData;
        // SoundDie/SoundDieFire/SoundDieToxin are Generals-1 keys with zero
        // Zero Hour occurrences, so in practice this loop stays silent for ZH
        // content and only revives Generals-1 objects. The death-type variant
        // is preferred and does NOT fall back to the generic cue, matching the
        // authored intent that a burn-specific clip replaces the normal one.
        container::StringView eventName;
        switch (event.deathType) {
        case game::DeathType::BURNED:
            eventName = templateData.soundDieFire;
            break;
        case game::DeathType::POISONED:
        case game::DeathType::POISONED_BETA:
        case game::DeathType::POISONED_GAMMA:
            eventName = templateData.soundDieToxin;
            break;
        default:
            break;
        }
        if (eventName.empty())
            eventName = container::StringView{templateData.soundDie};
        if (eventName.empty()) continue;
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        static_cast<void>(m_publication.emitAudioEvent({
            .eventName = container::String{eventName},
            .emitter = event.object,
            .owner = event.object,
            .position = transform
                ? std::optional<math::vec3>{math::vec3{
                      transform->x, transform->y, transform->z}}
                : std::nullopt,
        }));
    }
}

void GameSessionObjectFeedbackPublisher::publishMovementAudio(
    container::Vector<ObjectMovementEvent> events) {
    for (const ObjectMovementEvent& event : events) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromIdIncludingPending(event.object);
        if (!entity) continue;
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        if (!type || !type->archetype) continue;
        const game::ThingTemplate& templateData =
            type->archetype->templateData;
        // AIInternalMoveToState selects exactly one cue at move start: a
        // one-shot Start cue when authored, otherwise the matching persistent
        // Loop fallback.  Pristine and damaged families are independent; a
        // damaged object must never fall through to pristine audio merely
        // because its damaged slot is blank.
        const container::StringView moveLoop{templateData.soundMoveLoop};
        const container::StringView damagedMoveLoop{
            templateData.soundMoveLoopDamaged};
        if (event.kind != ObjectMovementEventKind::Started) {
            // The body may cross a damage threshold while the loop is live,
            // so stop both authored identities rather than re-reading the
            // current health to guess the one selected at the start edge.
            if (!moveLoop.empty()) {
                static_cast<void>(m_publication.emitAudioControlEvent({
                    .kind = game::GameAudioControlKind::
                        SetObjectLoopingSoundEnabled,
                    .enabled = false,
                    .eventName = container::String{moveLoop},
                    .object = event.object,
                }));
            }
            if (!damagedMoveLoop.empty() && damagedMoveLoop != moveLoop) {
                static_cast<void>(m_publication.emitAudioControlEvent({
                    .kind = game::GameAudioControlKind::
                        SetObjectLoopingSoundEnabled,
                    .enabled = false,
                    .eventName = container::String{damagedMoveLoop},
                    .object = event.object,
                }));
            }
            // VoiceTaskComplete is a voice, not a world sound, so it is
            // restricted to the local player's own units the way RefCode
            // restricts acknowledgement speech. An enemy unit finishing a
            // path must not announce itself.
            const bool builderApproach =
                event.orderSource == static_cast<uint8_t>(
                    ObjectOrderSource::System) &&
                event.systemPurpose == static_cast<uint8_t>(
                    ObjectOrderSystemPurpose::Builder);
            if (event.kind == ObjectMovementEventKind::Completed &&
                !builderApproach &&
                !templateData.voiceTaskComplete.empty() &&
                objectOwnedByLocalPlayer(
                    m_world.m_registry, m_content.m_players, *entity)) {
                static_cast<void>(m_publication.emitAudioEvent({
                    .eventName = templateData.voiceTaskComplete,
                    .emitter = event.object,
                    .owner = event.object,
                }));
            }
            continue;
        }

        // This is the Body state frozen by the Movement Started transition,
        // exactly when AIInternalMoveToState selects its sound family.  A
        // later same-frame Damage/Death update must not rewrite the chosen
        // start cue while this journal is being projected.
        const bool damaged = event.damagedAtStart;
        const container::StringView startName = damaged
            ? templateData.resolvedSoundMoveStartDamaged()
            : templateData.resolvedSoundMoveStart();
        const container::StringView loopName = damaged
            ? damagedMoveLoop : moveLoop;
        if (!startName.empty()) {
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = container::String{startName},
                .emitter = event.object,
                .owner = event.object,
                .position = transform
                    ? std::optional<math::vec3>{math::vec3{
                          transform->x, transform->y, transform->z}}
                    : std::nullopt,
            }));
        }
        // A loop is only the fallback when the corresponding Start cue is
        // absent.  Starting both was the source of duplicated/mismatched
        // vehicle movement sound after a damage-state transition.
        if (startName.empty() && !loopName.empty()) {
            static_cast<void>(m_publication.emitAudioControlEvent({
                .kind = game::GameAudioControlKind::
                    SetObjectLoopingSoundEnabled,
                .enabled = true,
                .eventName = container::String{loopName},
                .object = event.object,
            }));
        }
    }
}

} // namespace engine
