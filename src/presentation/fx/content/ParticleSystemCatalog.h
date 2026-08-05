#pragma once

#include "core/container/hash_containers.h"
#include <cstdint>
namespace game {
struct IniBlock;
}

namespace engine::fx {

inline constexpr size_t kParticleKeyframeCount = 8;

struct ParticleTemplateId final {
    uint32_t value = 0;

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(ParticleTemplateId, ParticleTemplateId) = default;
};

enum class ParticlePriority : uint8_t {
    Invalid,
    WeaponExplosion,
    ScorchMark,
    DustTrail,
    BuildUp,
    DebrisTrail,
    UnitDamageFx,
    DeathExplosion,
    SemiConstant,
    Constant,
    WeaponTrail,
    AreaEffect,
    Critical,
    AlwaysRender,
    Count,
};

enum class ParticleShader : uint8_t {
    None,
    Additive,
    Alpha,
    AlphaTest,
    Multiply,
    Count,
};

enum class ParticleKind : uint8_t {
    None,
    Billboard,
    Drawable,
    Streak,
    Volume,
    Smudge,
    Count,
};

enum class ParticleVelocityKind : uint8_t {
    None,
    Ortho,
    Spherical,
    Hemispherical,
    Cylindrical,
    Outward,
    Count,
};

enum class ParticleVolumeKind : uint8_t {
    None,
    Point,
    Line,
    Box,
    Sphere,
    Cylinder,
    Count,
};

enum class ParticleWindMotion : uint8_t {
    Invalid,
    Unused,
    PingPong,
    Circular,
    Count,
};

// First opt-in GPU Hybrid backend support matrix.  This is deliberately a
// reason rather than a boolean so content diagnostics and A/B telemetry can
// explain every CPU fallback without guessing from authored fields.  The
// first version only admits order-independent Additive and depth-writing
// AlphaTest billboards; expanding this enum is an explicit contract change.
enum class GpuParticleCompatibilityReason : uint8_t {
    Compatible,
    KindNone,
    KindDrawable,
    KindStreak,
    KindVolume,
    KindSmudge,
    KindInvalid,
    ShaderNoneRequiresCpuOrdering,
    ShaderAlphaRequiresCpuOrdering,
    ShaderMultiplyRequiresCpuOrdering,
    ShaderInvalid,
    SlaveSystemRequiresCpuAuthority,
    PerParticleAttachedSystemRequiresCpuAuthority,
    DynamicWindRequiresCpuEmitter,
    Count,
};

[[nodiscard]] constexpr container::StringView gpuParticleCompatibilityReasonName(
    GpuParticleCompatibilityReason reason) noexcept {
    switch (reason) {
    case GpuParticleCompatibilityReason::Compatible: return "compatible";
    case GpuParticleCompatibilityReason::KindNone: return "kind-none";
    case GpuParticleCompatibilityReason::KindDrawable: return "kind-drawable";
    case GpuParticleCompatibilityReason::KindStreak: return "kind-streak";
    case GpuParticleCompatibilityReason::KindVolume: return "kind-volume";
    case GpuParticleCompatibilityReason::KindSmudge: return "kind-smudge";
    case GpuParticleCompatibilityReason::KindInvalid: return "kind-invalid";
    case GpuParticleCompatibilityReason::ShaderNoneRequiresCpuOrdering:
        return "shader-none-requires-cpu-ordering";
    case GpuParticleCompatibilityReason::ShaderAlphaRequiresCpuOrdering:
        return "shader-alpha-requires-cpu-ordering";
    case GpuParticleCompatibilityReason::ShaderMultiplyRequiresCpuOrdering:
        return "shader-multiply-requires-cpu-ordering";
    case GpuParticleCompatibilityReason::ShaderInvalid: return "shader-invalid";
    case GpuParticleCompatibilityReason::SlaveSystemRequiresCpuAuthority:
        return "slave-system-requires-cpu-authority";
    case GpuParticleCompatibilityReason::PerParticleAttachedSystemRequiresCpuAuthority:
        return "per-particle-attached-system-requires-cpu-authority";
    case GpuParticleCompatibilityReason::DynamicWindRequiresCpuEmitter:
        return "dynamic-wind-requires-cpu-emitter";
    case GpuParticleCompatibilityReason::Count: return "invalid-reason";
    }
    return "invalid-reason";
}

struct ParticleRange final {
    float minimum = 0.0f;
    float maximum = 0.0f;
};

struct ParticleVector3 final {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ParticleColor final {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

struct ParticleAlphaKeyframe final {
    ParticleRange value;
    uint32_t frame = 0;
};

struct ParticleColorKeyframe final {
    ParticleColor color;
    uint32_t frame = 0;
};

struct ParticleSystemTemplate final {
    ParticleTemplateId id;
    container::String name;
    ParticlePriority priority = ParticlePriority::WeaponExplosion;
    ParticleShader shader = ParticleShader::None;
    ParticleKind kind = ParticleKind::None;
    container::String particleName;

    // The original ParticleSystemInfo keeps full authored Euler state.  Most
    // PARTICLE definitions are camera-facing billboards and only consume Z,
    // but DRAWABLE particles require all three axes; retaining them here also
    // keeps a sparse override from silently discarding original data.
    ParticleRange angleX;
    ParticleRange angleY;
    ParticleRange angleZ;
    ParticleRange angularRateX;
    ParticleRange angularRateY;
    ParticleRange angularRateZ;
    ParticleRange angularDamping;
    ParticleRange velocityDamping;
    float gravity = 0.0f;

