#include "game/session/transaction/GameSessionMultiplayerVictoryTransactions.h"

#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"
#include "game/session/transaction/GameSessionPlayerStateTransactions.h"

#include <algorithm>
#include <optional>

namespace engine {
namespace {

[[nodiscard]] bool hasObjectKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

} // namespace

GameSessionMultiplayerVictoryTransactions::
GameSessionMultiplayerVictoryTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort lifecycle) noexcept
    : m_content(content),
      m_world(world),
      m_presentation(presentation),
      m_lifecycle(lifecycle) {}

void GameSessionMultiplayerVictoryTransactions::refresh() {
    const GameMode recordedMode =
        m_content.m_startInfo.mode == GameMode::Replay &&
            m_content.m_resolvedMatchSetup
        ? m_content.m_resolvedMatchSetup->mode
        : m_content.m_startInfo.mode;
    const bool multiplayerRules = m_content.m_startInfo.network.enabled ||
        recordedMode == GameMode::Skirmish;
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        !multiplayerRules) {
        return;
    }

    container::Vector<PlayerId> participants;
    for (const PlayerId id : m_content.m_players.activePlayerIds()) {
        const PlayerState* player = m_content.m_players.get(id);
        // VictoryConditions::cachePlayerPtrs excludes Neutral, observers and
        // FactionCivilian. playableSide is the frozen modern equivalent.
        if (player && player->isPlayableSide()) participants.push_back(id);
    }
    if (participants.empty()) return;

    const auto containsPlayer = [](container::Span<const PlayerId> values,
                                   PlayerId player) noexcept {
        return std::binary_search(values.begin(), values.end(), player);
    };
    const auto hasLegacyVictoryBuilding = [&](PlayerId player) {
        for (const ObjectId object : m_world.m_ownership.objects(player)) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(object);
            const ObjectLifecycleComponent* lifecycle = entity
                ? ecs::try_get<ObjectLifecycleComponent>(
                      m_world.m_registry, *entity)
                : nullptr;
            const ObjectHealthComponent* health = entity
                ? ecs::try_get<ObjectHealthComponent>(
                      m_world.m_registry, *entity)
                : nullptr;
            const ObjectKindOfComponent* kinds = entity
                ? ecs::try_get<ObjectKindOfComponent>(
                      m_world.m_registry, *entity)
                : nullptr;
            if (!entity || !lifecycle || !kinds ||
                lifecycle->phase != ObjectLifecyclePhase::Alive ||
                m_world.m_objects.isPendingDestroy(object) ||
                (health && health->effectivelyDead)) {
                continue;
            }
            if (hasObjectKind(kinds, game::ObjectKindOf::Structure) &&
                hasObjectKind(kinds,
                              game::ObjectKindOf::MpCountForVictory)) {
                return true;
            }
        }
        return false;
    };
    const auto defeatedNow = [&](PlayerId player) {
        const PlayerState* state = m_content.m_players.get(player);
        return !state || state->life == PlayerLifeState::Defeated ||
            !hasLegacyVictoryBuilding(player);
    };
    const auto mutualAllies = [&](PlayerId left, PlayerId right) {
        return left != right &&
            m_content.m_players.relationships().get(left, right) ==
                PlayerRelationship::Allies &&
            m_content.m_players.relationships().get(right, left) ==
                PlayerRelationship::Allies;
    };

    ScriptMultiplayerVictoryState& victory =
        m_presentation.m_scriptMultiplayerVictory;
    GameSessionPlayerStateTransactions playerState{m_content.m_players};
    if (!victory.singleAllianceRemaining) {
        PlayerId firstUndefeated = INVALID_PLAYER_ID;
        bool multipleAlliances = false;
        for (const PlayerId player : participants) {
            if (defeatedNow(player)) continue;
            if (!firstUndefeated) {
                firstUndefeated = player;
            } else if (!mutualAllies(firstUndefeated, player)) {
                multipleAlliances = true;
                break;
            }
        }
        if (!multipleAlliances) {
            victory.singleAllianceRemaining = true;
            victory.endTick = m_presentation.m_confirmedTick;
            if (firstUndefeated) {
                for (const PlayerId player : participants) {
                    if (player == firstUndefeated ||
                        mutualAllies(player, firstUndefeated)) {
                        victory.victoriousPlayers.push_back(player);
                    }
                }
            }
        }
    }

    // RefCode records elimination after checking the alliance terminal edge.
    for (const PlayerId player : participants) {
        if (!defeatedNow(player) ||
            containsPlayer(victory.defeatedPlayers, player)) {
            continue;
        }
        const auto position = std::lower_bound(
            victory.defeatedPlayers.begin(),
            victory.defeatedPlayers.end(), player);
        victory.defeatedPlayers.insert(position, player);
        static_cast<void>(playerState.setLifeState(
            player, PlayerLifeState::Defeated));
        static_cast<void>(playerState.setCash(player, 0));

        const container::Vector<ObjectId> remainingObjects{
            m_world.m_ownership.objects(player).begin(),
            m_world.m_ownership.objects(player).end()};
        GameSessionObjectLifecycleTransactions lifecycle{
            m_content, m_world, m_presentation, m_lifecycle};
        for (const ObjectId object : remainingObjects) {
            static_cast<void>(lifecycle.destroyObject(object));
        }
    }
}

} // namespace engine
