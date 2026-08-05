#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace engine::render {

enum class UiSrvResourceKind : uint8_t {
    Texture,
    Glyph,
};

struct UiSrvInvalidation final {
    UiSrvResourceKind kind = UiSrvResourceKind::Texture;
    uint64_t identity = 0;
};

// CPU texture/font owners publish before destroying pixel storage. The render
// thread drains at beginFrame and retires the matching SRV through the normal
// D3D12 fence path. The process-lifetime bus intentionally outlives subsystem
// teardown so late static FontRegistry destruction cannot call a dead service.
void publishUiSrvInvalidation(
    UiSrvResourceKind kind, uint64_t identity) noexcept;

[[nodiscard]] container::Vector<UiSrvInvalidation>
takeUiSrvInvalidations() noexcept;

// A failed notification allocation is non-fatal because monotonic identities
// still prevent aliasing and idle eviction remains a fallback. Debug/runtime
// diagnostics consume this count so pressure cannot stay silent.
[[nodiscard]] uint64_t takeDroppedUiSrvInvalidationCount() noexcept;

} // namespace engine::render
