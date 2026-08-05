#pragma once

#include "engine/renderer/world/pipeline/WorldRenderer.h"
#include "presentation/render/RenderOverlaySnapshot.h"

#include <cstdint>
#include <optional>

namespace engine::render {

// Accepts durable script presentation values monotonically by
// (presentationEpoch, presentationSequence, simulationFrame). It is scene
// state, not an asset-cache concern.
class WorldDurablePresentationState final {
public:
    void reset(uint64_t presentationEpoch = 0) noexcept;

    [[nodiscard]] const SkyboxRenderState& consumeSkybox(
        const SkyboxRenderState& incoming, uint64_t simulationFrame) noexcept;
    [[nodiscard]] const TreeSwayRenderState& consumeTreeSway(
        const TreeSwayRenderState& incoming, uint64_t simulationFrame) noexcept;
    [[nodiscard]] const WeatherRenderState& consumeWeather(
        const WeatherRenderState& incoming, uint64_t simulationFrame) noexcept;
    void consumeCameraSlaveListener(
        const CameraSlaveRenderState& incoming,
        const CameraSlavePresentationCamera& resolved,
        uint64_t simulationFrame) noexcept;

    [[nodiscard]] std::optional<RenderCameraSnapshot> cameraSlaveListener(
        uint64_t expectedPresentationEpoch) const noexcept;

private:
    template <typename State>
    [[nodiscard]] static const State& consumeDurable(
        const State& incoming, uint64_t simulationFrame,
        State& accepted, uint64_t& acceptedSimulationFrame,
        bool& initialized) noexcept;

    SkyboxRenderState m_skybox;
    uint64_t m_skyboxSimulationFrame = 0;
    bool m_skyboxInitialized = false;
    TreeSwayRenderState m_treeSway;
    uint64_t m_treeSwaySimulationFrame = 0;
    bool m_treeSwayInitialized = false;
    WeatherRenderState m_weather;
    uint64_t m_weatherSimulationFrame = 0;
    bool m_weatherInitialized = false;
    std::optional<RenderCameraSnapshot> m_cameraSlaveListener;
    uint64_t m_cameraSlaveEpoch = 0;
    uint64_t m_cameraSlaveSequence = 0;
    uint64_t m_cameraSlaveSimulationFrame = 0;
};

} // namespace engine::render
