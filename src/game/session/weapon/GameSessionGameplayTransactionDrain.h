#pragma once

namespace engine {
class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionFrameCommitState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;
class GameSessionLifecycleTransactionPort;
namespace detail {

// 关闭当前 confirmed gameplay 因果栈。它是生命周期、伤害、武器和帧编排
// 共用的事务屏障，不是 ScriptInterface 的对外能力。
class GameSessionGameplayTransactionDrain final {
public:
    static void run(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        GameSessionFrameCommitState& frame,
        GameSessionLifecycleTransactionPort lifecycle);
};

} // namespace detail
} // namespace engine
