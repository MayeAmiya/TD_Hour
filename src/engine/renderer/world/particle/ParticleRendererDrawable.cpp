#include "ParticleRenderer.h"

#include "engine/renderer/world/model/D3D12W3dModel.h"
#include "engine/renderer/world/model/W3dAssetCache.h"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {

[[nodiscard]] bool finiteDrawableParticle(
    const fx::ParticleRuntimeParticle& particle) noexcept {
    return std::isfinite(particle.position.x) &&
        std::isfinite(particle.position.y) &&
        std::isfinite(particle.position.z) &&
        std::isfinite(particle.previousPosition.x) &&
        std::isfinite(particle.previousPosition.y) &&
        std::isfinite(particle.previousPosition.z) &&
        std::isfinite(particle.orientation.x) &&
        std::isfinite(particle.orientation.y) &&
        std::isfinite(particle.orientation.z) &&
        std::isfinite(particle.size) &&
        std::isfinite(particle.alpha);
}

[[nodiscard]] math::vec3 interpolatedDrawablePosition(
    const fx::ParticleRuntimeParticle& particle, float alpha) noexcept {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return {
        std::lerp(particle.previousPosition.x, particle.position.x, alpha),
        std::lerp(particle.previousPosition.y, particle.position.y, alpha),
        std::lerp(particle.previousPosition.z, particle.position.z, alpha),
    };
}

} // namespace

size_t ParticleRenderer::appendDrawableParticleDrawPackets(
    const fx::ParticleRuntime& runtime,
    const fx::ParticleSystemCatalog& catalog,
    W3dAssetCache& assets,
    container::Vector<StaticMeshDrawPacket>& output,
    W3dRestPaletteFrameCache& restPalettes,
    float visualTimeSeconds,
    float interpolationAlpha,
    const LocalVisibilityRenderSnapshot& localVisibility,
    W3dModelGraphTraversalStats* traversalStats) {
    const size_t packetStart = output.size();
    runtime.visitParticles(
        [&](size_t, const fx::ParticleRuntimeParticle& particle) {
            const fx::ParticleSystemTemplate* definition =
                catalog.find(particle.templateId);
            if (!definition || definition->kind != fx::ParticleKind::Drawable ||
                definition->particleName.empty() ||
                !finiteDrawableParticle(particle)) {
                return;
            }

            const math::vec3 position = interpolatedDrawablePosition(
                particle, interpolationAlpha);
            if (!localVisibility.isInsidePlayableBounds(position)) return;

            // DRAWABLE's authored Size = 0 means use the W3D's native scale;
            // it is not the billboard convention of an empty/invisible quad.
            const float scale = particle.size > 0.0f ? particle.size : 1.0f;
            const W3dModelHandle handle = assets.requestAsync(
                definition->particleName, true, RenderAssetPriority::Visible);
            if (!handle) return;

            // TD's W3D world is Z-up. Compose the authored Euler channels in
            // the same X → Y → Z order used by world presentation instead of
            // DirectX's Y-up RollPitchYaw helper.
            const math::quat xRotation = math::quat::from_axis_angle(
                {1.0f, 0.0f, 0.0f}, particle.orientation.x);
            const math::quat yRotation = math::quat::from_axis_angle(
                {0.0f, 1.0f, 0.0f}, particle.orientation.y);
            const math::quat zRotation = math::quat::from_axis_angle(
                {0.0f, 0.0f, 1.0f}, particle.orientation.z);
            const math::quat orientation =
                ((xRotation * yRotation) * zRotation).normalized();
            const math::transform world = math::transform::from_trs(
                {scale, scale, scale}, orientation, position);
            const size_t modelPacketStart = output.size();
            static_cast<void>(appendW3dModelGraphDrawPackets(
                assets, handle, world, {}, {}, output,
                {.visualTimeSeconds = visualTimeSeconds,
                 .objectOpacity = std::clamp(particle.alpha, 0.0f, 1.0f),
                 .receivesDynamicLights = false,
                 .restPalettes = &restPalettes},
                traversalStats));
            for (size_t index = modelPacketStart; index < output.size();
                 ++index) {
                // ParticleSystem drawables are transient visual debris. They
                // follow the map-border mask but never become static shadow
                // casters or persistent world geometry.
                output[index].receivesMapBorder = true;
                output[index].castsShadow = false;
            }
        });
    return output.size() - packetStart;
}

} // namespace engine::render
