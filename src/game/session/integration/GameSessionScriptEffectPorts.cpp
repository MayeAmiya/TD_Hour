#include "game/session/integration/GameSessionScriptAuthorityPort.h"
#include "game/session/integration/GameSessionScriptPresentationPort.h"

#include "core/config/GlobalData.h"
#include "core/container/string_utils.h"
#include "core/math/wwmath/base/wwmath.h"
#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>
#include <cmath>

namespace engine::script {
namespace {

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] bool isThisPlayerReference(container::StringView value) noexcept {
    return equalAsciiInsensitive(value, "ThisPlayer") ||
           equalAsciiInsensitive(value, "<This Player>");
}

[[nodiscard]] bool isThisPlayerEnemyReference(
    container::StringView value) noexcept {
    return equalAsciiInsensitive(value, "<This Player's Enemy>");
}

[[nodiscard]] bool isLocalPlayerReference(container::StringView value) noexcept {
    // Deliberately does NOT treat an empty reference as the local player.  An
    // empty player field never reaches a resolver (ScriptProgramBuilder drops
    // the whole program, and ScriptRuntime::resolvePlayer early-returns), so the
    // branch was dead — and it also contradicted the codebase's own convention
    // that an empty side name means Neutral.
    return equalAsciiInsensitive(value, "LocalPlayer") ||
           equalAsciiInsensitive(value, "<Local Player>");
}

[[nodiscard]] bool isChallengeLocalPlayerReference(
    container::StringView value, GameMode mode) noexcept {
    return mode == GameMode::Challenge &&
        equalAsciiInsensitive(value, "ThePlayer");
}

[[nodiscard]] std::optional<ObjectTeamId> resolveEffectTeamFor(
    GameSessionScriptQueryPort& queries, const ObjectTeamRegistry& objectTeams,
    container::StringView name,
    const ScriptEffectHeader& header) noexcept {
    if (equalAsciiInsensitive(name, "<This Team>")) {
        const ObjectTeamId team = header.invocation.thisTeam();
        return team && objectTeams.find(team)
            ? std::optional<ObjectTeamId>{team}
            : std::nullopt;
    }
    return queries.resolveScenarioTeamAlias(
        name, header.invocation.callingTeam,
        header.invocation.conditionTeam);
}

} // namespace

GameSessionScriptAuthorityPort::GameSessionScriptAuthorityPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort lifecycle,
    GameSessionScriptQueryPort& queries,
    uint64_t confirmedTick) noexcept
    : m_content(content),
      m_world(world),
      m_ai(ai),
      m_presentation(presentation),
      m_queries(queries),
      m_objectTransactions(world.m_registry, world.m_objects),
      m_ownershipTransactions(
          content, world, ai, presentation,
          lifecycle),
      m_progressionTransactions(
          content, world, presentation,
          lifecycle),
      m_damageTransactions(
          content, world, presentation,
          lifecycle),
      m_lifecycleTransactions(
          content, world, presentation,
          lifecycle),
      m_playerTransactions(content.m_players),
      m_containmentTransactions(
          world.m_registry, world.m_objects, world.m_objectSimulation,
          world.m_spatialIndex, world.m_objectTeams, content.m_players,
          content.m_contentSnapshot),
      m_containmentPlanTransactions(
          content, world, ai, presentation, &m_damageTransactions),
      m_orderTransactions(
          world.m_registry, world.m_objects, ai.m_objectAI,
          content.m_contentSnapshot, content.m_terrain,
          content.m_simulationRandom, world.m_objectSimulation),
      m_orderAdmissionTransactions(
          content, world, ai, presentation,
          makeOrderAdmissionPolicyPort(
              content, world, presentation,
              lifecycle)),
      m_scenarioPlanTransactions(
          content, world, ai, presentation,
          makeScenarioTransactionPort(
              content, world, ai, presentation,
              lifecycle)),
      m_confirmedTick(confirmedTick) {}

PlayerRegistry& GameSessionScriptAuthorityPort::players() noexcept {
    return m_content.m_players;
}

