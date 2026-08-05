#pragma once

#include "core/container/container_types.h"
#include "presentation/fx/content/ParticleSystemCatalog.h"

#include <cstddef>
#include <cstdint>

namespace engine::render {

// Device-independent compilation shared by the renderer and focused probes.
// UINT32_MAX denotes a template that must remain on the CPU fallback path.
struct GpuParticleMaterialBin final {
    fx::ParticleShader shader = fx::ParticleShader::None;
    container::String textureName;
    uint32_t firstTemplateId = 0;
};

struct GpuParticleMaterialBinCompilation final {
    container::Vector<uint32_t> templateToBin;
    container::Vector<GpuParticleMaterialBin> bins;
    size_t mappedTemplates = 0;
    size_t rejectedCompatibleTemplates = 0;
};

// Deduplication key for one GPU material: shader identity plus the texture
// name, case-folded because authored particle definitions spell the same
// texture inconsistently.
[[nodiscard]] container::String gpuParticleMaterialBinKey(
    fx::ParticleShader shader, container::StringView textureName);

// A bin retains the authored casing of the first template that created it so
// the texture is acquired under the same name the CPU batches use.  Consumers
// pairing draw batches with bins must therefore fold case exactly like the
// deduplication key does: a case-sensitive compare makes a bin whose only
// on-screen template spells the texture differently look inactive, leaving its
// texture descriptor unbound so the indirect draw samples the fallback.
[[nodiscard]] bool gpuParticleMaterialBinMatches(
    const GpuParticleMaterialBin& bin, fx::ParticleShader shader,
    container::StringView textureName) noexcept;

[[nodiscard]] GpuParticleMaterialBinCompilation compileGpuParticleMaterialBins(
    const fx::ParticleSystemCatalog& catalog, size_t maximumBins);

} // namespace engine::render
