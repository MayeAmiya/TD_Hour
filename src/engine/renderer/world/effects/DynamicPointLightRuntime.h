#pragma once

#include "core/container/container_types.h"

#include "engine/fx/runtime/FxPresentationCommands.h"
#include "presentation/render/DynamicLightPerformanceSettings.h"
#include "presentation/render/DynamicLightVisualSettings.h"
#include "core/math/wwmath/vector/float3.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
namespace engine::render {

struct DynamicPointLightRenderData final {
    math::vec3 position{};
    math::vec3 color{};
    math::vec3 ambientColor{};
    float innerRadius = dynamic_lights::visual_defaults::kInnerRadius;
    float outerRadius = dynamic_lights::visual_defaults::kInnerRadius;
    bool hasSeparateAmbientColor = false;
};

struct DynamicPointLightRuntimeStats final {
    uint32_t activeLights = 0;
    uint32_t permanentLights = 0;
    uint32_t highWaterLights = 0;
    uint64_t admittedCommands = 0;
    uint64_t invalidCommands = 0;
    uint64_t budgetRejectedCommands = 0;
    uint64_t futureCommands = 0;
    uint64_t expiredLights = 0;
    uint64_t epochResets = 0;
    uint64_t rollbackResets = 0;
    uint64_t staleEpochBatches = 0;
    uint64_t staleEpochAdvances = 0;
};

// Renderer-owned LightPulse lifetime. It consumes detached typed FX commands,
// advances only from sealed 30 Hz simulation frames and exposes a bounded
// value-only light array to WorldRenderer. No ECS object or GPU handle enters
// this owner.
class DynamicPointLightRuntime final {
public:
    explicit DynamicPointLightRuntime(
        DynamicLightRenderBudget budget = {}) noexcept
        : m_budget(budget) {
        m_pulses.reserve(m_budget.boundedMaximumLights());
        m_renderLights.reserve(m_budget.boundedMaximumLights());
    }

    void reset(uint64_t sessionEpoch = 0) noexcept {
        const bool changedEpoch = m_sessionEpoch != 0 &&
            sessionEpoch != m_sessionEpoch;
        m_pulses.clear();
        m_renderLights.clear();
        m_sessionEpoch = sessionEpoch;
        m_lastSimulationFrame = 0;
        m_hasAdvancedFrame = false;
        const uint64_t epochResets =
            m_stats.epochResets + (changedEpoch ? 1u : 0u);
        m_stats = {};
        m_stats.epochResets = epochResets;
    }

    void submit(uint64_t sessionEpoch,
                container::Span<const fx::FxLightPulseCommand> commands) {
        if (sessionEpoch == 0) {
            reset();
            return;
        }
        if (m_sessionEpoch != 0 && sessionEpoch < m_sessionEpoch) {
            ++m_stats.staleEpochBatches;
            return;
        }
        if (sessionEpoch != m_sessionEpoch) reset(sessionEpoch);

        // FxRuntime already admits each invocation exactly once before this
        // drained batch reaches the renderer.  eventId identifies the whole
        // invocation, not an individual nugget, so event-level de-duplication
        // here would incorrectly collapse authored multi-LightPulse FXLists.
        for (const fx::FxLightPulseCommand& command : commands) {
            if (!valid(command)) {
                ++m_stats.invalidCommands;
                continue;
            }
            if (m_pulses.size() >= m_budget.boundedMaximumLights()) {
                ++m_stats.budgetRejectedCommands;
                continue;
            }

            const fx::ParticleVector3 source =
                fx::worldTransform(command.anchor).position;
            constexpr float inverseByte = 1.0f / 255.0f;
            Pulse pulse;
            pulse.startFrame = command.identity.confirmedFrame;
            pulse.position = {source.x, source.y, source.z};
            pulse.targetColor = {
                static_cast<float>(command.color.red) * inverseByte,
                static_cast<float>(command.color.green) * inverseByte,
                static_cast<float>(command.color.blue) * inverseByte,
            };
            pulse.increaseFrames =
                dynamic_lights::performance_limits::framesFromMilliseconds(
                    command.increaseTimeMilliseconds);
            pulse.decreaseFrames = command.decreaseTimeMilliseconds == 0
                ? 0u
                : dynamic_lights::performance_limits::framesFromMilliseconds(
                      command.decreaseTimeMilliseconds);
            pulse.targetOuterRadius =
                dynamic_lights::visual_defaults::kInnerRadius +
                std::clamp(
                    command.radius,
                    dynamic_lights::visual_defaults::kMinimumAttenuationWidth,
                    dynamic_lights::visual_defaults::kMaximumAttenuationWidth);
            m_pulses.push_back(pulse);
            ++m_stats.admittedCommands;
        }
        m_stats.highWaterLights = std::max<uint32_t>(
            m_stats.highWaterLights,
            static_cast<uint32_t>(m_pulses.size()));
    }

