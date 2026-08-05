#pragma once

#include <cstdint>
#include <optional>

namespace app::input {

struct PendingWorldTargetCapture final {
    uint64_t modeRevision = 0;
    uint64_t sessionRevision = 0;
};

class PendingWorldTargetCaptureState final {
public:
    void arm(uint64_t modeRevision, uint64_t sessionRevision) noexcept {
        m_armed = true;
        m_modeRevision = modeRevision;
        m_sessionRevision = sessionRevision;
    }

    [[nodiscard]] std::optional<PendingWorldTargetCapture> release() noexcept {
        if (!m_armed) return std::nullopt;
        const PendingWorldTargetCapture result{
            .modeRevision = m_modeRevision,
            .sessionRevision = m_sessionRevision,
        };
        cancel();
        return result;
    }

    void cancel() noexcept {
        m_armed = false;
        m_modeRevision = 0;
        m_sessionRevision = 0;
    }

    [[nodiscard]] bool armed() const noexcept { return m_armed; }
    [[nodiscard]] bool staleFor(uint64_t modeRevision) const noexcept {
        return m_modeRevision != 0 && m_modeRevision != modeRevision;
    }

private:
    bool m_armed = false;
    uint64_t m_modeRevision = 0;
    uint64_t m_sessionRevision = 0;
};

} // namespace app::input
