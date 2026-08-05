#pragma once

#include "game/session/ai/GameSessionAISnapshot.h"
#include "game/session/state/GameSessionDomainState.h"

namespace engine {


// Focused AI projection and full AI rollback snapshot contract. It holds only
// a non-owning link to the Session-owned state root; callers never see raw
// ECS/player/terrain authority accessors.
class GameSessionAIDomain {
public:
    explicit GameSessionAIDomain(GameSessionStateRoot& state) noexcept
        : m_state(&state) {}
    [[nodiscard]] script::ScriptSequentialAuthorityState sequentialObjectState(
        ObjectId object) const noexcept;
    [[nodiscard]] ObjectAISimulationDigest objectAISimulationDigest() const;
    [[nodiscard]] ObjectAIWorldSnapshotStatus captureObjectAIWorldSnapshot(
        ObjectAIWorldSnapshot& output) const;
    [[nodiscard]] ObjectAIWorldSnapshotStatus restoreObjectAIWorldSnapshot(
        const ObjectAIWorldSnapshot& snapshot);
protected:
    [[nodiscard]] GameSessionStateRoot& domainState() noexcept {
        return *m_state;
    }
    [[nodiscard]] const GameSessionStateRoot& domainState() const noexcept {
        return *m_state;
    }

private:
    GameSessionStateRoot* m_state = nullptr;
};

} // namespace engine
