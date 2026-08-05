#include "game/session/lifecycle/GameSessionMapImportPort.h"

#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "core/container/string_utils.h"

namespace engine {

GameSessionMapImportPort GameSession::mapImportPort() noexcept {
    GameSessionStateRoot& state = domainState();
    return GameSessionMapImportPort{
        state.contentState(), state.worldState(), state.presentationState()};
}

ClientTerrainImportPolicy
GameSessionMapImportPort::clientTerrainPolicy() const {
    if (const auto& quality = m_presentation.m_renderFeatureQualitySnapshot) {
        return {
            .showTrees = quality->requested.showTrees,
            .multiplayer = m_content.m_startInfo.network.enabled,
        };
    }
    const RenderFeatureQualitySettings safeFeature;
    return {
        .showTrees = safeFeature.showTrees,
        .multiplayer = m_content.m_startInfo.network.enabled,
    };
}

game::ModelConditionMask
GameSessionMapImportPort::initialModelConditions() const {
    game::ModelConditionMask result;
    const RenderGameDataSettings& settings =
        m_presentation.m_renderGameDataSettings;
    if (settings.visual.defaultTimeOfDay == RenderTimeOfDay::Night) {
        result.set(game::ModelConditionFlag::Night);
    }
    if (settings.visual.defaultWeather == RenderWeather::Snowy) {
        result.set(game::ModelConditionFlag::Snow);
    }
    return result;
}

container::SharedPtr<const game::ObjectArchetype>
GameSessionMapImportPort::findObjectArchetype(
    container::StringView name) const {
    return m_content.m_contentSnapshot.findObjectArchetype(name);
}

const game::MapContentIdentity&
GameSessionMapImportPort::mapIdentity() const noexcept {
    return m_content.m_terrain.contentIdentity();
}

uint64_t GameSessionMapImportPort::simulationContentFingerprint()
    const noexcept {
    return m_content.m_players.simulationContentFingerprint();
}

uint64_t GameSessionMapImportPort::presentationEpoch() const noexcept {
    return m_presentation.m_scriptPresentationEpoch;
}

int64_t GameSessionMapImportPort::groundHeightRaw(
    int64_t x, int64_t y) const noexcept {
    return m_content.m_terrain.groundHeightRaw(x, y);
}

float GameSessionMapImportPort::groundHeight(
    float x, float y) const noexcept {
    return m_content.m_terrain.groundHeight(x, y);
}

void GameSessionMapImportPort::beginTerrainHeightMutationBatch() noexcept {
    m_content.m_terrain.beginHeightMutationBatch();
}

void GameSessionMapImportPort::endTerrainHeightMutationBatch() noexcept {
    m_content.m_terrain.endHeightMutationBatch();
}

MapImportOwnerResolution GameSessionMapImportPort::resolveOwner(
    container::StringView authoredOwner) {
    if (!authoredOwner.empty()) {
        if (const auto& scenarioDefinition = m_presentation.m_scenarioDefinition) {
            const scenario::OwnerReference resolved =
                scenarioDefinition->resolveOwner(authoredOwner);
            if (resolved.kind == scenario::OwnerReferenceKind::Player &&
                m_content.m_players.get(resolved.player)) {
                return {.player = resolved.player};
            }
            if (resolved.kind ==
                    scenario::OwnerReferenceKind::ScriptTeam &&
                m_content.m_players.get(resolved.player)) {
                const scenario::ScriptTeamDefinition* definition =
                    scenarioDefinition->findScriptTeam(resolved.scriptTeam);
                std::optional<ObjectTeamId> team;
                if (definition && m_content.m_active) {
                    team = m_world.m_objectTeams.scenarioTeam(definition->id);
                    if (!team) {
                        team = m_world.m_objectTeams.createScenarioTeamInstance(
                            definition->id, definition->name,
                            definition->resolvedOwner, false);
                    }
                }
                if (!team) {
                    return {
                        .player = resolved.player,
                        .scenarioTeamUnresolved = true,
                    };
                }
                return {
                    .player = resolved.player,
                    .primaryTeam = *team,
                    .usesScenarioTeam = true,
                };
            }
        }
        if (container::asciiEqualIgnoreCase(authoredOwner, "Neutral") &&
            m_content.m_players.neutralPlayer()) {
            return {.player = NEUTRAL_PLAYER_ID};
        }
        constexpr container::StringView legacyPlayerPrefix = "Plyr";
        container::StringView factionAlias = authoredOwner;
        const bool hasLegacyPlayerPrefix =
            authoredOwner.size() > legacyPlayerPrefix.size() &&
            container::asciiEqualIgnoreCase(
                authoredOwner.substr(0, legacyPlayerPrefix.size()),
                legacyPlayerPrefix);
        if (hasLegacyPlayerPrefix) {
            factionAlias = authoredOwner.substr(legacyPlayerPrefix.size());
            if (container::asciiEqualIgnoreCase(
                    factionAlias, "Civilian") &&
                m_content.m_players.neutralPlayer()) {
                return {.player = NEUTRAL_PLAYER_ID};
            }
        }
        if (const std::optional<PlayerId> mapPlayer =
                parseCanonicalMapPlayerAlias(authoredOwner);
            mapPlayer && m_content.m_players.get(*mapPlayer)) {
            return {.player = *mapPlayer};
        }
        for (const PlayerId id : m_content.m_players.activePlayerIds()) {
            const PlayerState* player = m_content.m_players.get(id);
            if (player &&
                (container::asciiEqualIgnoreCase(
                     authoredOwner, player->displayName) ||
                 container::asciiEqualIgnoreCase(
                     authoredOwner, player->side) ||
                 container::asciiEqualIgnoreCase(
                     authoredOwner, player->baseSide) ||
                 (hasLegacyPlayerPrefix &&
                  (container::asciiEqualIgnoreCase(
                       factionAlias, player->displayName) ||
                   container::asciiEqualIgnoreCase(
                       factionAlias, player->side) ||
                   container::asciiEqualIgnoreCase(
                       factionAlias, player->baseSide))))) {
                return {.player = id};
            }
        }
    }
    const PlayerState* neutral = m_content.m_players.neutralPlayer();
    return {
        .player = neutral ? neutral->id : INVALID_PLAYER_ID,
        .usedFallback = true,
    };
}

bool GameSessionMapImportPort::activateScenarioTeam(ObjectTeamId team) {
    if (!m_content.m_active || !team) return false;
    return m_presentation.m_hasConfirmedFrame
        ? m_world.m_objectTeams.activate(
              team, m_presentation.m_confirmedTick)
        : m_world.m_objectTeams.activateAtStartup(team);
}

MapImportedObjectState GameSessionMapImportPort::importedObjectState(
    ObjectId object) const {
    MapImportedObjectState result;
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(object);
    if (!entity) return result;
    if (const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(
                m_world.m_registry, *entity)) {
        result.currentHealth = health->currentFixed;
        result.maximumHealth = health->maximumFixed;
        result.damageState = static_cast<uint32_t>(health->damageState);
        result.hasHealth = true;
    }
    if (const RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(
                m_world.m_registry, *entity)) {
        result.modelConditions = visual->modelConditionFlags;
        result.modelAsset = visual->modelAsset;
        result.hasVisual = true;
    }
    return result;
}

} // namespace engine
