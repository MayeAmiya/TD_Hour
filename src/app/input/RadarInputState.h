#pragma once

#include "presentation/render/RenderWorldDescriptorContracts.h"

#include <optional>

namespace app::input {

class RadarInputState final {
public:
    void queueLookAt(engine::render::RenderVector world) noexcept {
        m_pendingLookAt = world;
    }

    void beginDrag(engine::render::RenderVector world) noexcept {
        m_pendingLookAt = world;
        m_dragging = true;
    }

    void endDrag() noexcept { m_dragging = false; }

    void cancel() noexcept {
        m_dragging = false;
        m_pendingLookAt.reset();
    }

    [[nodiscard]] bool dragging() const noexcept { return m_dragging; }

    [[nodiscard]] std::optional<engine::render::RenderVector>
    consumePendingLookAt() noexcept {
        std::optional<engine::render::RenderVector> value = m_pendingLookAt;
        m_pendingLookAt.reset();
        return value;
    }

private:
    bool m_dragging = false;
    std::optional<engine::render::RenderVector> m_pendingLookAt;
};

} // namespace app::input