    [[nodiscard]] container::Span<const DynamicPointLightRenderData> advance(
        uint64_t sessionEpoch, uint64_t simulationFrame) {
        if (sessionEpoch == 0) {
            reset();
            return {};
        }
        if (m_sessionEpoch != 0 && sessionEpoch < m_sessionEpoch) {
            ++m_stats.staleEpochAdvances;
            return {};
        }
        if (sessionEpoch != m_sessionEpoch) reset(sessionEpoch);
        if (m_hasAdvancedFrame && simulationFrame < m_lastSimulationFrame) {
            const uint64_t rollbackResets = m_stats.rollbackResets + 1u;
            m_pulses.clear();
            m_renderLights.clear();
            m_stats.activeLights = 0;
            m_stats.permanentLights = 0;
            m_stats.rollbackResets = rollbackResets;
        }
        m_hasAdvancedFrame = true;
        m_lastSimulationFrame = simulationFrame;
        m_renderLights.clear();
        m_stats.futureCommands = 0;
        m_stats.permanentLights = 0;

        auto pulse = m_pulses.begin();
        while (pulse != m_pulses.end()) {
            const Factor factor = factorAt(*pulse, simulationFrame);
            if (factor.future) {
                ++m_stats.futureCommands;
                ++pulse;
                continue;
            }
            if (factor.expired) {
                ++m_stats.expiredLights;
                pulse = m_pulses.erase(pulse);
                continue;
            }
            if (pulse->decreaseFrames == 0) ++m_stats.permanentLights;
            const float value = std::clamp(factor.value, 0.0f, 1.0f);
            m_renderLights.push_back({
                .position = pulse->position,
                .color = pulse->targetColor * value,
                .innerRadius =
                    dynamic_lights::visual_defaults::kInnerRadius,
                .outerRadius = std::max(
                    dynamic_lights::visual_defaults::kInnerRadius,
                    pulse->targetOuterRadius * value),
            });
            ++pulse;
        }
        m_stats.activeLights = static_cast<uint32_t>(m_renderLights.size());
        return m_renderLights;
    }

    [[nodiscard]] const DynamicPointLightRuntimeStats& stats() const noexcept {
        return m_stats;
    }
    [[nodiscard]] uint64_t sessionEpoch() const noexcept {
        return m_sessionEpoch;
    }

private:
    struct Pulse final {
        uint64_t startFrame = 0;
        math::vec3 position{};
        math::vec3 targetColor{};
        float targetOuterRadius =
            dynamic_lights::visual_defaults::kInnerRadius;
        uint32_t increaseFrames = 0;
        uint32_t decreaseFrames = 0;
    };
    struct Factor final {
        float value = 0.0f;
        bool future = false;
        bool expired = false;
    };

    [[nodiscard]] static bool valid(
        const fx::FxLightPulseCommand& command) noexcept {
        const fx::ParticleVector3 position =
            fx::worldTransform(command.anchor).position;
        return std::isfinite(position.x) && std::isfinite(position.y) &&
            std::isfinite(position.z) && std::isfinite(command.radius) &&
            command.radius > 0.0f;
    }

    [[nodiscard]] static Factor factorAt(
        const Pulse& pulse, uint64_t simulationFrame) noexcept {
        if (simulationFrame < pulse.startFrame) {
            return {.future = true};
        }
        const uint64_t age = simulationFrame - pulse.startFrame + 1u;
        if (pulse.increaseFrames != 0 && age <= pulse.increaseFrames) {
            return {
                .value = static_cast<float>(age) /
                    static_cast<float>(pulse.increaseFrames),
            };
        }
        if (pulse.decreaseFrames == 0) return {.value = 1.0f};
        const uint64_t decayStep = age - pulse.increaseFrames;
        if (decayStep >= pulse.decreaseFrames) return {.expired = true};
        return {
            .value = static_cast<float>(pulse.decreaseFrames - decayStep) /
                static_cast<float>(pulse.decreaseFrames),
        };
    }

    DynamicLightRenderBudget m_budget;
    uint64_t m_sessionEpoch = 0;
    uint64_t m_lastSimulationFrame = 0;
    bool m_hasAdvancedFrame = false;
    container::Vector<Pulse> m_pulses;
    container::Vector<DynamicPointLightRenderData> m_renderLights;
    DynamicPointLightRuntimeStats m_stats;
};

} // namespace engine::render