ecs::registry& GameSessionScriptAuthorityPort::registry() noexcept {
    return m_world.m_registry;
}

const GameContentSnapshot&
GameSessionScriptAuthorityPort::contentSnapshot() const noexcept {
    return m_content.m_contentSnapshot;
}

game::terrain::TerrainLogic&
GameSessionScriptAuthorityPort::terrain() noexcept {
    return m_content.m_terrain;
}

const ObjectOwnershipIndex&
GameSessionScriptAuthorityPort::ownership() const noexcept {
    return m_world.m_ownership;
}

ObjectTeamRegistry& GameSessionScriptAuthorityPort::objectTeams() noexcept {
    return m_world.m_objectTeams;
}

const GameStartInfo&
GameSessionScriptAuthorityPort::startInfo() const noexcept {
    return m_content.m_startInfo;
}

scenario::MissionState&
GameSessionScriptAuthorityPort::missionState() noexcept {
    return m_presentation.m_missionState;
}

ai::ObjectAIRuntime&
GameSessionScriptAuthorityPort::objectAIRuntime() noexcept {
    return m_ai.m_objectAI;
}

void GameSessionScriptAuthorityPort::setTimeFrozen(bool frozen) noexcept {
    if (m_content.m_active) m_presentation.m_scriptTimeFrozen = frozen;
}

void GameSessionScriptAuthorityPort::setScoreAccumulationEnabled(
    bool enabled) noexcept {
    if (!m_content.m_active) return;
    m_presentation.m_scoreAccumulationEnabled = enabled;
    m_content.m_players.setScoreAccumulationEnabled(enabled);
}

std::optional<ecs::entity>
GameSessionScriptAuthorityPort::entityFromId(ObjectId object) const {
    return m_world.m_objects.entityFromId(object);
}

bool GameSessionScriptAuthorityPort::applyObjectOrPlayerPolicy(
    const ScriptEffect& effect) {
    if (detail::applyObjectEffect(*this, effect)) return true;
    return detail::applyPlayerPolicyEffect(*this, effect);
}

bool GameSessionScriptAuthorityPort::applyOrderAndAi(
    const ScriptEffect& effect) {
    return detail::applyOrderAndAiEffect(*this, effect);
}

int32_t GameSessionScriptAuthorityPort::integerInclusive(
    int32_t lo, int32_t hi) noexcept {
    return m_content.m_simulationRandom.integerInclusive(lo, hi);
}

void GameSessionScriptAuthorityPort::resolveQueuedObjectDamage() {
    m_damageTransactions.resolveQueuedObjectDamage();
}

std::optional<PlayerId> GameSessionScriptAuthorityPort::resolvePlayer(
    container::StringView name, PlayerId currentPlayer,
    container::StringView currentPlayerAlias) const noexcept {
    if (isThisPlayerReference(name)) {
        if (currentPlayer && m_content.m_players.get(currentPlayer))
            return currentPlayer;
        if (currentPlayerAlias.empty() ||
            isThisPlayerReference(currentPlayerAlias)) {
            return std::nullopt;
        }
        return resolvePlayer(currentPlayerAlias, INVALID_PLAYER_ID, {});
    }
    if (isThisPlayerEnemyReference(name)) {
        if (currentPlayer && m_content.m_players.get(currentPlayer))
            return m_queries.currentEnemyPlayer(currentPlayer);
        if (currentPlayerAlias.empty() ||
            isThisPlayerReference(currentPlayerAlias) ||
            isThisPlayerEnemyReference(currentPlayerAlias)) {
            return std::nullopt;
        }
        const std::optional<PlayerId> resolvedCurrent = resolvePlayer(
            currentPlayerAlias, INVALID_PLAYER_ID, {});
        return resolvedCurrent
            ? m_queries.currentEnemyPlayer(*resolvedCurrent)
            : std::nullopt;
    }
    if (isLocalPlayerReference(name) ||
        isChallengeLocalPlayerReference(name, m_content.m_startInfo.mode)) {
        // localPlayerId() is per-client state, and this resolver feeds
        // AUTHORITATIVE effects (cash, relationships, science, ownership
        // transfer, kill/defeat).  Resolving it under lockstep would commit a
        // different mutation on every peer — a guaranteed desync.  Refuse, the
        // same way the sibling local-state queries do for network/replay.
        if (m_content.m_startInfo.network.enabled ||
            m_content.m_startInfo.mode == GameMode::Replay) {
            return std::nullopt;
        }
        const PlayerId local = m_content.m_players.localPlayerId();
        return m_content.m_players.get(local)
            ? std::optional<PlayerId>{local}
            : std::nullopt;
    }
    return m_queries.findPlayer(name);
}

