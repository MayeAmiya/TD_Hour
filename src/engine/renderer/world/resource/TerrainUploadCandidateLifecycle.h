#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/terrain/TerrainCompleteUploadCandidate.h"

namespace engine::render {

class TerrainUploadCandidateLifecycle final {
public:
    using Candidate = TerrainCompleteUploadCandidate;

    ~TerrainUploadCandidateLifecycle();

    void cancelAll();
    void retireCurrent();
    void reapRetired();

    [[nodiscard]] bool hasCurrent() const noexcept {
        return static_cast<bool>(m_current);
    }
    [[nodiscard]] Candidate* current() noexcept { return m_current.get(); }
    [[nodiscard]] const Candidate* current() const noexcept {
        return m_current.get();
    }
    void adopt(container::UniquePtr<Candidate> candidate) noexcept;
    void completeCurrent() noexcept;

    void resetState();
    void invalidateForTexturePolicy();
    void failCurrent();

    [[nodiscard]] bool fallbackActive() const noexcept {
        return m_fallbackActive;
    }
    void setFallbackActive(bool active) noexcept {
        m_fallbackActive = active;
    }

    [[nodiscard]] bool textureUpgradePending() const noexcept {
        return m_textureUpgradePending;
    }
    void requestTextureUpgrade(bool available) noexcept {
        m_textureUpgradePending = available;
    }
    void clearTextureUpgrade() noexcept {
        m_textureUpgradePending = false;
    }

private:
    container::UniquePtr<Candidate> m_current;
    container::Vector<container::UniquePtr<Candidate>> m_retired;
    bool m_fallbackActive = false;
    bool m_textureUpgradePending = false;
};

} // namespace engine::render
