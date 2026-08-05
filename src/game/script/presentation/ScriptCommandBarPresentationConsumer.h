#pragma once

#include "core/container/container_types.h"

#include "game/command/CommandBarOverrides.h"
#include "game/command/CommandButtonTypes.h"
#include <cstddef>
namespace engine {
class GameContentSnapshot;
}

namespace engine::script {

// Value-only description of one physical command-bar slot.  It deliberately
// contains no Widget, CommandButton pointer, ECS entity or order payload:
// widgets may rebuild after a WND reload, while command dispatch still needs
// the future selected-object/order bridge.
struct ScriptCommandBarUiSlot final {
    bool visible = false;
    container::String commandButtonName;
    container::String buttonImage;
    container::String textLabel;
    container::String descriptionLabel;
    game::CommandButtonBorderType borderType =
        game::CommandButtonBorderType::None;

    friend bool operator==(const ScriptCommandBarUiSlot&,
                           const ScriptCommandBarUiSlot&) = default;
};

// Client-local projection of the effective CommandSet for one selected
// ObjectType.  It consumes the immutable session content snapshot plus the
// session-owned COMMANDBAR_* override table, but intentionally receives the
// selected ObjectType as a value.  Selection ownership/picking can therefore
// remain local and evolve independently from script state or ECS storage.
class ScriptCommandBarPresentationConsumer final {
public:
    static constexpr size_t kSlotCount = game::CommandBarOverrideState::kSlotCount;
    using SlotArray = container::Array<ScriptCommandBarUiSlot, kSlotCount>;
    using ButtonNameArray = container::Array<container::String, kSlotCount>;

    void clear() noexcept;

    // Rebuilds the presentation only when its value output changed.  Unknown
    // object types/command sets and an empty selection intentionally produce
    // an empty bar, matching the original ControlBar's no-command fallback.
    [[nodiscard]] bool synchronize(const GameContentSnapshot& content,
                                   const game::CommandBarOverrideState& overrides,
                                   container::StringView selectedObjectType);
    // Integration callers resolve the selected object's effective CommandSet
    // and slot names at the session boundary. The pure consumer retains only
    // copied values and therefore never depends on GameSession or ECS.
    [[nodiscard]] bool synchronizeEffective(
        const GameContentSnapshot& content,
        container::StringView selectedObjectType,
        container::StringView selectedCommandSet,
        const ButtonNameArray& effectiveButtonNames);

    [[nodiscard]] const SlotArray& slots() const noexcept { return m_slots; }
    [[nodiscard]] bool hasSelection() const noexcept { return !m_selectedObjectType.empty(); }
    [[nodiscard]] bool hasCommandSet() const noexcept { return m_hasCommandSet; }
    [[nodiscard]] container::StringView selectedObjectType() const noexcept {
        return m_selectedObjectType;
    }

private:
    [[nodiscard]] bool commit(SlotArray slots, container::String selectedObjectType,
                              bool hasCommandSet);

    SlotArray m_slots{};
    container::String m_selectedObjectType;
    bool m_hasCommandSet = false;
};

} // namespace engine::script