std::optional<ObjectTeamId>
GameSessionScriptAuthorityPort::resolveEffectTeam(
    container::StringView name,
    const ScriptEffectHeader& header) const noexcept {
    return resolveEffectTeamFor(
        m_queries, m_world.m_objectTeams, name, header);
}

void GameSessionScriptAuthorityPort::emitDiagnostic(
    const ScriptEffectHeader& header, container::String text) {
    m_presentation.m_scriptSessionEvents.push_back({
        .kind = ScriptSessionEventKind::Diagnostic,
        .confirmedTick = header.confirmedTick,
        .sourceScriptId = header.sourceScript.value,
        .ordinal = header.ordinal,
        .text = std::move(text),
        .localized = false,
    });
}

GameSessionScriptPresentationPort::GameSessionScriptPresentationPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionScriptQueryPort& queries,
    GameSessionScriptLocalPresentationState& localPresentation,
    uint64_t confirmedTick) noexcept
    : m_content(content),
      m_world(world),
      m_presentation(presentation),
      m_queries(queries),
      m_localPresentation(localPresentation),
      m_confirmedTick(confirmedTick) {}

std::optional<ecs::entity>
GameSessionScriptPresentationPort::entityFromId(
    ObjectId object) const noexcept {
    return m_world.m_objects.entityFromId(object);
}

const GameStartInfo&
GameSessionScriptPresentationPort::startInfo() const noexcept {
    return m_content.m_startInfo;
}

game::terrain::TerrainLogic&
GameSessionScriptPresentationPort::terrain() noexcept {
    return m_content.m_terrain;
}

GameCameraDirector&
GameSessionScriptPresentationPort::cameraDirector() noexcept {
    return m_presentation.m_cameraDirector;
}

void GameSessionScriptPresentationPort::armCameraTimeFreeze() noexcept {
    if (m_content.m_active) m_presentation.m_scriptCamera.armTimeFreeze();
}

void GameSessionScriptPresentationPort::setCameraFollow(
    ObjectId object, bool snap) noexcept {
    if (!m_content.m_active || !object || !entityFromId(object)) return;
    m_presentation.m_scriptCamera.follow(object, snap);
}

void GameSessionScriptPresentationPort::stopCameraFollow() noexcept {
    m_presentation.m_scriptCamera.clearLock();
    m_presentation.m_cameraDirector.scriptStopFollowing();
}

void GameSessionScriptPresentationPort::setCameraTether(
    ObjectId object, bool snap, float play) noexcept {
    if (!m_content.m_active || !object || !entityFromId(object) ||
        !std::isfinite(play)) {
        return;
    }
    const float configuredCellSize = config::TheWritableGlobalData
        ? config::TheWritableGlobalData->partitionCellSize() : 100.0f;
    const float partitionCellSize = std::isfinite(configuredCellSize) &&
            configuredCellSize > math::EPSILON
        ? configuredCellSize : 100.0f;
    m_presentation.m_scriptCamera.tether(
        object, snap, play, partitionCellSize);
}

void GameSessionScriptPresentationPort::stopCameraTether() noexcept {
    stopCameraFollow();
}

