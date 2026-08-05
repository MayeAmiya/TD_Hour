#include "core/container/container_types.h"
#include "engine/renderer/world/effects/EnvironmentPresentationRender.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {
namespace {

[[nodiscard]] uint64_t mixBits(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] float unitRandom(uint64_t value) noexcept {
    // Keep the high 24 bits: that is exactly representable by float and has
    // a stable [0,1) mapping on every supported D3D12 host.
    constexpr float kInv24Bits = 1.0f / 16777216.0f;
    return static_cast<float>((mixBits(value) >> 40u) & 0x00ffffffu) * kInv24Bits;
}

[[nodiscard]] float fractional(float value) noexcept {
    return value - std::floor(value);
}

[[nodiscard]] bool finiteCamera(const RenderCameraSnapshot& camera) noexcept {
    return std::isfinite(camera.position.x()) && std::isfinite(camera.position.y()) &&
           std::isfinite(camera.position.z());
}

} // namespace

math::transform applyTreeSwayPresentation(
    const math::transform& world, const TreeSwayRenderState& sway,
    RenderEntityId entity, uint64_t simulationFrame) noexcept {
    if (!sway.enabled || entity == 0 || sway.periodFrames == 0 ||
        !std::isfinite(sway.directionRadians) || !std::isfinite(sway.intensityRadians) ||
        !std::isfinite(sway.leanRadians) || !std::isfinite(sway.randomness)) {
        return world;
    }

    // ScriptEngine::setSway always increments BreezeInfo's version.  Use the
    // stamped sequence as that visual generation and derive all per-tree
    // variation from it, rather than consuming a simulation-owned RNG.
    const uint64_t seed = entity ^ (sway.presentationSequence * 0xd6e8feb86659fd93ull) ^
                          (sway.presentationEpoch * 0xa0761d6478bd642full);
    const float variation = sway.randomness * 0.5f;
    const float amplitudeScale = (1.0f - variation) +
        (2.0f * variation * unitRandom(seed ^ 0x9d14e4c3b95c5f15ull));
    const float deltaScale = (1.0f - variation) +
        (2.0f * variation * unitRandom(seed ^ 0xbf58476d1ce4e5b9ull));
    const float leanScale = (1.0f - variation) +
        (2.0f * variation * unitRandom(seed ^ 0x94d049bb133111ebull));

    const uint64_t elapsedFrames = simulationFrame >= sway.confirmedTick
        ? simulationFrame - sway.confirmedTick : 0;
    const float progress = static_cast<float>(elapsedFrames % sway.periodFrames);
    float phase = progress * (math::TWO_PI / static_cast<float>(sway.periodFrames)) * deltaScale;
    // RefCode resets m_curValue only when randomness is exactly zero.  A
    // stable nonzero offset keeps randomized trees visually de-correlated
    // while preserving that uniform-zero behavior.
    if (sway.randomness != 0.0f) {
        phase += math::TWO_PI * unitRandom(seed ^ 0x632be59bd9b4e019ull);
    }
    const float targetAngle = std::cos(phase) * sway.intensityRadians * amplitudeScale +
        sway.leanRadians * leanScale;
    if (!std::isfinite(targetAngle)) return world;

    // RefCode applies X(-angle * sin(direction)) then Y(angle * cos(direction))
    // to each tree instance.  With wwmath's row-vector convention, pre-
    // multiplying the Y rotation gives the same local-to-world composition;
    // multiplying the completed local sway on the left preserves the tree's
    // world-space translation/pivot.
    math::transform localSway = math::transform::rotation_x(
        -targetAngle * std::sin(sway.directionRadians));
    localSway.pre_multiply(math::transform::rotation_y(
        targetAngle * std::cos(sway.directionRadians)));
    return localSway * world;
}

