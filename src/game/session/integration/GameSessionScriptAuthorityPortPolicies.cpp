#include "GameSessionScriptAuthorityPort.h"
#include "game/session/state/GameSessionDomainState.h"

#include "core/container/string_utils.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/session/script/GameSessionScriptTeamsDetail.h"
#include "game/object/simulation/status/ObjectDisabled.h"

#include <algorithm>
#include <limits>

namespace engine::script {

bool GameSessionScriptAuthorityPort::acceptsAuthorityTick(
    uint64_t confirmedTick) const noexcept {
    return m_content.m_active && m_presentation.m_hasConfirmedFrame &&
        confirmedTick == m_presentation.m_confirmedTick;
}

size_t GameSessionScriptAuthorityPort::setPlayerFactoryTypeEnabled(
    PlayerId player, container::StringView factoryType, bool enabled,
    uint64_t confirmedTick) {
    if (!acceptsAuthorityTick(confirmedTick) ||
        !m_content.m_players.get(player) || factoryType.empty()) {
        return 0;
    }

    const container::Span<const ObjectId> owned = m_world.m_ownership.objects(player);
    container::Vector<ObjectId> snapshot{owned.begin(), owned.end()};
    size_t changed = 0;
    for (const ObjectId object : snapshot) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        const ThingTemplateComponent* type = entity
            ? ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !type || !type->archetype ||
            !container::asciiEqualIgnoreCase(
                type->archetype->templateData.name, factoryType)) {
            continue;
        }
        const ObjectDisabledTransition transition = enabled
            ? ObjectDisabledSystem::clear(
                  m_world.m_registry, *entity,
                  ObjectDisabledReason::ScriptDisabled, confirmedTick)
            : ObjectDisabledSystem::setUntil(
                  m_world.m_registry, *entity,
                  ObjectDisabledReason::ScriptDisabled,
                  OBJECT_DISABLED_FOREVER_TICK, confirmedTick);
        changed += transition.changed() ? 1u : 0u;
    }
    return changed;
}

bool GameSessionScriptAuthorityPort::setObjectBuildability(
    container::StringView objectType,
    game::ObjectBuildabilityStatus buildability,
    uint64_t confirmedTick) {
    if (!acceptsAuthorityTick(confirmedTick) || objectType.empty()) return false;
    switch (buildability) {
    case game::ObjectBuildabilityStatus::Yes:
    case game::ObjectBuildabilityStatus::IgnorePrerequisites:
    case game::ObjectBuildabilityStatus::No:
    case game::ObjectBuildabilityStatus::OnlyByAi:
        break;
    default:
        return false;
    }

    const container::SharedPtr<const game::ObjectArchetype> archetype =
        m_content.m_contentSnapshot.findObjectArchetype(objectType);
    if (!archetype) return false;
    const container::String& canonicalName = archetype->templateData.name;
    const auto found =
        m_presentation.m_scriptObjectBuildabilityOverrides.find(canonicalName);
    if (found != m_presentation.m_scriptObjectBuildabilityOverrides.end() &&
        found->second == buildability) {
        return false;
    }
    m_presentation.m_scriptObjectBuildabilityOverrides.insert_or_assign(
        canonicalName, buildability);
    return true;
}

void GameSessionScriptAuthorityPort::projectTeamRelationshipPolicy(
    ObjectTeamId team) {
    m_ownershipTransactions.projectTeamRelationshipPolicy(team);
}

bool GameSessionScriptAuthorityPort::setTeamToTeamRelationship(
    ObjectTeamId source, ObjectTeamId target,
    std::optional<PlayerRelationship> relationship,
    uint64_t confirmedTick) {
    if (!acceptsAuthorityTick(confirmedTick) || !source || !target)
        return false;
    const bool changed = relationship
        ? m_world.m_objectTeams.setTeamRelationshipOverride(
              source, target, *relationship)
        : m_world.m_objectTeams.removeTeamRelationshipOverride(source, target);
    if (changed) projectTeamRelationshipPolicy(source);
    return changed;
}

