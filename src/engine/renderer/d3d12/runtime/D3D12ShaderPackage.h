#pragma once

#include "core/container/container_types.h"

namespace engine::d3d12 {

struct ShaderPackageEntrySpec final {
    container::StringView fileManifestKey;
    container::StringView expectedFile;
    container::StringView profileManifestKey;
    container::StringView expectedProfile;
};

// Build-generated DXBC package contract. Runtime loading is intentionally
// strict: an absent, stale or malformed package disables only its owning
// optional renderer feature and never falls back to D3DCompile.
[[nodiscard]] bool loadShaderPackage(
    container::StringView shaderName,
    container::StringView expectedVersion,
    container::StringView expectedSourceSha256,
    container::Span<const ShaderPackageEntrySpec> entries,
    container::Vector<container::Vector<uint8_t>>& bytecode);

} // namespace engine::d3d12