bool GameSessionScriptPresentationPort::setTreeSwayPresentation(
    float directionRadians, float intensityRadians, float leanRadians,
    int32_t periodFrames, float randomness, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) noexcept {
    if (!m_content.m_active || !std::isfinite(directionRadians) ||
        !std::isfinite(intensityRadians) || !std::isfinite(leanRadians) ||
        !std::isfinite(randomness)) {
        return false;
    }
    const uint32_t effectivePeriod =
        static_cast<uint32_t>(std::max(1, periodFrames));
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) {
        ++m_presentation.m_scriptPresentationSequence;
    }
    m_presentation.m_scriptTreeSwayPresentation = {
        .enabled = m_presentation.m_scriptTreeSwayPresentation.enabled,
        .directionRadians = directionRadians,
        .intensityRadians = intensityRadians,
        .leanRadians = leanRadians,
        .periodFrames = effectivePeriod,
        .randomness = randomness,
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    };
    return true;
}

bool GameSessionScriptPresentationPort::setWeatherPresentation(
    bool visible, uint64_t confirmedTick, uint32_t sourceScriptId,
    uint32_t ordinal) noexcept {
    if (!m_content.m_active ||
        m_presentation.m_scriptWeatherPresentation.visible == visible) {
        return false;
    }
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) {
        ++m_presentation.m_scriptPresentationSequence;
    }
    m_presentation.m_scriptWeatherPresentation.visible = visible;
    m_presentation.m_scriptWeatherPresentation.stamp = {
        .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
        .sequence = m_presentation.m_scriptPresentationSequence,
        .confirmedTick = confirmedTick,
        .sourceScriptId = sourceScriptId,
        .ordinal = ordinal,
    };
    return true;
}

bool GameSessionScriptPresentationPort::setInfantryLightingPresentation(
    std::optional<float> overrideScale, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) noexcept {
    if (!m_content.m_active ||
        (overrideScale && (!std::isfinite(*overrideScale) ||
                           *overrideScale <= 0.0f)) ||
        m_presentation.m_scriptInfantryLightingPresentation.overrideScale ==
            overrideScale) {
        return false;
    }
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) {
        ++m_presentation.m_scriptPresentationSequence;
    }
    m_presentation.m_scriptInfantryLightingPresentation = {
        .overrideScale = overrideScale,
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    };
    return true;
}

bool GameSessionScriptPresentationPort::apply(const ScriptEffect& effect) {
    return detail::applyPresentationEffect(*this, effect);
}

std::optional<ObjectTeamId>
GameSessionScriptPresentationPort::resolveEffectTeam(
    container::StringView name,
    const ScriptEffectHeader& header) const noexcept {
    return resolveEffectTeamFor(
        m_queries, m_world.m_objectTeams, name, header);
}

void GameSessionScriptPresentationPort::emitDiagnostic(
    const ScriptEffectHeader& header, container::String text) {
    m_presentation.m_scriptSessionEvents.push_back({
        .kind = ScriptSessionEventKind::Diagnostic,
        .confirmedTick = header.confirmedTick,
        .sourceScriptId = header.sourceScript.value,
        .ordinal = header.ordinal,
        .text = std::move(text),
        .localized = false,
    });
}

void GameSessionScriptPresentationPort::emitSessionEvent(
    ScriptSessionEvent event) {
    if (!m_content.m_active || event.text.empty()) return;
    m_presentation.m_scriptSessionEvents.push_back(std::move(event));
}

bool GameSessionScriptPresentationPort::emitMoviePresentation(
    ScriptMovieTarget target, container::String movieName,
    uint64_t confirmedTick, uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active || static_cast<uint8_t>(target) >=
            static_cast<uint8_t>(ScriptMovieTarget::Count)) {
        return false;
    }
    // Video is deliberately a synchronous compatibility fact: no Bink
    // request, playback owner, or later client acknowledgement exists in
    // this target.  It is nevertheless an action, not a latest-value state:
    // every confirmed MOVIE_PLAY_* contributes one completion because every
    // HAS_FINISHED_VIDEO consumes exactly one.  Collapsing same-name actions
    // before that consumption would let a later matching condition wait
    // forever.
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) {
        ++m_presentation.m_scriptPresentationSequence;
    }
    // Video playback is disabled for the current ingame target. Record the
    // request as a deterministic one-shot completion without a decoder or UI
    // round trip.
    return m_presentation.m_scriptPresentationCompletions.recordOneShot({
        .kind = ScriptPresentationCompletionKind::Video,
        .name = std::move(movieName),
        .stamp = {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        },
    });
}

