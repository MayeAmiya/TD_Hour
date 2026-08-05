#pragma once

#include "core/math/fixed/q32_32.h"

#include <optional>

namespace engine::selection {

struct LocalPlacementScreenPoint final {
    float x = 0.0f;
    float y = 0.0f;
};

struct LocalPlacementAnchorConfirmation final {
    LocalPlacementScreenPoint start;
    LocalPlacementScreenPoint end;
    bool dragged = false;
};

// Local mouse gesture state matching PlaceEventTranslator's anchored build
// placement. It stores screen points so camera/resize changes reproject both
// anchors through the current tactical view instead of retaining stale world
// coordinates. No ECS, command, or renderer state is owned here.
class LocalPlacementAnchorInput final {
public:
    static constexpr float DragThresholdPixels = 5.0f;

    [[nodiscard]] bool begin(LocalPlacementScreenPoint point) noexcept;
    [[nodiscard]] bool update(LocalPlacementScreenPoint point) noexcept;
    [[nodiscard]] std::optional<LocalPlacementAnchorConfirmation>
        release(LocalPlacementScreenPoint point) noexcept;
    void cancel() noexcept;

    [[nodiscard]] bool active() const noexcept { return m_active; }
    [[nodiscard]] bool dragged() const noexcept { return m_dragged; }
    [[nodiscard]] LocalPlacementScreenPoint start() const noexcept {
        return m_start;
    }
    [[nodiscard]] LocalPlacementScreenPoint end() const noexcept {
        return m_end;
    }

private:
    [[nodiscard]] bool admitEnd(LocalPlacementScreenPoint point) noexcept;

    LocalPlacementScreenPoint m_start;
    LocalPlacementScreenPoint m_end;
    bool m_active = false;
    bool m_dragged = false;
};

// The original convenience tweak snaps only while force-attack mode is
// active, after screen anchors have been projected to a world-space yaw.
[[nodiscard]] math::q32_32 snapLocalPlacementYaw45Fixed(
    math::q32_32 yawRadians, bool forceAttackMode) noexcept;

} // namespace engine::selection
