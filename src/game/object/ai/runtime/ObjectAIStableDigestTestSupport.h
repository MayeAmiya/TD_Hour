#pragma once

#include <cstdint>

namespace engine::ai
{

struct ObjectAIOrderAdmissionSnapshot;

namespace test_support
{

// Narrow digest hook for field-coverage contracts. Runtime code should digest
// the complete ObjectAIRuntimeSnapshot through ObjectAIStableDigest.
[[nodiscard]] uint64_t stableDigest(
    const ObjectAIOrderAdmissionSnapshot& snapshot) noexcept;

} // namespace test_support
} // namespace engine::ai
