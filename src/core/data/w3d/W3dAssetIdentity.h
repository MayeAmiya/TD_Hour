#pragma once

#include "core/container/container_types.h"

#include <optional>

namespace data::w3d {

inline constexpr container::StringView kW3dExtension = ".w3d";
inline constexpr container::StringView kDefaultW3dRoot = "art/w3d/";

struct W3dModelIdentity final {
    container::String sourcePath;
    container::String prototype;
};

// Shared legacy-name resolver used by renderer caches and authoritative
// content compilation. Keeping it in core prevents model/prototype rules from
// drifting once W3D pristine poses become lockstep inputs.
[[nodiscard]] std::optional<W3dModelIdentity> resolveW3dModelIdentity(
    container::StringView source,
    container::StringView explicitPrototype = {},
    container::String* error = nullptr);

[[nodiscard]] container::String w3dHierarchySourcePath(
    container::StringView hierarchyName);

// Ref AssetManager loads the suffix after the first '.', so
// HIERARCHY.CLIP resolves to art/w3d/clip.w3d.
[[nodiscard]] container::String w3dAnimationFileStem(
    container::StringView logicalAnimationName);
[[nodiscard]] container::String w3dAnimationSourcePath(
    container::StringView logicalAnimationName);

} // namespace data::w3d
