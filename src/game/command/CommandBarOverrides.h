#pragma once

#include "core/container/container_types.h"
#include "game/command/CommandSetStore.h"

#include <cstddef>
#include <cstdint>

namespace game {

// Confirmed presentation ordering attached to one effective CommandSet
// mutation.  This is a command/presentation contract: scripts are one
// producer, but production admission and UI projection are also consumers.
struct CommandBarOverrideMutationStamp final {
    uint64_t presentationEpoch = 0;
    uint64_t sequence = 0;
    uint64_t confirmedTick = 0;
    uint32_t sourceScriptId = 0;
    uint32_t ordinal = 0;
};

// `active == false` means use the authored CommandSet slot.  An active entry
// with an empty button name is an explicit removal and must survive UI rebuilds.
struct CommandBarSlotOverride final {
    bool active = false;
    container::String commandButtonName;
    CommandBarOverrideMutationStamp stamp{};
};

// Session-owned, content-name-based effective CommandSet overlay.  It owns no
// Widget, script runtime object, ECS entity, or live CommandButton pointer.
class CommandBarOverrideState final {
public:
    static constexpr size_t kSlotCount = COMMAND_SET_SLOT_COUNT;

    void reset(uint64_t presentationEpoch = 0) noexcept;
    void rebindPresentationEpoch(uint64_t presentationEpoch) noexcept;

    [[nodiscard]] bool setSlotOverride(container::String commandSetName, size_t slot,
                                       container::String commandButtonName,
                                       CommandBarOverrideMutationStamp stamp);
    [[nodiscard]] const CommandBarSlotOverride*
    findSlotOverride(container::StringView commandSetName, size_t slot) const noexcept;
    [[nodiscard]] container::StringView effectiveButtonName(
        container::StringView commandSetName, size_t slot,
        container::StringView authoredButtonName) const noexcept;
    [[nodiscard]] const CommandBarOverrideMutationStamp& lastMutation() const noexcept {
        return m_lastMutation;
    }

private:
    using SlotArray = container::Array<CommandBarSlotOverride, kSlotCount>;

    container::TreeMap<container::String, SlotArray, std::less<>> m_overrides;
    CommandBarOverrideMutationStamp m_lastMutation{};
};

} // namespace game
