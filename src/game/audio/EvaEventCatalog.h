#pragma once

#include "core/container/container_types.h"
#include "presentation/audio/EvaEventContracts.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace game {

using engine::audio::EvaEventType;
using engine::audio::kEvaEventTypeCount;

struct EvaSideSounds final {
    container::String side;
    container::Vector<container::String> sounds;
};

struct EvaEventDefinition final {
    EvaEventType type = EvaEventType::Count;
    uint32_t priority = 1;
    uint32_t cooldownMilliseconds = 30'000;
    uint32_t expirationMilliseconds = 5'000;
    container::Vector<EvaSideSounds> sideSounds;
};

// Immutable client-content catalog compiled from the winning Data/INI/Eva.ini
// during GameSession content freeze. It owns no audio handles or runtime
// cooldown state.
class EvaEventCatalog final {
public:
    [[nodiscard]] bool compile(container::StringView content,
                               container::StringView sourcePath,
                               container::String* error = nullptr);

    [[nodiscard]] const EvaEventDefinition*
    find(EvaEventType type) const noexcept;

    // Empty means the authored side has no playable sound (including
    // NoSound), not a fallback to another faction.
    [[nodiscard]] container::String resolveSound(
        EvaEventType type, container::StringView side,
        uint64_t variationSeed) const;

    [[nodiscard]] size_t size() const noexcept { return m_size; }

private:
    container::Array<std::optional<EvaEventDefinition>, kEvaEventTypeCount>
        m_definitions{};
    size_t m_size = 0;
};

[[nodiscard]] std::optional<EvaEventType>
parseEvaEventType(container::StringView name) noexcept;
[[nodiscard]] container::StringView
evaEventTypeName(EvaEventType type) noexcept;

} // namespace game