bool GameSessionScriptAuthorityPort::setTeamToPlayerRelationship(
    ObjectTeamId source, PlayerId target,
    std::optional<PlayerRelationship> relationship,
    uint64_t confirmedTick) {
    if (!acceptsAuthorityTick(confirmedTick) || !source ||
        !m_content.m_players.get(target)) {
        return false;
    }
    const bool changed = relationship
        ? m_world.m_objectTeams.setPlayerRelationshipOverride(
              source, target, *relationship)
        : m_world.m_objectTeams.removePlayerRelationshipOverride(source, target);
    if (changed) projectTeamRelationshipPolicy(source);
    return changed;
}

bool GameSessionScriptAuthorityPort::clearTeamRelationships(
    ObjectTeamId source, uint64_t confirmedTick) {
    if (!acceptsAuthorityTick(confirmedTick) || !source) return false;
    const bool changed = m_world.m_objectTeams.clearRelationshipOverrides(source);
    if (changed) projectTeamRelationshipPolicy(source);
    return changed;
}

bool GameSessionScriptAuthorityPort::setPlayerToTeamRelationship(
    PlayerId source, ObjectTeamId target,
    std::optional<PlayerRelationship> relationship,
    uint64_t confirmedTick) {
    if (!acceptsAuthorityTick(confirmedTick) ||
        !m_content.m_players.get(source) || !m_world.m_objectTeams.find(target)) {
        return false;
    }
    return relationship
        ? m_content.m_players.setTeamRelationshipOverride(
              source, target, *relationship)
        : m_content.m_players.removeTeamRelationshipOverride(source, target);
}

void GameSessionScriptAuthorityPort::setRankLevelLimit(int32_t limit) noexcept {
    m_presentation.m_scriptRankLevelLimit = std::max(1, limit);
}

int32_t GameSessionScriptAuthorityPort::rankLevelLimit() const noexcept {
    return m_presentation.m_scriptRankLevelLimit;
}

bool GameSessionScriptAuthorityPort::canCreateNamedObject(
    container::StringView scriptName) const noexcept {
    if (scriptName.empty()) return true;
    const std::optional<ObjectId> existing =
        m_presentation.m_scriptObjects.liveNamedObject(scriptName);
    if (!existing) return true;
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(*existing);
    if (!entity) return true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
    return health && health->effectivelyDead;
}

std::optional<ObjectTeamId>
GameSessionScriptAuthorityPort::ensureScenarioTeamForCreate(
    container::StringView alias, uint64_t confirmedTick) {
    if (!acceptsAuthorityTick(confirmedTick) ||
        !m_presentation.m_scenarioDefinition || alias.empty()) {
        return std::nullopt;
    }
    const scenario::OwnerReference reference =
        m_presentation.m_scenarioDefinition->resolveOwner(alias);
    if (reference.kind != scenario::OwnerReferenceKind::ScriptTeam ||
        !reference.scriptTeam) {
        return std::nullopt;
    }
    const scenario::ScriptTeamDefinition* definition =
        m_presentation.m_scenarioDefinition->findScriptTeam(
            reference.scriptTeam);
    if (!definition ||
        !m_content.m_players.get(definition->resolvedOwner)) {
        return std::nullopt;
    }

    std::optional<ObjectTeamId> instance =
        m_world.m_objectTeams.scenarioTeam(definition->id);
    if (!instance) {
        return m_world.m_objectTeams.createScenarioTeamInstance(
            definition->id, definition->name, definition->resolvedOwner,
            true, confirmedTick);
    }
    if (definition->isSingleton &&
        !m_world.m_objectTeams.isActive(*instance) &&
        !m_world.m_objectTeams.activate(*instance, confirmedTick)) {
        return std::nullopt;
    }
    return instance;
}

bool GameSessionScriptAuthorityPort::setTeamActive(
    ObjectTeamId team, bool active, uint64_t confirmedTick) {
    if (!acceptsAuthorityTick(confirmedTick) || !team) return false;
    if (active) return m_world.m_objectTeams.activate(team, confirmedTick);
    if (!m_world.m_objectTeams.deactivate(team)) return false;

    for (const ObjectTeamRecord& source : m_world.m_objectTeams.teams()) {
        if (source.id == team) continue;
        if (m_world.m_objectTeams.removeTeamRelationshipOverride(
                source.id, team)) {
            projectTeamRelationshipPolicy(source.id);
        }
    }
    for (const PlayerId source : m_content.m_players.activePlayerIds()) {
        static_cast<void>(
            m_content.m_players.removeTeamRelationshipOverride(source, team));
    }
    if (m_world.m_objectTeams.clearRelationshipOverrides(team)) {
        projectTeamRelationshipPolicy(team);
    }
    return true;
}

