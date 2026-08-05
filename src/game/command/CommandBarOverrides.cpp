#include "CommandBarOverrides.h"

#include <utility>

namespace game {

void CommandBarOverrideState::reset(uint64_t presentationEpoch) noexcept {
    m_overrides.clear();
    m_lastMutation = {.presentationEpoch = presentationEpoch};
}

void CommandBarOverrideState::rebindPresentationEpoch(
    uint64_t presentationEpoch) noexcept {
    for (auto& [name, slots] : m_overrides) {
        static_cast<void>(name);
        for (CommandBarSlotOverride& slot : slots) {
            if (slot.active) slot.stamp.presentationEpoch = presentationEpoch;
        }
    }
    m_lastMutation.presentationEpoch = presentationEpoch;
}

bool CommandBarOverrideState::setSlotOverride(
    container::String commandSetName, size_t slot, container::String commandButtonName,
    CommandBarOverrideMutationStamp stamp) {
    if (commandSetName.empty() || slot >= kSlotCount) return false;
    SlotArray& slots = m_overrides[std::move(commandSetName)];
    CommandBarSlotOverride& target = slots[slot];
    if (target.active && target.commandButtonName == commandButtonName) return false;
    target.active = true;
    target.commandButtonName = std::move(commandButtonName);
    target.stamp = stamp;
    m_lastMutation = stamp;
    return true;
}

const CommandBarSlotOverride*
CommandBarOverrideState::findSlotOverride(container::StringView commandSetName,
                                          size_t slot) const noexcept {
    if (commandSetName.empty() || slot >= kSlotCount) return nullptr;
    const auto found = m_overrides.find(commandSetName);
    if (found == m_overrides.end()) return nullptr;
    const CommandBarSlotOverride& candidate = found->second[slot];
    return candidate.active ? &candidate : nullptr;
}

container::StringView CommandBarOverrideState::effectiveButtonName(
    container::StringView commandSetName, size_t slot,
    container::StringView authoredButtonName) const noexcept {
    if (const CommandBarSlotOverride* override =
            findSlotOverride(commandSetName, slot)) {
        return override->commandButtonName;
    }
    return authoredButtonName;
}

} // namespace game