    container::String slaveSystemName;
    ParticleTemplateId slaveSystem;
    ParticleVector3 slavePositionOffset;
    container::String perParticleAttachedSystemName;
    ParticleTemplateId perParticleAttachedSystem;

    ParticleRange lifetime;
    uint32_t systemLifetime = 0;
    ParticleRange startSize;
    ParticleRange startSizeRate;
    ParticleRange sizeRate;
    ParticleRange sizeRateDamping;
    container::Array<ParticleAlphaKeyframe, kParticleKeyframeCount> alphaKeys{};
    container::Array<ParticleColorKeyframe, kParticleKeyframeCount> colorKeys{};
    ParticleRange colorScale;

    ParticleRange burstDelay;
    ParticleRange burstCount;
    ParticleRange initialDelay;
    ParticleVector3 driftVelocity;

    ParticleVelocityKind velocityKind = ParticleVelocityKind::None;
    ParticleRange velocityOrthoX;
    ParticleRange velocityOrthoY;
    ParticleRange velocityOrthoZ;
    ParticleRange velocitySpherical;
    ParticleRange velocityHemispherical;
    ParticleRange velocityCylindricalRadial;
    ParticleRange velocityCylindricalNormal;
    ParticleRange velocityOutward;
    ParticleRange velocityOutwardOther;

    ParticleVolumeKind volumeKind = ParticleVolumeKind::None;
    ParticleVector3 volumeLineStart;
    ParticleVector3 volumeLineEnd;
    ParticleVector3 volumeBoxHalfSize;
    float volumeSphereRadius = 0.0f;
    float volumeCylinderRadius = 0.0f;
    float volumeCylinderLength = 0.0f;

    bool oneShot = false;
    bool hollow = false;
    bool groundAligned = false;
    bool emitAboveGroundOnly = false;
    bool particleUpTowardsEmitter = false;

    ParticleWindMotion windMotion = ParticleWindMotion::Unused;
    float windAngleChangeMinimum = 0.15f;
    float windAngleChangeMaximum = 0.45f;
    float windPingPongStartAngleMinimum = 0.0f;
    float windPingPongStartAngleMaximum = 0.78539816339f;
    float windPingPongEndAngleMinimum = 5.49778714378f;
    float windPingPongEndAngleMaximum = 6.28318530718f;

    // Derived after references have been resolved.  It is not authored state
    // and therefore does not participate in the content fingerprint.
    GpuParticleCompatibilityReason gpuCompatibilityReason =
        GpuParticleCompatibilityReason::KindNone;

    // The legacy renderer also routes PARTICLE definitions whose texture
    // starts with "SMUD" through the smudge path.  Keep track of that derived
    // promotion so a later sparse override can still update ParticleName or
    // Type against the inherited authored PARTICLE value.
    bool smudgeKindInferred = false;

    [[nodiscard]] bool gpuCompatible() const noexcept {
        return gpuCompatibilityReason == GpuParticleCompatibilityReason::Compatible;
    }
};

[[nodiscard]] GpuParticleCompatibilityReason classifyGpuParticleCompatibility(
    const ParticleSystemTemplate& definition) noexcept;

class ParticleSystemCatalog final {
public:
    static container::Vector<container::String> enumerateVfsLoadFiles(
        container::Span<const container::StringView> loadRoots);

    bool loadFromVfsLoadDirectories(container::Span<const container::StringView> loadRoots,
                                    container::String* error = nullptr);
    bool loadFromVfsFiles(const container::Vector<container::String>& logicalFiles,
                          container::String* error = nullptr);
    bool loadFromText(container::StringView content,
                      container::StringView sourceName = "<memory>",
                      container::String* error = nullptr);
    [[nodiscard]] bool applyOverridesFromVfs(
        container::StringView path, container::String* error = nullptr);

    void clear();

    [[nodiscard]] const ParticleSystemTemplate* find(container::StringView name) const noexcept;
    [[nodiscard]] const ParticleSystemTemplate* find(ParticleTemplateId id) const noexcept;
    [[nodiscard]] ParticleTemplateId findId(container::StringView name) const noexcept;
    [[nodiscard]] const container::Vector<ParticleSystemTemplate>& templates() const noexcept {
        return m_templates;
    }
    [[nodiscard]] const container::Vector<container::String>& diagnostics() const noexcept {
        return m_diagnostics;
    }
    [[nodiscard]] uint64_t presentationFingerprint() const noexcept { return m_fingerprint; }

private:
    bool appendParsedLayer(container::StringView content,
                           container::StringView sourceName,
                           container::String* error,
                           bool* parsedDefinition = nullptr);
    bool parseTemplate(const game::IniBlock& block, container::StringView sourceName,
                       ParticleSystemTemplate& output, container::String* error);
    void rebuildIndexAndResolveReferences();

    container::Vector<ParticleSystemTemplate> m_templates;
    container::HashMap<container::String, size_t> m_indicesByName;
    container::Vector<container::String> m_diagnostics;
    uint64_t m_fingerprint = 0;
};

} // namespace engine::fx
