#include "engine/renderer/world/pipeline/DebugWorldSceneState.h"

#include <algorithm>
#include <utility>

namespace engine::render {

bool DebugWorldSceneState::hasVisualSource(bool fxRuntimeActive) const
    noexcept {
    return static_cast<bool>(m_model) || static_cast<bool>(m_terrain) ||
        fxRuntimeActive;
}

uint64_t DebugWorldSceneState::nextSimulationFrame() noexcept {
    ++m_simulationFrame;
    if (m_simulationFrame == 0) ++m_simulationFrame;
    return m_simulationFrame;
}

void DebugWorldSceneState::setModel(W3dModelHandle model) noexcept {
    m_model = model;
    m_preparationPending = false;
    m_failureReported = false;
}

void DebugWorldSceneState::setTerrain(
    container::SharedPtr<const TerrainRenderSnapshot> terrain) noexcept {
    m_terrain = std::move(terrain);
    m_preparationPending = false;
}

void DebugWorldSceneState::clearSceneAssets() noexcept {
    m_model = {};
    m_terrain.reset();
    m_simulationFrame = 0;
    m_preparationPending = false;
    m_failureReported = false;
}

void DebugWorldSceneState::configureModelBounds(float radius) noexcept {
    m_boundingRadius = std::max(radius, 0.25f);
    m_instanceSpacing = m_boundingRadius * 2.4f;
}

void DebugWorldSceneState::setAnimationState(container::String state) {
    m_animationState = std::move(state);
}

void DebugWorldSceneState::setMaterialEffectsEnabled(bool enabled) noexcept {
    m_materialEffectsEnabled = enabled;
    m_preparationPending = false;
}

void DebugWorldSceneState::setVisibilityEnabled(bool enabled) noexcept {
    m_visibilityEnabled = enabled;
    m_preparationPending = false;
}

} // namespace engine::render
