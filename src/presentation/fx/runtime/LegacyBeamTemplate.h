#pragma once

#include "core/container/hash_containers.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>

namespace game {
struct ThingTemplate;
}

namespace engine::fx {

enum class LegacyBeamTemplateKind : uint8_t {
    Laser,
    Tracer,
    Rope,
    ModelRay,
};

struct LegacyBeamColor final {
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;
    float alpha = 1.0f;
};

struct LegacyLaserTemplate final {
    container::String textureName;
    LegacyBeamColor innerColor;
    LegacyBeamColor outerColor;
    float innerBeamWidth = 0.0f;
    float outerBeamWidth = 1.0f;
    // Frozen gameplay mirror used by ParticleUplinkCannon collision/damage
    // radius. Rendering continues to consume the authored float fields.
    math::q32_32 outerBeamWidthFixed{int32_t{1}};
    float scrollRate = 0.0f;
    float arcHeight = 0.0f;
    float segmentOverlapRatio = 0.0f;
    float tilingScalar = 1.0f;
    float punchThroughScalar = 0.0f;
    float minimumLifetimeSeconds = 0.0f;
    float maximumLifetimeSeconds = 0.0f;
    uint32_t numberOfBeams = 1;
    uint32_t segments = 1;
    uint32_t maximumIntensityFrames = 0;
    uint32_t fadeFrames = 0;
    container::String muzzleParticleSystem;
    container::String targetParticleSystem;
    bool tileTexture = false;
};

struct LegacyModelRayTemplate final {
    uint32_t minimumLifetimeMilliseconds = 0;
    uint32_t maximumLifetimeMilliseconds = 0;
    float assetScale = 1.0f;
    bool lifetimeAuthored = false;
    bool castsDirectionalShadow = false;
};

struct LegacyBeamTemplate final {
    container::String objectTemplate;
    LegacyBeamTemplateKind kind = LegacyBeamTemplateKind::ModelRay;
    LegacyLaserTemplate laser;
    container::String modelAsset;
    LegacyModelRayTemplate modelRay;
};

using LegacyBeamTemplateCatalog =
    container::HashMap<container::String, LegacyBeamTemplate>;

[[nodiscard]] std::optional<LegacyBeamTemplate> compileLegacyBeamTemplate(
    const game::ThingTemplate& source,
    container::String* diagnostic = nullptr);

} // namespace engine::fx
