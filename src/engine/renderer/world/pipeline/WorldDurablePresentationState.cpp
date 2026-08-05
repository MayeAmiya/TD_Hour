#include "engine/renderer/world/pipeline/WorldDurablePresentationState.h"

namespace engine::render {

void WorldDurablePresentationState::reset(
    uint64_t presentationEpoch) noexcept {
    m_skybox = {};
    m_skyboxSimulationFrame = 0;
    m_skyboxInitialized = false;
    m_treeSway = {};
    m_treeSwaySimulationFrame = 0;
    m_treeSwayInitialized = false;
    m_weather = {};
    m_weatherSimulationFrame = 0;
    m_weatherInitialized = false;
    m_cameraSlaveListener.reset();
    m_cameraSlaveEpoch = presentationEpoch;
    m_cameraSlaveSequence = 0;
    m_cameraSlaveSimulationFrame = 0;
}

template <typename State>
const State& WorldDurablePresentationState::consumeDurable(
    const State& incoming, uint64_t simulationFrame,
    State& accepted, uint64_t& acceptedSimulationFrame,
    bool& initialized) noexcept {
    if (incoming.presentationEpoch == 0) return incoming;
    if (!initialized || incoming.presentationEpoch > accepted.presentationEpoch) {
        accepted = incoming;
        acceptedSimulationFrame = simulationFrame;
        initialized = true;
    } else if (incoming.presentationEpoch == accepted.presentationEpoch &&
               (incoming.presentationSequence > accepted.presentationSequence ||
                (incoming.presentationSequence == accepted.presentationSequence &&
                 simulationFrame >= acceptedSimulationFrame))) {
        accepted = incoming;
        acceptedSimulationFrame = simulationFrame;
    }
    return accepted;
}

const SkyboxRenderState& WorldDurablePresentationState::consumeSkybox(
    const SkyboxRenderState& incoming, uint64_t simulationFrame) noexcept {
    return consumeDurable(
        incoming, simulationFrame, m_skybox,
        m_skyboxSimulationFrame, m_skyboxInitialized);
}

const TreeSwayRenderState& WorldDurablePresentationState::consumeTreeSway(
    const TreeSwayRenderState& incoming, uint64_t simulationFrame) noexcept {
    return consumeDurable(
        incoming, simulationFrame, m_treeSway,
        m_treeSwaySimulationFrame, m_treeSwayInitialized);
}

const WeatherRenderState& WorldDurablePresentationState::consumeWeather(
    const WeatherRenderState& incoming, uint64_t simulationFrame) noexcept {
    return consumeDurable(
        incoming, simulationFrame, m_weather,
        m_weatherSimulationFrame, m_weatherInitialized);
}

void WorldDurablePresentationState::consumeCameraSlaveListener(
    const CameraSlaveRenderState& incoming,
    const CameraSlavePresentationCamera& resolved,
    uint64_t simulationFrame) noexcept {
    if (incoming.presentationEpoch == 0) {
        m_cameraSlaveListener.reset();
        m_cameraSlaveEpoch = 0;
        m_cameraSlaveSequence = 0;
        m_cameraSlaveSimulationFrame = simulationFrame;
        return;
    }
    const bool newerEpoch = incoming.presentationEpoch > m_cameraSlaveEpoch;
    const bool sameEpochNewerValue =
        incoming.presentationEpoch == m_cameraSlaveEpoch &&
        (incoming.presentationSequence > m_cameraSlaveSequence ||
         (incoming.presentationSequence == m_cameraSlaveSequence &&
          simulationFrame >= m_cameraSlaveSimulationFrame));
    if (!newerEpoch && !sameEpochNewerValue) return;

    m_cameraSlaveEpoch = incoming.presentationEpoch;
    m_cameraSlaveSequence = incoming.presentationSequence;
    m_cameraSlaveSimulationFrame = simulationFrame;
    m_cameraSlaveListener = resolved.applied
        ? std::optional<RenderCameraSnapshot>{resolved.camera}
        : std::nullopt;
}

std::optional<RenderCameraSnapshot>
WorldDurablePresentationState::cameraSlaveListener(
    uint64_t expectedPresentationEpoch) const noexcept {
    if (expectedPresentationEpoch == 0 ||
        m_cameraSlaveEpoch != expectedPresentationEpoch) {
        return std::nullopt;
    }
    return m_cameraSlaveListener;
}

} // namespace engine::render