bool GameSessionScriptPresentationPort::beginMusicCompletionTracking(
    container::String trackName, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick) {
        return false;
    }
    ++m_presentation.m_scriptPresentationSequence;
    if (m_presentation.m_scriptPresentationSequence == 0) {
        ++m_presentation.m_scriptPresentationSequence;
    }
    return m_presentation.m_scriptPresentationCompletions.beginMusicTrack(
        std::move(trackName), {
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        });
}

bool GameSessionScriptPresentationPort::emitAudioEvent(
    game::GameAudioEvent event) {
    // ScriptActions constructs AudioEventRTS with the local player index.
    // Preserve that authored address explicitly: script sounds often have no
    // world object from which the generic publication port could derive it.
    if (!event.sourcePlayer) {
        if (const PlayerState* local = m_content.m_players.localPlayer()) {
            event.sourcePlayer = local->id;
        }
    }
    event.logical = true;
    return m_content.m_active && m_presentation.m_audioJournal.emit(
        std::move(event), m_content.m_startInfo.seed);
}

bool GameSessionScriptPresentationPort::emitAudioControlEvent(
    game::GameAudioControlEvent event) {
    return m_content.m_active &&
        m_presentation.m_audioJournal.emit(std::move(event));
}

bool GameSessionScriptPresentationPort::applyMapPresentation(
    const ScriptMapPresentationEffect& effect, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal) {
    if (!m_content.m_active) return false;
    const auto nextStamp = [&]() noexcept {
        ++m_presentation.m_scriptPresentationSequence;
        if (m_presentation.m_scriptPresentationSequence == 0) {
            ++m_presentation.m_scriptPresentationSequence;
        }
        return ScriptPresentationControlStamp{
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        };
    };

    switch (effect.command) {
    case ScriptMapPresentationCommand::CreateRadarEvent: {
        if (!std::isfinite(effect.position.x()) ||
            !std::isfinite(effect.position.y()) ||
            !std::isfinite(effect.position.z())) {
            return false;
        }
        const uint64_t framesPerSecond = static_cast<uint32_t>(
            std::max(1, m_content.m_startInfo.gameSpeedFPS));
        constexpr uint64_t LifetimeSeconds = 4;
        const uint64_t lifetimeTicks = framesPerSecond * LifetimeSeconds;
        const uint64_t dieTick = confirmedTick >
                std::numeric_limits<uint64_t>::max() - lifetimeTicks
            ? std::numeric_limits<uint64_t>::max()
            : confirmedTick + lifetimeTicks;
        m_presentation.m_scriptMapPresentation.appendRadarEvent({
            .position = effect.position,
            .eventType = effect.radarEventType,
            .stamp = nextStamp(),
            .fadeTick = dieTick - framesPerSecond / 2u,
            .dieTick = dieTick,
        });
        return true;
    }
    case ScriptMapPresentationCommand::SetBorderShroud:
        if (m_presentation.m_scriptMapPresentation.borderShroudEnabled() !=
            effect.enabled) {
            static_cast<void>(m_presentation.m_scriptMapPresentation.
                setBorderShroudEnabled(effect.enabled, nextStamp()));
        }
        return true;
    case ScriptMapPresentationCommand::SetRadarHidden:
        if (m_presentation.m_scriptMapPresentation.radarHidden() !=
            effect.enabled) {
            static_cast<void>(m_presentation.m_scriptMapPresentation.
                setRadarHidden(effect.enabled, nextStamp()));
        }
        return true;
    case ScriptMapPresentationCommand::SetRadarForced:
        if (m_presentation.m_scriptMapPresentation.radarForced() !=
            effect.enabled) {
            static_cast<void>(m_presentation.m_scriptMapPresentation.
                setRadarForced(effect.enabled, nextStamp()));
        }
        return true;
    default:
        return false;
    }
}

} // namespace engine::script