bool GameSessionScriptAuthorityPort::applyAttackPrioritySet(
    ObjectId object, container::StringView setName) {
    if (!m_ai.m_objectAI.actorState(object)) return false;
    const auto known = m_presentation.m_scriptAttackPrioritySets.find(
        container::String{setName});
    const uint32_t resolvedId =
        known == m_presentation.m_scriptAttackPrioritySets.end()
        ? 0u
        : known->second.id;
    return m_objectTransactions.setAttackPrioritySetId(object, resolvedId);
}

size_t GameSessionScriptAuthorityPort::applyTeamAttackPrioritySet(
    ObjectTeamId team, container::StringView setName) {
    if (!team || !m_world.m_objectTeams.find(team)) return 0;
    const auto known = m_presentation.m_scriptAttackPrioritySets.find(
        container::String{setName});
    if (known != m_presentation.m_scriptAttackPrioritySets.end()) {
        static_cast<void>(m_world.m_objectTeams.setAttackPrioritySet(
            team, known->first));
    }
    const container::StringView appliedName =
        known == m_presentation.m_scriptAttackPrioritySets.end()
        ? container::StringView{}
        : container::StringView{known->first};
    size_t changed = 0;
    for (const ObjectId member : m_world.m_objectTeams.legacyMembers(team)) {
        changed += applyAttackPrioritySet(member, appliedName) ? 1u : 0u;
    }
    return changed;
}

size_t GameSessionScriptAuthorityPort::setTeamWanderInPlace(
    ObjectTeamId team, uint64_t confirmedTick) {
    if (!team || !m_world.m_objectTeams.find(team)) return 0;
    size_t changed = 0;
    for (const ObjectId member : m_world.m_objectTeams.legacyMembers(team)) {
        // ScriptActions::doTeamWanderInPlace skips members without an
        // AIUpdateInterface. Avoid leaving an unowned persistent Move head on
        // objects that cannot enter the ObjectAI WanderInPlace state.
        if (!m_ai.m_objectAI.find(member)) continue;
        changed += m_world.m_objectSimulation.setWanderInPlace(
            m_world.m_registry, m_world.m_objects, member, confirmedTick)
            ? 1u
            : 0u;
    }
    return changed;
}

size_t GameSessionScriptAuthorityPort::setTeamCommandButtonHunt(
    ObjectTeamId team, container::StringView commandButton,
    uint64_t confirmedTick) {
    if (!team || commandButton.empty() ||
        !m_world.m_objectTeams.find(team)) {
        return 0;
    }
    const game::CommandButtonTemplate* button =
        m_content.m_contentSnapshot.findCommandButton(commandButton);
    if (!button) return 0;
    const game::CommandButtonKind commandKind = button->descriptor.kind;
    const bool objectTargetSpecialPower =
        commandKind == game::CommandButtonKind::SpecialPower &&
        !button->specialPower.empty() &&
        script_team_detail::commandButtonNeedsObjectTarget(*button);
    const bool weaponCommand =
        commandKind == game::CommandButtonKind::SwitchWeapon ||
        commandKind == game::CommandButtonKind::FireWeapon;
    const bool intentionalContact =
        commandKind == game::CommandButtonKind::HijackVehicle ||
        commandKind == game::CommandButtonKind::ConvertToCarBomb ||
        commandKind == game::CommandButtonKind::SabotageBuilding;
    if (!objectTargetSpecialPower && !weaponCommand &&
        !intentionalContact) return 0;

    size_t changed = 0;
    for (const ObjectId member : m_world.m_objectTeams.legacyMembers(team)) {
        if (!m_ai.m_objectAI.find(member)) continue;
        bool containsButton = false;
        for (size_t slot = 0; slot < game::COMMAND_SET_SLOT_COUNT; ++slot) {
            if (container::asciiEqualIgnoreCase(
                    m_queries.effectiveObjectCommandBarButton(member, slot),
                    button->name)) {
                containsButton = true;
                break;
            }
        }
        if (!containsButton) continue;
        changed += m_world.m_objectSimulation.setCommandButtonHunt(
            m_world.m_registry, m_world.m_objects, member, button->name,
            confirmedTick) ? 1u : 0u;
    }
    return changed;
}

