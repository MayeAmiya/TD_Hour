#pragma once

#include "core/container/hash_containers.h"

#include "ParticleSystemCatalog.h"

#include <cstdint>
#include <variant>
namespace game {
struct IniBlock;
}

namespace engine::fx {

struct FxListId final {
    uint32_t value = 0;

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(FxListId, FxListId) = default;
};

enum class FxViewShake : uint8_t {
    Subtle,
    Normal,
    Strong,
    Severe,
    CineExtreme,
    CineInsane,
};

enum class FxTerrainScorch : int8_t {
    Random = -1,
    Scorch1,
    Scorch2,
    Scorch3,
    Scorch4,
    ShadowScorch,
};

struct FxSoundNugget final {
    container::String name;
};

struct FxRayEffectNugget final {
    container::String objectTemplate;
    ParticleVector3 primaryOffset;
    ParticleVector3 secondaryOffset;
};

struct FxTracerNugget final {
    container::String tracerName = "GenericTracer";
    container::String boneName;
    float speed = 0.0f;
    float decayAt = 1.0f;
    float length = 10.0f;
    float width = 1.0f;
    ParticleColor color{255, 255, 255};
    float probability = 1.0f;
};

struct FxLightPulseNugget final {
    ParticleColor color;
    float radius = 0.0f;
    float radiusAsPercentOfObjectSize = 0.0f;
    uint32_t increaseTimeMilliseconds = 0;
    uint32_t decreaseTimeMilliseconds = 0;
};

struct FxViewShakeNugget final {
    FxViewShake type = FxViewShake::Normal;
};

struct FxTerrainScorchNugget final {
    FxTerrainScorch type = FxTerrainScorch::Random;
    float radius = 0.0f;
};

struct FxParticleSystemNugget final {
    container::String particleSystemName;
    ParticleTemplateId particleSystem;
    int32_t count = 1;
    ParticleVector3 offset;
    ParticleRange radius;
    ParticleRange height;
    ParticleRange initialDelay{-1.0f, -1.0f};
    float rotateX = 0.0f;
    float rotateY = 0.0f;
    float rotateZ = 0.0f;
    bool orientToObject = false;
    bool ricochet = false;
    bool attachToObject = false;
    bool createAtGroundHeight = false;
    bool useCallersRadius = false;
};

struct FxListAtBoneNugget final {
    container::String fxName;
    FxListId fx;
    container::String boneName;
    bool orientToBone = true;
};

// Authored FXListAtBonePos semantics: the unadorned bone is followed by a
// contiguous BoneName01..BoneName40 sequence.
inline constexpr uint32_t kMaximumFxListAtBonePoints = 40;

using FxNugget = std::variant<
    FxSoundNugget,
    FxRayEffectNugget,
    FxTracerNugget,
    FxLightPulseNugget,
    FxViewShakeNugget,
    FxTerrainScorchNugget,
    FxParticleSystemNugget,
    FxListAtBoneNugget>;

struct FxListDefinition final {
    FxListId id;
    container::String name;
    container::Vector<FxNugget> nuggets;
};

class FxListCatalog final {
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
    [[nodiscard]] bool applyOverridesFromVfs(container::StringView path,
                                             container::String* error = nullptr);
    void resolveReferences(const ParticleSystemCatalog& particles);
    void clear();

    [[nodiscard]] const FxListDefinition* find(container::StringView name) const noexcept;
    [[nodiscard]] const FxListDefinition* find(FxListId id) const noexcept;
    [[nodiscard]] FxListId findId(container::StringView name) const noexcept;
    [[nodiscard]] const container::Vector<FxListDefinition>& definitions() const noexcept {
        return m_definitions;
    }
    [[nodiscard]] const container::Vector<container::String>& diagnostics() const noexcept {
        return m_diagnostics;
    }
    [[nodiscard]] uint64_t presentationFingerprint() const noexcept { return m_fingerprint; }

private:
    bool appendParsedLayer(container::StringView content, container::StringView sourceName,
                           container::String* error, bool* parsedDefinition = nullptr);
    bool parseDefinition(const game::IniBlock& block, container::StringView sourceName,
                         FxListDefinition& output, container::String* error);
    bool parseNugget(const game::IniBlock& block, container::StringView sourceName,
                     FxNugget& output, container::String* error);
    void rebuildIndexAndResolveFxReferences();

    container::Vector<FxListDefinition> m_definitions;
    container::HashMap<container::String, size_t> m_indicesByName;
    container::Vector<container::String> m_diagnostics;
    uint64_t m_fingerprint = 0;
    bool m_loaded = false;
};

} // namespace engine::fx
