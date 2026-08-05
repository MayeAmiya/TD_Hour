#pragma once

#include "game/session/state/GameSessionDomainState.h"
#include "game/session/integration/GameSessionScriptPresentationPort.h"
#include "game/script/contracts/ScriptPresentationLimits.h"
#include "core/config/GlobalData.h"
#include "core/container/string_utils.h"
#include "game/base/GameTacticalCamera.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/render/VisualAnimationState.h"
#include "presentation/contracts/PresentationPlayerAudience.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "debug/debug.h"

namespace engine::game_session_presentation_detail {

[[nodiscard]] inline uint32_t saturatingFrameCount(uint32_t current,
                                             uint64_t added) noexcept {
    constexpr uint32_t Maximum = std::numeric_limits<uint32_t>::max();
    return added >= static_cast<uint64_t>(Maximum - current)
        ? Maximum
        : current + static_cast<uint32_t>(added);
}

inline constexpr size_t kMaximumPendingScriptScreenShakeImpulses = 256;
inline constexpr size_t kMaximumPendingScriptLocalizedCameraShakeImpulses = 256;
inline constexpr size_t kMaximumPendingScriptBlackAndWhiteCommands = 256;
inline constexpr size_t kMaximumPendingScriptMotionBlurCommands = 256;

// RefCode AudioManager evaluates the source player's directed relationship
// to the local/observed player.  Freeze that result at extraction so all
// presentation consumers can carry a plain value instead of reading player
// state after the confirmed frame.
[[nodiscard]] inline presentation::PlayerAudience freezePlayerAudience(
    const PlayerRegistry& players, const PlayerState* listener,
    PlayerId sourcePlayer) noexcept {
    presentation::PlayerAudience audience;
    if (!sourcePlayer) return audience;
    audience.sourcePlayer = sourcePlayer.value;
    if (!listener || !listener->id) return audience;
    if (sourcePlayer == listener->id) {
        audience.relation = presentation::PlayerAudienceRelation::Self;
        return audience;
    }
    switch (players.relationship(sourcePlayer, listener->id)) {
    case PlayerRelationship::Allies:
        audience.relation = presentation::PlayerAudienceRelation::Ally;
        break;
    case PlayerRelationship::Enemies:
        audience.relation = presentation::PlayerAudienceRelation::Enemy;
        break;
    case PlayerRelationship::Neutral:
        audience.relation = presentation::PlayerAudienceRelation::Neutral;
        break;
    }
    return audience;
}

inline void advanceScreenFadeState(
    script::ScriptScreenFadePresentationState& fade) noexcept {
    if (!fade.active) return;
    if (fade.currentFrame < std::numeric_limits<int64_t>::max())
        ++fade.currentFrame;
    int64_t frame = fade.currentFrame;
    const int64_t increase = static_cast<int64_t>(fade.increaseFrames);
    const int64_t hold = static_cast<int64_t>(fade.holdFrames);
    const int64_t decrease = static_cast<int64_t>(fade.decreaseFrames);

    if (frame <= increase) {
        if (increase != 0) {
            const float factor = static_cast<float>(fade.currentFrame) /
                                 static_cast<float>(increase);
            fade.currentIntensity = fade.minimumIntensity + factor *
                (fade.maximumIntensity - fade.minimumIntensity);
        }
        return;
    }
    frame -= increase;
    if (frame <= hold) {
        fade.currentIntensity = fade.maximumIntensity;
        return;
    }
    frame -= hold;
    if (frame <= decrease) {
        int64_t divisor = decrease + 1;
        if (divisor == 0) divisor = 1;
        const float factor = static_cast<float>(frame) /
                             static_cast<float>(divisor);
        fade.currentIntensity = fade.maximumIntensity + factor *
            (fade.minimumIntensity - fade.maximumIntensity);
        return;
    }
    fade.active = false;
}

[[nodiscard]] inline bool validScriptPresentationCompletionForSession(
    const script::ScriptPresentationCompletion& completion,
    uint64_t presentationEpoch) noexcept {
    return completion.kind !=
            script::ScriptPresentationCompletionKind::MusicLoop &&
        completion.stamp.presentationEpoch == presentationEpoch &&
        completion.stamp.sequence != 0 && !completion.name.empty() &&
        completion.name.size() <=
            script::kMaximumScriptPresentationNameLength &&
        completion.name.find('\0') == container::String::npos;
}

// Drawable::startAmbientSound selects the damaged/really-damaged/rubble
// template event before it reaches AudioEventRTS. Preserve its fallback:
// ordinary damage variants inherit the pristine sound when blank, whereas
// rubble deliberately stays silent without an explicit event.
[[nodiscard]] inline container::StringView ambientSoundFor(
    const game::ThingTemplate& templateData,
    ObjectBodyDamageState damageState) noexcept {
    switch (damageState) {
    case ObjectBodyDamageState::Damaged:
        return templateData.soundAmbientDamaged.empty()
            ? container::StringView{templateData.soundAmbient}
            : container::StringView{templateData.soundAmbientDamaged};
    case ObjectBodyDamageState::ReallyDamaged:
        return templateData.soundAmbientReallyDamaged.empty()
            ? container::StringView{templateData.soundAmbient}
            : container::StringView{templateData.soundAmbientReallyDamaged};
    case ObjectBodyDamageState::Rubble:
        return templateData.soundAmbientRubble;
    case ObjectBodyDamageState::Pristine:
    default:
        return templateData.soundAmbient;
    }
}

[[nodiscard]] inline container::StringView ambientSoundForObject(
    const ecs::registry& registry, ecs::entity entity,
    const game::ThingTemplate& templateData,
    ObjectBodyDamageState damageState) noexcept {
    const ObjectAmbientAudioOverrideComponent* overrideValue =
        ecs::try_get<ObjectAmbientAudioOverrideComponent>(registry, entity);
    if (overrideValue && overrideValue->eventName) {
        return *overrideValue->eventName;
    }
    return ambientSoundFor(templateData, damageState);
}

[[nodiscard]] inline game::GameAudioInstanceOverrides ambientOverridesForObject(
    const ecs::registry& registry, ecs::entity entity) {
    const ObjectAmbientAudioOverrideComponent* value =
        ecs::try_get<ObjectAmbientAudioOverrideComponent>(registry, entity);
    if (!value) return {};
    return {
        .looping = value->looping,
        .loopCount = value->loopCount,
        .minimumVolume = value->minimumVolume,
        .volume = value->volume,
        .minimumRange = value->minimumRange,
        .maximumRange = value->maximumRange,
        .priority = value->priority,
    };
}

[[nodiscard]] inline math::vec3 resolveScriptIndicatorColor(
    const script::ScriptObjectPresentationState& presentation,
    const ecs::registry& registry, const PlayerRegistry& players,
    const MultiplayerRuleset* ruleset,
    std::optional<ecs::entity> entity, ObjectId object) noexcept {
    if (const std::optional<math::vec3> custom =
            presentation.customIndicatorColor(object)) {
        return *custom;
    }
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(registry, *entity) : nullptr;
    const PlayerState* player = owner ? players.get(owner->player) : nullptr;
    if (!player || !ruleset) return {1.0f, 1.0f, 1.0f};
    const PlayerRgbColor color = resolvePlayerPresentationColor(
        *player, *ruleset);
    return {
        static_cast<float>(color.red) / 255.0f,
        static_cast<float>(color.green) / 255.0f,
        static_cast<float>(color.blue) / 255.0f,
    };
}

[[nodiscard]] inline bool isThisPlayerReference(container::StringView value) noexcept {
    return container::asciiEqualIgnoreCase(value, "ThisPlayer") ||
        container::asciiEqualIgnoreCase(value, "<This Player>");
}

[[nodiscard]] inline bool isLocalPlayerReference(container::StringView value) noexcept {
    return container::asciiEqualIgnoreCase(value, "LocalPlayer") ||
        container::asciiEqualIgnoreCase(value, "<Local Player>");
}

[[nodiscard]] inline bool isChallengeLocalPlayerReference(
    container::StringView value, GameMode mode) noexcept {
    // RefCode accepts ThePlayer as the local-player alias only in Generals
    // Challenge. Campaign maps may contain an authored player named ThePlayer.
    return mode == GameMode::Challenge &&
        container::asciiEqualIgnoreCase(value, "ThePlayer");
}

} // namespace engine::game_session_presentation_detail
