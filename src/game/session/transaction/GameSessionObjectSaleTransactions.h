#pragma once

#include "game/session/transaction/GameSessionTransactionPorts.h"

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// 售卖 admission 是对象状态、生产退款、容器、AI 与生命周期共同组成的
// 原子事务。所有玩家命令和脚本策略都必须进入这一实现。
class GameSessionObjectSaleTransactions final {
public:
    GameSessionObjectSaleTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionLifecycleTransactionPort barrier) noexcept;

    [[nodiscard]] bool beginObjectSale(
        ObjectId object, PlayerId player, uint64_t confirmedTick,
        bool respectScriptUnsellable = false);
    void settleDueSales(uint64_t confirmedTick);

private:
    struct StateView final {
        GameSessionContentStartState& content;
        GameSessionWorldState& world;
        GameSessionAIState& ai;
        GameSessionScriptPresentationState& presentation;
        GameSessionContentStartState& contentState() noexcept { return content; }
        GameSessionWorldState& worldState() noexcept { return world; }
        GameSessionAIState& aiState() noexcept { return ai; }
        GameSessionScriptPresentationState& presentationState() noexcept {
            return presentation;
        }
    };

    [[nodiscard]] StateView& domainState() noexcept { return m_state; }
    [[nodiscard]] bool requestDestroyObject(
        ObjectId object, ObjectDestroyReason reason,
        uint64_t confirmedTick);

    StateView m_state;
    GameSessionLifecycleTransactionPort m_barrier;
};

} // namespace engine
