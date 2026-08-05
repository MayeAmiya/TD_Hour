#include "core/container/container_types.h"
#include "GameSessionScriptBridge.h"


namespace engine::script {

GameSessionScriptBridge::GameSessionScriptBridge(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionObjectEventState& objectEvents,
    GameSessionLifecycleTransactionPort lifecycle,
    uint64_t confirmedTick,
    container::Span<const ObjectId> localSelection)
    : m_localPresentation(localSelection),
      m_queries(
          content, world, ai, presentation, objectEvents,
          confirmedTick, m_localPresentation),
      m_authority(
          content, world, ai, presentation, lifecycle, m_queries,
          confirmedTick),
      m_presentation(
          content, world, presentation, m_queries,
          m_localPresentation, confirmedTick),
      m_confirmedTick(confirmedTick) {}


int32_t GameSessionScriptBridge::integerInclusive(int32_t lo, int32_t hi) noexcept {
    return m_authority.integerInclusive(lo, hi);
}

void GameSessionScriptBridge::emit(ScriptEffect effect) {
    // Runtime effects are generated in one call stack and carry a monotonic
    // ordinal. Reject malformed producers rather than sorting/replaying them
    // later and thereby hiding a determinism violation. A rejected effect
    // never rolls back the already committed source-order prefix.
    if (effect.header.confirmedTick != m_confirmedTick ||
        (m_hasOrdinal && effect.header.ordinal != m_lastOrdinal + 1u)) {
        m_rejectedEffects = true;
        ScriptEffectHeader header = effect.header;
        if (header.confirmedTick == 0) header.confirmedTick = m_confirmedTick;
        m_authority.emitDiagnostic(
            header, "Rejected malformed ScriptEffect order/tick");
        return;
    }
    m_lastOrdinal = effect.header.ordinal;
    m_hasOrdinal = true;

    // RefCode's executeActions invokes each ScriptActions method immediately.
    // Keep the modern pure-runtime boundary, but commit the detached value at
    // this one bridge point so later actions, CALL_SUBROUTINE and later
    // ScriptLists observe the actual Session state rather than a partial
    // overlay. apply() never re-enters ScriptRuntime.
    apply(effect);
    if (std::holds_alternative<ScriptDamageEffect>(effect.payload)) {
        // Damage has a separate Body/ObjectSimulation ingress. Drain this
        // exact stamped batch before the next script effect, so a lethal
        // DAMAGE is visible to a following condition, subroutine or DELETE.
        m_authority.resolveQueuedObjectDamage();
    }
}

bool GameSessionScriptBridge::flush() {
    // emit() already committed every accepted effect. Keep this explicit
    // frame-end check so GameSession preserves its existing diagnostic API.
    return !m_rejectedEffects;
}

void GameSessionScriptBridge::apply(const ScriptEffect& effect) {
    if (m_authority.applyObjectOrPlayerPolicy(effect)) return;
    if (m_presentation.apply(effect)) return;
    static_cast<void>(m_authority.applyOrderAndAi(effect));
}

} // namespace engine::script
