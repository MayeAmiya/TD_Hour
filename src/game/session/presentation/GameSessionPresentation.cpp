#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/presentation/GameSessionObjectAmbientAudioLifecycle.h"
#include "game/session/integration/GameSessionScriptPresentationPort.h"
#include "game/script/contracts/ScriptPresentationLimits.h"
#include "core/config/GlobalData.h"
#include "core/container/string_utils.h"
#include "game/base/GameTacticalCamera.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/render/VisualAnimationState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "debug/debug.h"
#include "game/session/presentation/GameSessionPresentationDetail.h"

namespace engine {
using namespace game_session_presentation_detail;

GameSessionObjectAmbientAudioLifecycle::
GameSessionObjectAmbientAudioLifecycle(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionGameplayPublicationPort publication) noexcept
    : m_content(content),
      m_world(world),
      m_presentation(presentation),
      m_publication(publication) {}

void GameSessionObjectAmbientAudioLifecycle::start(ObjectId object) {
    if (!object || !m_content.m_active) return;
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    if (!entity) return;
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(
            m_world.m_registry, *entity);
    if (!type || !type->archetype) return;
    ObjectBodyDamageState damageState = ObjectBodyDamageState::Pristine;
    if (const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(
                m_world.m_registry, *entity)) {
        damageState = objectBodyDamagePresentationState(
            m_world.m_registry, *entity, health->damageState);
    }
    auto [position, inserted] =
        m_presentation.m_objectAmbientAudio.try_emplace(
            object);
    ObjectAmbientAudioPresentationState& state = position->second;
    const ObjectAmbientAudioOverrideComponent* overrideValue =
        ecs::try_get<ObjectAmbientAudioOverrideComponent>(
            m_world.m_registry, *entity);
    if (inserted) {
        state.enabled = overrideValue && overrideValue->enabled
            ? *overrideValue->enabled : true;
        state.automaticEnabled =
            overrideValue && !overrideValue->enabled;
    }
    state.damageState = damageState;
    state.eventName = container::String(ambientSoundForObject(
        m_world.m_registry, *entity,
        type->archetype->templateData, damageState));
    if (overrideValue && overrideValue->eventName &&
        overrideValue->eventName->empty()) {
        state.enabled = false;
    }
    ++state.generation;
    if (state.generation == 0) state.generation = 1;
    if (!state.enabled || state.eventName.empty()) return;
    static_cast<void>(m_publication.emitAudioControlEvent({
        .kind = game::GameAudioControlKind::SetObjectAmbientSoundEnabled,
        .enabled = true,
        .automaticEnabled = state.automaticEnabled,
        .eventName = state.eventName,
        .object = object,
        .generation = state.generation,
        .instanceOverrides = ambientOverridesForObject(
            m_world.m_registry, *entity),
    }));
}

void GameSessionObjectAmbientAudioLifecycle::refresh(
    ObjectId object, ObjectBodyDamageState damageState) {
    if (!object || !m_content.m_active) return;
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    if (!entity) return;
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(
            m_world.m_registry, *entity);
    if (!type || !type->archetype) return;
    damageState = objectBodyDamagePresentationState(
        m_world.m_registry, *entity, damageState);
    auto [position, inserted] =
        m_presentation.m_objectAmbientAudio.try_emplace(
            object);
    ObjectAmbientAudioPresentationState& state = position->second;
    const ObjectAmbientAudioOverrideComponent* overrideValue =
        ecs::try_get<ObjectAmbientAudioOverrideComponent>(
            m_world.m_registry, *entity);
    if (inserted) {
        state.enabled = overrideValue && overrideValue->enabled
            ? *overrideValue->enabled : true;
        state.automaticEnabled =
            overrideValue && !overrideValue->enabled;
    }
    state.damageState = damageState;
    state.eventName = container::String(ambientSoundForObject(
        m_world.m_registry, *entity,
        type->archetype->templateData, damageState));
    if (overrideValue && overrideValue->eventName &&
        overrideValue->eventName->empty()) {
        state.enabled = false;
    }
    ++state.generation;
    if (state.generation == 0) state.generation = 1;
    // AudioSubsystem stops the previous emitter-owned ambient before
    // considering this replacement.  An empty rubble event is therefore a
    // typed stop rather than a missing-resource error.
    static_cast<void>(m_publication.emitAudioControlEvent({
        .kind = game::GameAudioControlKind::SetObjectAmbientSoundEnabled,
        .enabled = state.enabled && !state.eventName.empty(),
        .automaticEnabled = state.automaticEnabled,
        .eventName = state.eventName,
        .object = object,
        .generation = state.generation,
        .instanceOverrides = ambientOverridesForObject(
            m_world.m_registry, *entity),
    }));
}

void GameSessionObjectAmbientAudioLifecycle::stop(ObjectId object) {
    if (!object) return;
    const auto found =
        m_presentation.m_objectAmbientAudio.find(object);
    if (found ==
        m_presentation.m_objectAmbientAudio.end()) {
        return;
    }
    ++found->second.generation;
    if (found->second.generation == 0) found->second.generation = 1;
    static_cast<void>(m_publication.emitAudioControlEvent({
        .kind = game::GameAudioControlKind::SetObjectAmbientSoundEnabled,
        .enabled = false,
        .eventName = found->second.eventName,
        .object = object,
        .generation = found->second.generation,
    }));
    m_presentation.m_objectAmbientAudio.erase(found);
}

bool script::GameSessionScriptPresentationPort::setObjectAmbientAudioEnabled(
    ObjectId object, bool enabled) {
    if (!object || !m_content.m_active) return false;
    const std::optional<ecs::entity> entity = entityFromId(object);
    if (!entity) return true;
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(
            m_world.m_registry, *entity);
    if (!type || !type->archetype) return true;
    ObjectBodyDamageState damageState = ObjectBodyDamageState::Pristine;
    if (const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(
                m_world.m_registry, *entity)) {
        damageState = objectBodyDamagePresentationState(
            m_world.m_registry, *entity, health->damageState);
    }
    auto [position, inserted] =
        m_presentation.m_objectAmbientAudio.try_emplace(
            object);
    ObjectAmbientAudioPresentationState& state = position->second;
    state.enabled = enabled;
    state.automaticEnabled = false;
    state.damageState = damageState;
    state.eventName = container::String(ambientSoundForObject(
        m_world.m_registry, *entity,
        type->archetype->templateData, damageState));
    ++state.generation;
    if (state.generation == 0) state.generation = 1;
    if (state.eventName.empty() && enabled) return true;
    return emitAudioControlEvent({
        .kind = game::GameAudioControlKind::SetObjectAmbientSoundEnabled,
        .enabled = enabled,
        .eventName = state.eventName,
        .object = object,
        .generation = state.generation,
        .instanceOverrides = ambientOverridesForObject(
            m_world.m_registry, *entity),
    });
}

} // namespace engine
