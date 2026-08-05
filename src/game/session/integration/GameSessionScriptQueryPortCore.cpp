#include "game/session/integration/GameSessionScriptQueryPort.h"

#include "core/container/string_utils.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/player/PlayerRegistry.h"
#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>

namespace engine::script {

container::StringView
GameSessionScriptQueryPort::effectiveObjectCommandBarButton(
    ObjectId object, size_t slot) const {
    if (slot >= game::COMMAND_SET_SLOT_COUNT) return {};
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(object);
    if (!entity) return {};
    const container::StringView commandSetName =
        effectiveObjectCommandSetName(m_world.m_registry, *entity);
    const game::CommandSetTemplate* commandSet =
        m_content.m_contentSnapshot.findCommandSet(commandSetName);
    if (!commandSet) return {};
    return m_presentation.m_scriptCommandBarOverrides.effectiveButtonName(
        commandSet->name, slot, commandSet->commands[slot]);
}

bool GameSessionScriptQueryPort::seesAny(
    ObjectId source, const ObjectSightQuery& query) const {
    if (!source ||
        (query.targetPlayer && !m_content.m_players.get(query.targetPlayer))) {
        return false;
    }
    const std::optional<ecs::entity> sourceEntity =
        m_world.m_objects.entityFromId(source);
    if (!sourceEntity) return false;
    const TransformComponent* sourceTransform =
        ecs::try_get<TransformComponent>(m_world.m_registry, *sourceEntity);
    const ThingTemplateComponent* sourceType =
        ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *sourceEntity);
    if (!sourceTransform || !sourceType || !sourceType->archetype) return false;
    if (query.requireAliveSource) {
        const ObjectHealthComponent* sourceHealth =
            ecs::try_get<ObjectHealthComponent>(
                m_world.m_registry, *sourceEntity);
        if (sourceHealth && sourceHealth->effectivelyDead) return false;
    }

    math::q32_32 sight = effectiveObjectVisionRangeFixed(
        m_world.m_registry, *sourceEntity);
    if (sight < math::q32_32{}) return false;
    if (const ObjectBattlePlanEffectComponent* battlePlan =
            ecs::try_get<ObjectBattlePlanEffectComponent>(
                m_world.m_registry, *sourceEntity)) {
        sight *= battlePlan->sightRangeScalar;
    }
    if (sight < math::q32_32{}) return false;

    const ObjectMapStatusComponent* sourceMap =
        ecs::try_get<ObjectMapStatusComponent>(
            m_world.m_registry, *sourceEntity);
    const bool sourceOffMap = sourceMap && sourceMap->offMap;
    const math::q32_32 sightSquared = sight * sight;
    const LogicFixedVec3 center = readAuthoritativeObjectPosition(
        m_world.m_registry, *sourceEntity, *sourceTransform);

    auto& candidates = m_presentation.m_scriptSightQueryScratch;
    m_world.m_spatialIndex.queryRadiusFixed(center, sight, candidates);
    for (const ObjectId candidate : candidates) {
        if (!candidate || candidate == source) continue;
        const std::optional<ecs::entity> candidateEntity =
            m_world.m_objects.entityFromId(candidate);
        if (!candidateEntity) continue;
        const ObjectLifecycleComponent* lifecycle =
            ecs::try_get<ObjectLifecycleComponent>(
                m_world.m_registry, *candidateEntity);
        if (lifecycle && lifecycle->phase != ObjectLifecyclePhase::Alive)
            continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(
                m_world.m_registry, *candidateEntity);
        if (health && health->effectivelyDead) continue;
        const ObjectMapStatusComponent* candidateMap =
            ecs::try_get<ObjectMapStatusComponent>(
                m_world.m_registry, *candidateEntity);
        if ((candidateMap && candidateMap->offMap) != sourceOffMap) continue;

        if (query.concealment ==
            ObjectSightConcealment::RejectHiddenStealth) {
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(
                    m_world.m_registry, *candidateEntity);
            if (status &&
                status->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Stealthed)) &&
                !status->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Detected)) &&
                !status->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Disguised))) {
                continue;
            }
        }

        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
            m_world.m_registry, *candidateEntity);
        if (query.targetPlayer &&
            (!owner || owner->player != query.targetPlayer)) {
            continue;
        }
        if (query.relationship &&
            relationshipBetweenObjects(
                m_world.m_registry, m_content.m_players, *sourceEntity,
                *candidateEntity) != *query.relationship) {
            continue;
        }

        if (!query.exactObjectTypes.empty()) {
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry, *candidateEntity);
            if (!type) continue;
            const container::StringView actual = !type->name.empty()
                ? container::StringView{type->name}
                : (type->archetype
                    ? container::StringView{
                          type->archetype->templateData.name}
                    : container::StringView{});
            if (!std::any_of(
                    query.exactObjectTypes.begin(),
                    query.exactObjectTypes.end(),
                    [actual](const container::String& expected) noexcept {
                        return container::asciiEqualIgnoreCase(
                            actual, expected);
                    })) {
                continue;
            }
        }

        const TransformComponent* targetTransform =
            ecs::try_get<TransformComponent>(
                m_world.m_registry, *candidateEntity);
        if (!targetTransform) continue;
        const LogicFixedVec3 targetPosition =
            readAuthoritativeObjectPosition(
                m_world.m_registry, *candidateEntity, *targetTransform);
        const math::q32_32 dx = targetPosition.x - center.x;
        const math::q32_32 dy = targetPosition.y - center.y;
        if (dx * dx + dy * dy <= sightSquared) return true;
    }
    return false;
}

bool GameSessionScriptQueryPort::canReceiveUpgrade(
    ObjectId object, container::StringView upgrade) const {
    if (!m_content.m_active || !object || upgrade.empty() ||
        m_world.m_objects.isPendingDestroy(object)) {
        return false;
    }
    const UpgradeCatalog* catalog =
        m_content.m_contentSnapshot.upgradeCatalog();
    const UpgradeDefinition* definition = catalog
        ? catalog->find(upgrade)
        : nullptr;
    if (!definition) return false;
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(object);
    if (!entity) return false;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
    const PlayerState* player = owner
        ? m_content.m_players.get(owner->player)
        : nullptr;
    return player && m_world.m_objectSimulation.canObjectReceiveUpgrade(
        m_world.m_registry, *entity, player->upgrades.completed,
        definition->id);
}

std::optional<game::ObjectBuildabilityStatus>
GameSessionScriptQueryPort::effectiveObjectBuildability(
    container::StringView objectType) const noexcept {
    const container::SharedPtr<const game::ObjectArchetype> archetype =
        m_content.m_contentSnapshot.findObjectArchetype(objectType);
    if (!archetype) return std::nullopt;
    const auto found =
        m_presentation.m_scriptObjectBuildabilityOverrides.find(
            archetype->templateData.name);
    return found ==
            m_presentation.m_scriptObjectBuildabilityOverrides.end()
        ? std::optional<game::ObjectBuildabilityStatus>{
              archetype->templateData.buildability}
        : std::optional<game::ObjectBuildabilityStatus>{found->second};
}

} // namespace engine::script
