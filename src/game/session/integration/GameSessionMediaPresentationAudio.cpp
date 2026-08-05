#include "game/session/integration/GameSessionMediaPresentationPort.h"

#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/simulation/structure/ObjectParticleUplinkCannon.h"
#include "game/script/contracts/ScriptPresentationLimits.h"
#include "game/script/presentation/ScriptPresentationCompletionLedger.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/presentation/GameSessionPresentationDetail.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine {

audio::AudioPresentationSnapshot
GameSessionMediaPresentationPort::takeAudio(uint64_t simulationFrame) {
    GameSessionContentStartState& content = *m_content;
    GameSessionWorldState& world = *m_world;
    GameSessionScriptPresentationState& presentation = *m_presentation;
    audio::AudioPresentationSnapshot snapshot;
    snapshot.sessionEpoch = presentation.m_audioJournal.presentationEpoch();
    snapshot.simulationFrame = simulationFrame;
    snapshot.simulationDeltaSeconds = 1.0f / static_cast<float>(std::max(
        1, content.m_startInfo.gameSpeedFPS));

    // Mirror AudioManager::shouldPlayLocally without giving the audio thread
    // a PlayerRegistry pointer.  Original code queries the owning player's
    // directed relation to the local/observed player; capture that exact
    // value while confirmed state is still available and publish only this
    // detached value contract downstream.
    const PlayerState* listener = content.m_players.localPlayer();
    const auto audienceFor = [&content, listener](PlayerId sourcePlayer) {
        return game_session_presentation_detail::freezePlayerAudience(
            content.m_players, listener, sourcePlayer);
    };

    // Object transforms are a complete latest-value liveness channel. Every
    // audio emitter gets a stable object-ID key; the sequencer owns no
    // registry reference after this function returns and can retire a bound
    // 3D voice when that key vanishes on a later extraction.
    const auto objects = ecs::view<const ObjectIdentityComponent, const TransformComponent>(
        world.m_registry);
    snapshot.emitters.reserve(objects.size_hint());
    for (const ecs::entity entity : objects) {
        const auto& identity = objects.template get<const ObjectIdentityComponent>(entity);
        const auto& transform = objects.template get<const TransformComponent>(entity);
        if (!identity.id) continue;
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(world.m_registry, entity);
        audio::AudioEmitterTransform audioTransform{
            .position = {transform.x, transform.y, transform.z},
            // Movement/physics velocity is not an ECS component yet. A
            // future extraction can fill this value without changing the
            // audio event or device API.
            .velocity = {},
        };
        snapshot.emitters.push_back({
            .emitterKey = identity.id.value,
            .transform = audioTransform,
            .audience = owner ? audienceFor(owner->player)
                              : audio::AudioAudience{},
        });
    }
    // ParticleUplink's annihilation loop belongs to the orbit-to-target beam,
    // not the cannon root. Publish that stable synthetic emitter alongside
    // ordinary ObjectId emitters; no audio handle enters ECS.
    const auto uplinks = ecs::view<
        const ObjectIdentityComponent,
        const ObjectParticleUplinkComponent>(world.m_registry);
    for (const ecs::entity entity : uplinks) {
        const ObjectParticleUplinkComponent& component =
            uplinks.template get<const ObjectParticleUplinkComponent>(entity);
        for (const ObjectParticleUplinkRuntime& runtime :
             component.instances) {
            if (!runtime.orbitalBeamAlive ||
                runtime.orbitalBeamIdentity == 0) {
                continue;
            }
            const audio::AudioEmitterTransform audioTransform{
                .position = {
                    runtime.currentTargetPosition.x.to_float(),
                    runtime.currentTargetPosition.y.to_float(),
                    runtime.currentTargetPosition.z.to_float() + 500.0f,
                },
                .velocity = {},
            };
            const uint64_t emitterKey = particleUplinkAudioEmitterKey(
                runtime.orbitalBeamIdentity);
            snapshot.emitters.push_back({
                .emitterKey = emitterKey,
                .transform = audioTransform,
            });
        }
    }
    // The snapshot already owns the complete emitter projection. Canonicalize
    // it once and use binary lookup for same-frame event positions instead of
    // allocating and filling a second full Transform HashMap every tick.
    std::sort(
        snapshot.emitters.begin(), snapshot.emitters.end(),
        [](const audio::AudioEmitterSnapshot& left,
           const audio::AudioEmitterSnapshot& right) {
            return left.emitterKey < right.emitterKey;
        });
    const auto findEmitter = [&snapshot](uint64_t key)
        -> const audio::AudioEmitterSnapshot* {
        const auto found = std::lower_bound(
            snapshot.emitters.begin(), snapshot.emitters.end(), key,
            [](const audio::AudioEmitterSnapshot& value, uint64_t candidate) {
                return value.emitterKey < candidate;
            });
        return found != snapshot.emitters.end() && found->emitterKey == key
            ? &*found : nullptr;
    };
    const auto audienceForObject = [&findEmitter](ObjectId object)
        -> audio::AudioAudience {
        if (!object) return {};
        const audio::AudioEmitterSnapshot* emitter = findEmitter(object.value);
        return emitter ? emitter->audience : audio::AudioAudience{};
    };

    container::Vector<game::GameAudioEvent> events =
        presentation.m_audioJournal.takeEvents();
    snapshot.events.reserve(events.size());
    for (game::GameAudioEvent& source : events) {
        if (source.eventName.empty() && !source.evaPolicy) continue;

        audio::AudioEventIntent intent;
        intent.eventName = std::move(source.eventName);
        intent.eventId = source.eventId;
        intent.confirmedFrame = source.confirmedFrame;
        intent.variationSeed = source.variationSeed;
        intent.volumeScale = source.volumeScale;
        intent.uninterruptible = source.uninterruptible;
        intent.logical = source.logical;
        intent.audience = source.sourcePlayer
            ? audienceFor(*source.sourcePlayer)
            : source.owner
                ? audienceForObject(*source.owner)
                : source.emitter
                    ? audienceForObject(*source.emitter)
                    : audio::AudioAudience{};
        if (source.position) intent.position = *source.position;

        if (source.emitter && *source.emitter) {
            intent.emitterKey = source.emitter->value;
            // A same-frame trigger should use the completed logic transform
            // immediately. Later presentation frames refresh it by key.
            if (const audio::AudioEmitterSnapshot* transform =
                    findEmitter(source.emitter->value)) {
                intent.position = transform->transform.position;
            }
        }
        if (source.owner && *source.owner) {
            intent.ownerKey = source.owner->value;
        } else if (source.emitter && *source.emitter) {
            // Object-attached Voice events use their owner object in original
            // GameSounds; this is the useful default for modern callers.
            intent.ownerKey = source.emitter->value;
        }

        snapshot.events.push_back({
            .intent = std::move(intent),
            .ambient = source.ambient,
            .eva = source.eva,
            .evaPolicy = source.evaPolicy,
        });
    }

    container::Vector<game::GameAudioControlEvent> controls =
        presentation.m_audioJournal.takeControlEvents();
    snapshot.controls.reserve(controls.size());
    for (game::GameAudioControlEvent& source : controls) {
        audio::AudioPresentationControlEvent control;
        switch (source.kind) {
        case game::GameAudioControlKind::SetMusicTrack:
            control.kind = audio::AudioPresentationControlKind::SetMusicTrack;
            break;
        case game::GameAudioControlKind::SetMusicVolume:
            control.kind = audio::AudioPresentationControlKind::SetMusicVolume;
            break;
        case game::GameAudioControlKind::SetAmbientPaused:
            control.kind = audio::AudioPresentationControlKind::SetAmbientPaused;
            break;
        case game::GameAudioControlKind::SetBackgroundSoundsPaused:
            control.kind = audio::AudioPresentationControlKind::SetBackgroundSoundsPaused;
            break;
        case game::GameAudioControlKind::SetSoundVolume:
            control.kind = audio::AudioPresentationControlKind::SetSoundVolume;
            break;
        case game::GameAudioControlKind::SetSpeechVolume:
            control.kind = audio::AudioPresentationControlKind::SetSpeechVolume;
            break;
        case game::GameAudioControlKind::SetEventVolumeOverride:
            control.kind = audio::AudioPresentationControlKind::SetEventVolumeOverride;
            break;
        case game::GameAudioControlKind::RestoreEventVolumeOverride:
            control.kind = audio::AudioPresentationControlKind::RestoreEventVolumeOverride;
            break;
        case game::GameAudioControlKind::RestoreAllEventVolumeOverrides:
            control.kind = audio::AudioPresentationControlKind::RestoreAllEventVolumeOverrides;
            break;
        case game::GameAudioControlKind::RemoveEvent:
            control.kind = audio::AudioPresentationControlKind::RemoveEvent;
            break;
        case game::GameAudioControlKind::RemoveDisabledEvents:
            control.kind = audio::AudioPresentationControlKind::RemoveDisabledEvents;
            break;
        case game::GameAudioControlKind::SetEvaEnabled:
            control.kind = audio::AudioPresentationControlKind::SetEvaEnabled;
            break;
        case game::GameAudioControlKind::SetObjectAmbientSoundEnabled:
            control.kind = audio::AudioPresentationControlKind::SetObjectAmbientSoundEnabled;
            break;
        case game::GameAudioControlKind::SetObjectLoopingSoundEnabled:
            control.kind = audio::AudioPresentationControlKind::SetObjectLoopingSoundEnabled;
            break;
        }
        control.trackName = std::move(source.trackName);
        control.fadeOut = source.fadeOut;
        control.fadeIn = source.fadeIn;
        control.paused = source.paused;
        control.enabled = source.enabled;
        control.automaticEnabled = source.automaticEnabled;
        control.eventName = std::move(source.eventName);
        control.emitterKey = source.emitterKeyOverride != 0
            ? source.emitterKeyOverride
            : source.object && *source.object ? source.object->value : 0;
        control.generation = source.generation;
        control.instanceOverrides = {
            .looping = source.instanceOverrides.looping,
            .loopCount = source.instanceOverrides.loopCount,
            .minimumVolume = source.instanceOverrides.minimumVolume,
            .volume = source.instanceOverrides.volume,
            .minimumRange = source.instanceOverrides.minimumRange,
            .maximumRange = source.instanceOverrides.maximumRange,
            .priority = source.instanceOverrides.priority,
        };
        control.volume = source.volume;
        control.eventId = source.eventId;
        control.confirmedFrame = source.confirmedFrame;
        control.audience = source.sourcePlayer
            ? audienceFor(*source.sourcePlayer)
            : source.object ? audienceForObject(*source.object)
                            : audio::AudioAudience{};
        snapshot.controls.push_back(std::move(control));
    }
    return snapshot;
}