bool GameSessionScriptAuthorityPort::mutateAttackPrioritySet(
    ScriptAttackPriorityMutationKind mutation,
    container::StringView setName,
    container::Span<const container::String> selectors,
    int32_t priority) {
    if (setName.empty()) return false;
    const bool needsSelector =
        mutation != ScriptAttackPriorityMutationKind::Default;
    if (needsSelector != !selectors.empty()) return false;

    container::Vector<container::String> canonicalSelectors;
    if (mutation == ScriptAttackPriorityMutationKind::ObjectType) {
        canonicalSelectors.reserve(selectors.size());
        for (const container::String& authoredSelector : selectors) {
            if (authoredSelector.empty()) break;
            const container::SharedPtr<const game::ObjectArchetype> archetype =
                m_content.m_contentSnapshot.findObjectArchetype(
                    authoredSelector);
            if (!archetype) break;
            canonicalSelectors.push_back(archetype->templateData.name);
        }
        if (canonicalSelectors.empty()) return false;
    } else if (mutation == ScriptAttackPriorityMutationKind::KindOf) {
        canonicalSelectors.assign(selectors.begin(), selectors.end());
    }

    auto [found, inserted] =
        m_presentation.m_scriptAttackPrioritySets.try_emplace(
            container::String{setName});
    auto& set = found->second;
    if (inserted) {
        if (m_presentation.m_scriptAttackPriorityById.empty())
            m_presentation.m_scriptAttackPriorityById.push_back(nullptr);
        if (m_presentation.m_scriptAttackPriorityById.size() >
            std::numeric_limits<uint32_t>::max()) {
            m_presentation.m_scriptAttackPrioritySets.erase(found);
            return false;
        }
        set.id = static_cast<uint32_t>(
            m_presentation.m_scriptAttackPriorityById.size());
        m_presentation.m_scriptAttackPriorityById.push_back(&set);
    }
    bool changed = inserted;
    if (mutation == ScriptAttackPriorityMutationKind::Default) {
        if (set.defaultPriority != priority) {
            set.defaultPriority = priority;
            changed = true;
        }
    } else {
        for (container::String& canonicalSelector : canonicalSelectors) {
            if (canonicalSelector.empty()) continue;
            const std::optional<game::ObjectKindOf> selectorKind =
                mutation == ScriptAttackPriorityMutationKind::KindOf
                ? game::parseObjectKindOf(canonicalSelector)
                : std::optional<game::ObjectKindOf>{};
            if (mutation == ScriptAttackPriorityMutationKind::KindOf &&
                !selectorKind) {
                continue;
            }
            ++m_presentation.m_scriptAttackPrioritySequence;
            if (m_presentation.m_scriptAttackPrioritySequence == 0)
                ++m_presentation.m_scriptAttackPrioritySequence;
            set.rules.push_back({
                .mutation = mutation,
                .selector = std::move(canonicalSelector),
                .selectorKind = selectorKind.value_or(
                    game::ObjectKindOf::Count),
                .priority = priority,
                .sequence =
                    m_presentation.m_scriptAttackPrioritySequence,
            });
            changed = true;
        }
    }
    if (changed) {
        ++set.revision;
        if (set.revision == 0) ++set.revision;
    }
    return changed;
}

size_t GameSessionScriptAuthorityPort::destroyAllUnmanned(
    uint64_t confirmedTick) {
    if (!acceptsAuthorityTick(confirmedTick)) return 0;
    container::Vector<ObjectId> targets;
    const auto view = ecs::view<const ObjectIdentityComponent>(m_world.m_registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && isObjectDisabledBy(
                m_world.m_registry, entity,
                ObjectDisabledReason::Unmanned, confirmedTick)) {
            targets.push_back(identity.id);
        }
    }
    std::sort(targets.begin(), targets.end());
    size_t destroyed = 0;
    for (const ObjectId target : targets) {
        destroyed +=
            m_lifecycleTransactions.destroyObject(target) ? 1u : 0u;
    }
    return destroyed;
}

} // namespace engine::script
