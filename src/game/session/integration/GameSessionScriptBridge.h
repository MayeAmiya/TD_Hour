#pragma once

#include "core/container/container_types.h"

#include "game/player/PlayerTypes.h"
#include "game/script/runtime/ScriptRuntime.h"
#include "game/session/integration/GameSessionScriptAuthorityPort.h"
#include "game/session/integration/GameSessionScriptPresentationPort.h"
#include "game/session/integration/GameSessionScriptQueryPort.h"

#include <cstdint>
#include <optional>
namespace engine {
class GameSessionLifecycleTransactionPort;
}

namespace engine::script {

// Session adapter for the value-only ScriptRuntime contract. ScriptRuntime
// emits detached effects; this bridge commits each accepted effect immediately
// in that same source-order call stack. It never calls back into ScriptRuntime,
// so later actions/subroutines observe prior authoritative mutations just as
// RefCode's ScriptEngine::executeActions does, without exposing ECS to the
// immutable runtime.
class GameSessionScriptBridge final : public ScriptRandomSource,
                                      public ScriptEffectSink {
public:
    GameSessionScriptBridge(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        GameSessionLifecycleTransactionPort lifecycle,
        uint64_t confirmedTick,
        container::Span<const ObjectId> localSelection = {});

    [[nodiscard]] GameSessionScriptQueryPort& queries() noexcept {
        return m_queries;
    }

    [[nodiscard]] int32_t integerInclusive(int32_t lo, int32_t hi) noexcept override;
    void emit(ScriptEffect effect) override;

    // Effects are synchronously committed by emit(). This reports whether an
    // invalid stamped effect was rejected; accepted earlier effects remain
    // committed in source order.
    [[nodiscard]] bool flush();
    [[nodiscard]] bool hasRejectedEffects() const noexcept { return m_rejectedEffects; }

private:
    void apply(const ScriptEffect& effect);

    GameSessionScriptLocalPresentationState m_localPresentation;
    GameSessionScriptQueryPort m_queries;
    GameSessionScriptAuthorityPort m_authority;
    GameSessionScriptPresentationPort m_presentation;
    uint64_t m_confirmedTick = 0;
    uint32_t m_lastOrdinal = 0;
    bool m_hasOrdinal = false;
    bool m_rejectedEffects = false;
};

} // namespace engine::script
