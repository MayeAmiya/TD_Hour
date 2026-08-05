#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/model/W3dAssetCache.h"
#include "presentation/render/TerrainRenderSnapshot.h"

#include <cstdint>

namespace engine::render {

// Renderer-local diagnostic scene owner. Debug asset identity, terrain,
// showcase parameters and asynchronous preparation state reset together and
// never leak into production world snapshot ownership.
class DebugWorldSceneState final {
public:
    [[nodiscard]] bool hasVisualSource(bool fxRuntimeActive) const noexcept;
    [[nodiscard]] uint64_t nextSimulationFrame() noexcept;

    [[nodiscard]] W3dModelHandle model() const noexcept { return m_model; }
    void setModel(W3dModelHandle model) noexcept;
    [[nodiscard]] const container::SharedPtr<const TerrainRenderSnapshot>&
    terrain() const noexcept { return m_terrain; }
    void setTerrain(
        container::SharedPtr<const TerrainRenderSnapshot> terrain) noexcept;
    void clearSceneAssets() noexcept;

    void configureModelBounds(float radius) noexcept;
    [[nodiscard]] float instanceSpacing() const noexcept {
        return m_instanceSpacing;
    }
    [[nodiscard]] float boundingRadius() const noexcept {
        return m_boundingRadius;
    }

    [[nodiscard]] container::StringView animationState() const noexcept {
        return m_animationState;
    }
    void setAnimationState(container::String state);

    [[nodiscard]] bool materialEffectsEnabled() const noexcept {
        return m_materialEffectsEnabled;
    }
    void setMaterialEffectsEnabled(bool enabled) noexcept;
    [[nodiscard]] bool visibilityEnabled() const noexcept {
        return m_visibilityEnabled;
    }
    void setVisibilityEnabled(bool enabled) noexcept;

    [[nodiscard]] bool preparationPending() const noexcept {
        return m_preparationPending;
    }
    void setPreparationPending(bool pending) noexcept {
        m_preparationPending = pending;
    }
    void cancelPreparation() noexcept { m_preparationPending = false; }

    [[nodiscard]] bool failureReported() const noexcept {
        return m_failureReported;
    }
    void markFailureReported() noexcept { m_failureReported = true; }
    void clearFailureReport() noexcept { m_failureReported = false; }

private:
    W3dModelHandle m_model;
    container::SharedPtr<const TerrainRenderSnapshot> m_terrain;
    uint64_t m_simulationFrame = 0;
    float m_instanceSpacing = 2.0f;
    float m_boundingRadius = 4.0f;
    container::String m_animationState;
    bool m_materialEffectsEnabled = false;
    bool m_visibilityEnabled = false;
    bool m_preparationPending = false;
    bool m_failureReported = false;
};

} // namespace engine::render
