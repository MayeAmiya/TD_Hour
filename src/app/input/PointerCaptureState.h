#pragma once

#include <cstdint>

namespace app::input {

enum class PointerCaptureOwner : uint8_t {
    None,
    Gui,
    Placement,
    Radar,
    Selection,
    Camera,
    WorldCommand,
    PendingWorldCommand,
};

class PointerCaptureState final {
public:
    [[nodiscard]] bool begin(PointerCaptureOwner owner, uint8_t button,
                             uint64_t sessionRevision) noexcept {
        if (active() || owner == PointerCaptureOwner::None || button == 0)
            return false;
        m_owner = owner;
        m_button = button;
        m_sessionRevision = sessionRevision;
        return true;
    }

    void clear() noexcept {
        m_owner = PointerCaptureOwner::None;
        m_button = 0;
        m_sessionRevision = 0;
    }

    [[nodiscard]] bool active() const noexcept {
        return m_owner != PointerCaptureOwner::None;
    }
    [[nodiscard]] bool ownedBy(PointerCaptureOwner owner) const noexcept {
        return m_owner == owner;
    }
    [[nodiscard]] PointerCaptureOwner owner() const noexcept { return m_owner; }
    [[nodiscard]] uint8_t button() const noexcept { return m_button; }
    [[nodiscard]] uint64_t sessionRevision() const noexcept {
        return m_sessionRevision;
    }
    [[nodiscard]] bool stale(bool hasSession,
                             uint64_t currentRevision) const noexcept {
        if (!active() || ownedBy(PointerCaptureOwner::Gui)) return false;
        return m_sessionRevision == 0
            ? hasSession
            : (!hasSession || m_sessionRevision != currentRevision);
    }

private:
    PointerCaptureOwner m_owner = PointerCaptureOwner::None;
    uint8_t m_button = 0;
    uint64_t m_sessionRevision = 0;
};

} // namespace app::input