size_t buildWeatherSnowflakes(
    const WeatherRenderState& weather, const RenderCameraSnapshot& camera,
    uint64_t simulationFrame, float viewportWidth, float viewportHeight,
    container::Span<WeatherSnowflake> output) noexcept {
    if (!weather.visible || !weather.snowEnabled || output.empty() ||
        !finiteCamera(camera) || !std::isfinite(viewportWidth) ||
        !std::isfinite(viewportHeight) || viewportWidth <= 0.0f || viewportHeight <= 0.0f ||
        !std::isfinite(weather.boxDimensions) || !std::isfinite(weather.boxDensity) ||
        !std::isfinite(weather.velocity) || !std::isfinite(weather.quadSize) ||
        !std::isfinite(weather.pointSize) || !std::isfinite(weather.minimumPointSize) ||
        !std::isfinite(weather.maximumPointSize) || !std::isfinite(weather.frequencyScaleX) ||
        !std::isfinite(weather.frequencyScaleY) || !std::isfinite(weather.amplitude) ||
        weather.boxDimensions <= 0.0f ||
        weather.boxDensity <= 0.0f || weather.velocity <= 0.0f) {
        return 0;
    }

    // The reference can submit tens of thousands of point sprites.  The
    // modern fallback uses a deliberately bounded procedural field; density
    // still affects visual population while the cap keeps the existing UI
    // quad batch within its per-frame upload budget.
    // Weather.ini accepts finite reals, including values whose float product
    // would overflow before sqrt().  Compute the density estimate in double
    // and clamp it before converting to the bounded upload count.
    const double requested = std::sqrt(static_cast<double>(weather.boxDimensions) *
                                       static_cast<double>(weather.boxDensity)) * 14.0;
    const size_t maximumParticles = std::min<size_t>(output.size(), 192);
    const size_t minimumParticles = std::min<size_t>(24, maximumParticles);
    if (!std::isfinite(requested)) return 0;
    const double clampedRequested = std::clamp(
        requested, static_cast<double>(minimumParticles),
        static_cast<double>(maximumParticles));
    const size_t desired = static_cast<size_t>(std::lround(clampedRequested));
    if (desired == 0) return 0;

    const float timeSeconds = static_cast<float>(simulationFrame % 1800000ull) / 30.0f;
    const float normalizedVelocity = weather.velocity / std::max(weather.boxDimensions, 1.0f);
    const float cameraOffsetX = camera.position.x() * weather.frequencyScaleX * 0.04f;
    const float cameraOffsetY = camera.position.y() * weather.frequencyScaleY * 0.04f;
    const float baseSize = weather.usePointSprites ? weather.pointSize * 4.0f
                                                    : weather.quadSize * 12.0f;
    const float minimumSize = std::max(0.5f, weather.minimumPointSize);
    const float maximumSize = std::max(minimumSize, weather.maximumPointSize);
    const uint64_t epochSeed = weather.presentationEpoch * 0x9e3779b97f4a7c15ull;

    for (size_t index = 0; index < desired; ++index) {
        const uint64_t seed = epochSeed ^ (static_cast<uint64_t>(index) * 0xd6e8feb86659fd93ull);
        const float horizontal = fractional(unitRandom(seed) + cameraOffsetX +
            std::sin(timeSeconds * 0.27f + unitRandom(seed ^ 0x10ull) * math::TWO_PI) *
                (weather.amplitude / std::max(weather.boxDimensions, 1.0f)));
        const float vertical = fractional(unitRandom(seed ^ 0x1ull) + cameraOffsetY -
                                          timeSeconds * normalizedVelocity);
        const float scale = 0.65f + unitRandom(seed ^ 0x2ull) * 0.85f;
        output[index] = {
            .x = horizontal * viewportWidth,
            .y = vertical * viewportHeight,
            .size = std::clamp(baseSize * scale, minimumSize, maximumSize),
            .opacity = 0.30f + unitRandom(seed ^ 0x3ull) * 0.45f,
        };
    }
    return desired;
}

} // namespace engine::render
