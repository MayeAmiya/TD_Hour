#pragma once

#include "game/selection/LocalPlacementAnchorInput.h"

#include <optional>

namespace app::input {

struct PlacementPointerSample final {
    engine::selection::LocalPlacementScreenPoint start;
    engine::selection::LocalPlacementScreenPoint end;
    bool confirm = false;
    bool forceAttackSnap = false;
    bool queueConstruction = false;
};

class PlacementInputState final {
public:
    [[nodiscard]] bool begin(
        engine::selection::LocalPlacementScreenPoint point) noexcept {
        m_pendingConfirmation.reset();
        return m_anchor.begin(point);
    }

    [[nodiscard]] bool update(
        engine::selection::LocalPlacementScreenPoint point) noexcept {
        return m_anchor.update(point);
    }

    void release(engine::selection::LocalPlacementScreenPoint point,
                 bool forceAttackSnap,
                 bool queueConstruction) noexcept {
        m_pendingConfirmation = m_anchor.release(point);
        m_pendingForceAttack = forceAttackSnap;
        m_pendingQueueConstruction = queueConstruction;
    }

    void cancel() noexcept {
        m_anchor.cancel();
        m_pendingConfirmation.reset();
        m_pendingForceAttack = false;
        m_pendingQueueConstruction = false;
    }

    [[nodiscard]] bool active() const noexcept { return m_anchor.active(); }

    [[nodiscard]] PlacementPointerSample sample(
        engine::selection::LocalPlacementScreenPoint fallback,
        bool currentForceAttack) const noexcept {
        if (m_pendingConfirmation) {
            return {
                .start = m_pendingConfirmation->start,
                .end = m_pendingConfirmation->end,
                .confirm = true,
                .forceAttackSnap = m_pendingForceAttack,
                .queueConstruction = m_pendingQueueConstruction,
            };
        }
        if (m_anchor.active()) {
            return {
                .start = m_anchor.start(),
                .end = m_anchor.end(),
                .forceAttackSnap = currentForceAttack,
            };
        }
        return {.start = fallback, .end = fallback};
    }

    void acknowledgeConfirmation() noexcept {
        m_pendingConfirmation.reset();
        m_pendingForceAttack = false;
        m_pendingQueueConstruction = false;
    }

private:
    engine::selection::LocalPlacementAnchorInput m_anchor;
    std::optional<engine::selection::LocalPlacementAnchorConfirmation>
        m_pendingConfirmation;
    bool m_pendingForceAttack = false;
    bool m_pendingQueueConstruction = false;
};

} // namespace app::input
