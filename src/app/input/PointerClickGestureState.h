#pragma once

#include <cmath>
#include <cstdint>

namespace app::input {

struct PointerClickCompletion final {
    float screenX = 0.0f;
    float screenY = 0.0f;
    uint64_t sessionRevision = 0;
    uint8_t clickCount = 0;
    bool click = false;
};

// Owns one press/move/release click gesture. It contains no SDL, camera,
// Session or command policy and can therefore be reused by world-command and
// default deselection input without duplicating gesture lifetime fields.
class PointerClickGestureState final {
public:
    [[nodiscard]] bool begin(
        float x, float y, uint64_t timestampMilliseconds,
        uint64_t sessionRevision, uint8_t clickCount = 1) noexcept {
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        m_armed = true;
        m_startX = m_currentX = x;
        m_startY = m_currentY = y;
        m_startedMilliseconds = timestampMilliseconds;
        m_sessionRevision = sessionRevision;
        m_clickCount = clickCount;
        return true;
    }

    [[nodiscard]] bool update(float x, float y) noexcept {
        if (!m_armed || !std::isfinite(x) || !std::isfinite(y)) return false;
        m_currentX = x;
        m_currentY = y;
        return true;
    }

    [[nodiscard]] PointerClickCompletion complete(
        float x, float y, uint64_t releasedMilliseconds,
        float tolerancePixels,
        uint64_t toleranceMilliseconds) noexcept {
        static_cast<void>(update(x, y));
        const uint64_t elapsed = releasedMilliseconds >= m_startedMilliseconds
            ? releasedMilliseconds - m_startedMilliseconds
            : UINT64_MAX;
        PointerClickCompletion result{
            .screenX = m_currentX,
            .screenY = m_currentY,
            .sessionRevision = m_sessionRevision,
            .clickCount = m_clickCount,
            .click = m_armed &&
                std::abs(m_currentX - m_startX) < tolerancePixels &&
                std::abs(m_currentY - m_startY) < tolerancePixels &&
                elapsed <= toleranceMilliseconds,
        };
        cancel();
        return result;
    }

    void cancel() noexcept {
        m_armed = false;
        m_startedMilliseconds = 0;
        m_sessionRevision = 0;
        m_clickCount = 0;
    }

    [[nodiscard]] bool armed() const noexcept { return m_armed; }

private:
    bool m_armed = false;
    float m_startX = 0.0f;
    float m_startY = 0.0f;
    float m_currentX = 0.0f;
    float m_currentY = 0.0f;
    uint64_t m_startedMilliseconds = 0;
    uint64_t m_sessionRevision = 0;
    uint8_t m_clickCount = 0;
};

} // namespace app::input
