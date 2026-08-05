#pragma once

#include "core/container/container_types.h"

#include "MatchSetup.h"

#include <optional>
namespace engine {

// Versioned binary boundary for future replay-v3/network match identity.  It
// serializes only ResolvedMatchSetup; LocalControlContext and transport tokens
// are intentionally impossible to encode here.
class MatchSetupCodec final {
public:
    [[nodiscard]] static bool encode(const ResolvedMatchSetup& setup,
                                     container::Vector<uint8_t>& output,
                                     container::String* error = nullptr);
    [[nodiscard]] static std::optional<ResolvedMatchSetup> decode(container::Span<const uint8_t> input,
                                                                    container::String* error = nullptr);
};

} // namespace engine
