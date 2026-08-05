#pragma once

#include "GameCommandQueue.h"
#include "LockstepFrameBuffer.h"

namespace engine {

class LocalFrameAuthority {
public:
    void reset();
    // Returns false when the deterministic local ingress rejected a duplicate
    // key or reached its explicit pending-command budget.
    [[nodiscard]] bool submit(GameCommand command);
    [[nodiscard]] GameCommandQueueSubmitResult submitResolved(
        GameCommand command);
    ConfirmedCommandFrame confirmFrame(GameTick tick);

    GameCommandQueue& commandQueue() { return m_commandQueue; }

private:
    GameCommandQueue m_commandQueue;
};

} // namespace engine
