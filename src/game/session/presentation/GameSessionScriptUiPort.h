#pragma once

#include "game/script/bridge/ScriptSessionEvents.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/script/presentation/ScriptUiPresentationControls.h"

#include <cstdint>
#include <optional>

namespace engine {

class GameSessionScriptPresentationState;

// Script-authored UI journal source. It exposes only UI state, counter values
// needed by indicators, and destructive drains owned by the UI publisher.
class GameSessionScriptUiPort final {
public:
    explicit GameSessionScriptUiPort(
        GameSessionScriptPresentationState& presentation) noexcept
        : m_presentation(&presentation) {}

    [[nodiscard]] uint64_t presentationEpoch() const noexcept;
    [[nodiscard]] script::ScriptUiPresentationState state() const;
    [[nodiscard]] script::ScriptLetterboxPresentationState letterbox()
        const noexcept;
    [[nodiscard]] std::optional<int32_t> counterValue(
        container::StringView name) const noexcept;
    [[nodiscard]] container::Vector<script::ScriptSessionEvent>
    takeSessionEvents();
    [[nodiscard]] container::Vector<script::ScriptCameoFlashPresentation>
    takeCameoFlashes();

private:
    GameSessionScriptPresentationState* m_presentation = nullptr;
};

} // namespace engine
