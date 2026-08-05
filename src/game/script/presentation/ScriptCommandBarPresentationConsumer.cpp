#include "core/container/container_types.h"
#include "ScriptCommandBarPresentationConsumer.h"

#include "game/command/CommandButtonStore.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/content/runtime/GameContentSnapshot.h"

#include <utility>

namespace engine::script {
namespace {

template <typename ButtonNameResolver>
[[nodiscard]] ScriptCommandBarPresentationConsumer::SlotArray buildSlots(
    const GameContentSnapshot& content, container::StringView selectedObjectType,
    container::StringView selectedCommandSet,
    ButtonNameResolver&& resolveButtonName, container::String& resolvedSelection,
    bool& hasCommandSet) {
    using SlotArray = ScriptCommandBarPresentationConsumer::SlotArray;

    SlotArray slots{};
    resolvedSelection.clear();
    hasCommandSet = false;
    if (!content.isCaptured() || selectedObjectType.empty()) return slots;

    const container::SharedPtr<const game::ObjectArchetype> objectTemplate =
        content.findObjectArchetype(selectedObjectType);
    if (!objectTemplate || selectedCommandSet.empty()) return slots;

    const game::CommandSetTemplate* commandSet =
        content.findCommandSet(selectedCommandSet);
    if (!commandSet) return slots;

    resolvedSelection.assign(selectedObjectType);
    hasCommandSet = true;
    for (size_t slot = 0; slot < ScriptCommandBarPresentationConsumer::kSlotCount; ++slot) {
        const container::StringView buttonName = resolveButtonName(*commandSet, slot);
        if (buttonName.empty()) continue;
        const game::CommandButtonTemplate* commandButton =
            content.findCommandButton(buttonName);
        // The legacy INI parser rejects a CommandSet reference to an unknown
        // CommandButton. Retain a defensive empty slot for malformed/modded
        // content rather than exposing a button that cannot be dispatched or
        // rendered.
        if (!commandButton || game::hasCommandButtonOption(
                commandButton->descriptor.options,
                game::CommandButtonOption::ScriptOnly)) {
            continue;
        }
        slots[slot] = {
            .visible = true,
            .commandButtonName = container::String{buttonName},
            .buttonImage = commandButton->buttonImage,
            .textLabel = commandButton->textLabel,
            .descriptionLabel = commandButton->descriptionLabel,
            .borderType = commandButton->borderType,
        };
    }
    return slots;
}

} // namespace

void ScriptCommandBarPresentationConsumer::clear() noexcept {
    m_slots = {};
    m_selectedObjectType.clear();
    m_hasCommandSet = false;
}

bool ScriptCommandBarPresentationConsumer::synchronize(
    const GameContentSnapshot& content, const game::CommandBarOverrideState& overrides,
    container::StringView selectedObjectType) {
    const container::SharedPtr<const game::ObjectArchetype> objectTemplate =
        content.findObjectArchetype(selectedObjectType);
    const container::StringView commandSetName = objectTemplate
        ? container::StringView{objectTemplate->templateData.commandSet}
        : container::StringView{};
    container::String nextSelection;
    bool nextHasCommandSet = false;
    SlotArray nextSlots = buildSlots(
        content, selectedObjectType, commandSetName,
        [&overrides](const game::CommandSetTemplate& commandSet, size_t slot) {
            return overrides.effectiveButtonName(commandSet.name, slot, commandSet.commands[slot]);
        },
        nextSelection, nextHasCommandSet);
    return commit(std::move(nextSlots), std::move(nextSelection), nextHasCommandSet);
}

bool ScriptCommandBarPresentationConsumer::synchronizeEffective(
    const GameContentSnapshot& content,
    container::StringView selectedObjectType,
    container::StringView selectedCommandSet,
    const ButtonNameArray& effectiveButtonNames) {
    container::String nextSelection;
    bool nextHasCommandSet = false;
    SlotArray nextSlots = buildSlots(
        content, selectedObjectType, selectedCommandSet,
        [&effectiveButtonNames](const game::CommandSetTemplate&, size_t slot) {
            return container::StringView{effectiveButtonNames[slot]};
        },
        nextSelection, nextHasCommandSet);
    return commit(std::move(nextSlots), std::move(nextSelection), nextHasCommandSet);
}

bool ScriptCommandBarPresentationConsumer::commit(
    SlotArray slots, container::String selectedObjectType, bool hasCommandSet) {
    if (m_slots == slots && m_selectedObjectType == selectedObjectType &&
        m_hasCommandSet == hasCommandSet) {
        return false;
    }
    m_slots = std::move(slots);
    m_selectedObjectType = std::move(selectedObjectType);
    m_hasCommandSet = hasCommandSet;
    return true;
}

} // namespace engine::script