size_t GameSessionMediaPresentationPort::admitAudioCompletions(
    container::Vector<audio::AudioNaturalCompletion> completions) {
    GameSessionContentStartState& content = *m_content;
    GameSessionScriptPresentationState& presentation = *m_presentation;
    const auto acceptsLocalCompletion = [&content]() noexcept {
        return content.m_active && !content.m_startInfo.network.enabled &&
            content.m_startInfo.mode != GameMode::Replay;
    };
    const auto admitMediaCompletion =
        [&presentation, &acceptsLocalCompletion](
            script::ScriptPresentationCompletionKind kind,
            container::String name, uint64_t audioEpoch,
            uint64_t confirmedFrame) {
        if ((kind != script::ScriptPresentationCompletionKind::Audio &&
             kind != script::ScriptPresentationCompletionKind::Speech) ||
            !acceptsLocalCompletion() || audioEpoch == 0 ||
            audioEpoch != presentation.m_audioJournal.presentationEpoch() ||
            confirmedFrame > presentation.m_confirmedTick) {
            return false;
        }
        ++presentation.m_scriptPresentationSequence;
        if (presentation.m_scriptPresentationSequence == 0) {
            presentation.m_scriptPresentationSequence = 1;
        }
        script::ScriptPresentationCompletion scriptCompletion{
            .kind = kind,
            .name = std::move(name),
            .stamp = {
                .presentationEpoch = presentation.m_scriptPresentationEpoch,
                .sequence = presentation.m_scriptPresentationSequence,
                .confirmedTick = confirmedFrame,
            },
        };
        if (!game_session_presentation_detail::
                validScriptPresentationCompletionForSession(
                    scriptCompletion,
                    presentation.m_scriptPresentationEpoch)) {
            return false;
        }
        presentation.m_pendingScriptPresentationCompletions.push_back(
            std::move(scriptCompletion));
        return true;
    };
    const auto admitMusicLoop =
        [&presentation, &acceptsLocalCompletion](
            container::StringView trackName, uint64_t audioEpoch) {
        if (!acceptsLocalCompletion() || audioEpoch == 0 ||
            audioEpoch != presentation.m_audioJournal.presentationEpoch()) {
            return false;
        }
        const script::ScriptMusicLoopState& active =
            presentation.m_scriptPresentationCompletions.musicLoopState();
        if (!active.active || active.trackName != trackName ||
            trackName.empty()) {
            return false;
        }
        presentation.m_pendingScriptMusicLoops.emplace_back(trackName);
        return true;
    };

    size_t admitted = 0;
    for (audio::AudioNaturalCompletion& completion : completions) {
        if (completion.sessionEpoch == 0 ||
            completion.sessionEpoch !=
                presentation.m_audioJournal.presentationEpoch()) {
            continue;
        }
        bool accepted = false;
        switch (completion.kind) {
        case audio::AudioNaturalCompletionKind::Audio:
            accepted = admitMediaCompletion(
                script::ScriptPresentationCompletionKind::Audio,
                std::move(completion.eventName), completion.sessionEpoch,
                completion.confirmedFrame);
            break;
        case audio::AudioNaturalCompletionKind::Speech:
            accepted = admitMediaCompletion(
                script::ScriptPresentationCompletionKind::Speech,
                std::move(completion.eventName), completion.sessionEpoch,
                completion.confirmedFrame);
            break;
        case audio::AudioNaturalCompletionKind::MusicLoop:
            accepted = admitMusicLoop(
                completion.eventName, completion.sessionEpoch);
            break;
        }
        if (accepted) ++admitted;
    }
    return admitted;
}

} // namespace engine
