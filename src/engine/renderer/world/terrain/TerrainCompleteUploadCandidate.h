#pragma once

#include "core/container/container_types.h"

namespace engine::render {

class D3D12TerrainVisual;

// CPU/GPU staged terrain-build lifetime token. It is independent of the
// published terrain visual and can be retired while that visual remains live.
class TerrainCompleteUploadCandidate final {
public:
    ~TerrainCompleteUploadCandidate();

    TerrainCompleteUploadCandidate(
        const TerrainCompleteUploadCandidate&) = delete;
    TerrainCompleteUploadCandidate& operator=(
        const TerrainCompleteUploadCandidate&) = delete;

    void requestCancel() noexcept;
    [[nodiscard]] bool readyToDestroy() const noexcept;

private:
    friend class D3D12TerrainVisual;
    struct Impl;
    explicit TerrainCompleteUploadCandidate(
        container::UniquePtr<Impl> impl) noexcept;
    container::UniquePtr<Impl> m_impl;
};

} // namespace engine::render
