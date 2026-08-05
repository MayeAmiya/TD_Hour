#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/script/GameSessionScriptFrameTransactions.h"
#include "game/session/frame/GameSessionGameplayEventCollector.h"

#include "debug/debug.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/session/integration/GameSessionScriptBridge.h"

#include <algorithm>
#include <cctype>
#include <variant>

namespace engine {
void GameSessionScriptFrameTransactions::refreshAreaTransitions() {
    const container::Span<const container::String> areaNames =
        m_presentation.m_scriptGameplayEvents.trackedAreas();
    m_presentation.m_scriptGameplayEvents.beginAreaRefresh();
    if (areaNames.empty()) {
        m_presentation.m_scriptGameplayEvents.finishAreaRefresh();
        return;
    }
    container::Vector<const game::terrain::PolygonTriggerRecord*> areas;
    areas.reserve(areaNames.size());
    for (const container::String& name : areaNames) {
        areas.push_back(m_content.m_terrain.triggerByName(name));
    }
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto objects = ecs::view<const ObjectIdentityComponent,
                                   const TransformComponent,
                                   const ObjectLifecycleComponent>(m_world.m_registry);
    candidates.reserve(objects.size_hint());
    for (const ecs::entity entity : objects) {
        const auto& identity =
            objects.template get<const ObjectIdentityComponent>(entity);
        const auto& lifecycle =
            objects.template get<const ObjectLifecycleComponent>(entity);
        if (!identity.id || lifecycle.phase != ObjectLifecyclePhase::Alive ||
            ecs::try_get<ObjectContainedByComponent>(m_world.m_registry, entity)) continue;
        const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, entity);
        if (mapStatus && mapStatus->offMap) continue;
        candidates.push_back({.object = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    for (const Candidate& candidate : candidates) {
        const TransformComponent& transform =
            ecs::get<TransformComponent>(m_world.m_registry, candidate.entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, candidate.entity,
            transform);
        for (uint32_t areaIndex = 0; areaIndex < areas.size(); ++areaIndex) {
            const game::terrain::PolygonTriggerRecord* area = areas[areaIndex];
            const bool inside = area && m_content.m_terrain.isInsideTriggerLegacyRaw(
                *area, position.x.raw(), position.y.raw());
            m_presentation.m_scriptGameplayEvents.recordAreaSample(
                candidate.object, areaIndex, inside,
                m_presentation.m_confirmedTick);
        }
    }
    m_presentation.m_scriptGameplayEvents.finishAreaRefresh();
}

bool GameSessionScriptFrameTransactions::acceptsLocalPresentationCompletion()
    const noexcept {
    return m_content.m_active && !m_content.m_startInfo.network.enabled &&
        m_content.m_startInfo.mode != GameMode::Replay;
}

void GameSessionScriptFrameTransactions::drainPresentationCompletions() {
    if (!acceptsLocalPresentationCompletion()) {
        m_presentation.m_pendingScriptPresentationCompletions.clear();
        m_presentation.m_pendingScriptMusicLoops.clear();
        return;
    }

    for (script::ScriptPresentationCompletion& completion :
         m_presentation.m_pendingScriptPresentationCompletions) {
        // Movie actions complete synchronously in
        // GameSessionScriptPresentationPort.  A backend must never inject a
        // delayed Video completion here, both because there is no video
        // playback implementation and because that would make a script
        // condition depend on wall-clock presentation state.
        if (completion.kind ==
            script::ScriptPresentationCompletionKind::Video) {
            continue;
        }
        static_cast<void>(
            m_presentation.m_scriptPresentationCompletions.recordOneShot(
                std::move(completion)));
    }
    m_presentation.m_pendingScriptPresentationCompletions.clear();

    for (const container::String& trackName :
         m_presentation.m_pendingScriptMusicLoops) {
        static_cast<void>(
            m_presentation.m_scriptPresentationCompletions.recordMusicLoop(
                trackName, m_presentation.m_scriptPresentationEpoch));
    }
    m_presentation.m_pendingScriptMusicLoops.clear();
}

script::ScriptRuntimeStepResult GameSessionScriptFrameTransactions::advance(
    uint64_t scriptTick, container::Span<const ObjectId> localSelection,
    uint64_t worldConfirmedTick) {
    script::ScriptRuntimeStepResult result;
    if (!m_content.m_active) return result;
    if (!m_presentation.m_scriptRuntime.program()) return result;
    // VICTORY/DEFEAT/QUICKVICTORY finish their current action chain, then
    // ScriptEngine returns before timer/condition/script work on every later
    // update while its end-game timer counts down. Presentation may use the
    // stored MissionEndMode for its own close/exit timing; simulation must
    // never resume this ScriptRuntime after a terminal action.
    if (m_presentation.m_missionState.state() !=
        scenario::MissionTerminalState::Running) return result;
    const uint64_t effectTick = worldConfirmedTick != 0
        ? worldConfirmedTick : scriptTick;
    if (!m_presentation.m_hasConfirmedFrame ||
        effectTick != m_presentation.m_confirmedTick) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::ScriptRuntime,
            .code = SimulationFaultCode::ScriptTickRejected,
            .confirmedTick = effectTick,
        }));
        TD_LOG_WARN("[GameSession] Refused script advance {} for world tick {} outside active confirmed frame {}",
                    scriptTick, effectTick,
                    m_presentation.m_confirmedTick);
        return result;
    }

    // Audio and speech backends may acknowledge natural completion between
    // confirmed ticks.  Commit those validated facts only after this call has
    // crossed confirmed-frame ingress.  Video is excluded: MOVIE_PLAY_* is
    // a synchronous compatibility completion written directly by the script
    // bridge.  This remains active while FREEZE_TIME holds world systems,
    // because ScriptRuntime itself continues advancing then.
    drainPresentationCompletions();
    refreshAreaTransitions();
    GameSessionGameplayEventCollector{
        m_content, m_world, m_presentation, m_publication}
        .collectSpecialPowerProducerEvents();

    script::GameSessionScriptBridge bridge(
        m_content, m_world, m_ai, m_presentation, m_objectEvents,
        m_lifecycle, effectTick, localSelection);
    m_presentation.m_scriptRuntime.setContext(
        {.world = &bridge.queries(), .random = &bridge});
    result = m_presentation.m_scriptRuntime.advanceConfirmedTick(
        scriptTick, bridge);
    const bool flushed = bridge.flush();
    m_presentation.m_scriptRuntime.setContext({});

    if (!result.accepted) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::ScriptRuntime,
            .code = SimulationFaultCode::ScriptTickRejected,
            .confirmedTick = effectTick,
        }));
        TD_LOG_WARN("[GameSession] Script runtime rejected script tick {} at world tick {}",
                    scriptTick, effectTick);
    }
    if (result.recursionLimitReached) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::ScriptRuntime,
            .code = SimulationFaultCode::ScriptRecursionLimit,
            .confirmedTick = effectTick,
        }));
    }
    if (!flushed) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::ScriptRuntime,
            .code = SimulationFaultCode::MalformedScriptEffect,
            .confirmedTick = effectTick,
        }));
    }
    if (result.recursionLimitReached || !flushed) {
        TD_LOG_WARN("[GameSession] Script runtime diagnostic at tick {} (recursionLimit={} malformedEffects={})",
                    scriptTick, result.recursionLimitReached,
                    bridge.hasRejectedEffects());
    }
    return result;
}

} // namespace engine
