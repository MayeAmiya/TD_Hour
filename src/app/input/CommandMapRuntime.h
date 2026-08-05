#pragma once

#include "core/container/container_types.h"

#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_keycode.h>

#include <cstdint>
#include <optional>

namespace app::input {

enum class CommandMapAction : uint8_t {
    Unknown,
    SaveView,
    ViewSaved,
    CreateTeam,
    SelectTeam,
    AddTeam,
    ViewTeam,
    SelectMatchingUnits,
    SelectNextUnit,
    SelectPreviousUnit,
    SelectNextWorker,
    SelectPreviousWorker,
    SelectHero,
    ViewCommandCenter,
    ViewLastRadarEvent,
    SelectAll,
    Scatter,
    Stop,
    CreateFormation,
    ChatAllies,
    ChatEveryone,
    Diplomacy,
    PlaceBeacon,
    DeleteBeacon,
    Options,
    ToggleControlBar,
    BeginForceAttack,
    EndForceAttack,
    // Preserved for authored CommandMap overrides.  Retail ships the meta
    // messages and CommandXlat handling even though its stock map does not
    // bind a key; mods use this mode to issue an intentional Move (rather
    // than context Attack/Enter) and to select VoiceCrush where applicable.
    BeginForceMove,
    EndForceMove,
    BeginWaypoints,
    EndWaypoints,
    BeginPreferSelection,
    EndPreferSelection,
    TakeScreenshot,
    BeginCameraRotateLeft,
    EndCameraRotateLeft,
    BeginCameraRotateRight,
    EndCameraRotateRight,
    BeginCameraZoomIn,
    EndCameraZoomIn,
    BeginCameraZoomOut,
    EndCameraZoomOut,
    CameraReset,
    ToggleCameraTrackingDrawable,
};

enum class CommandMapTransition : uint8_t { Down, Up };

enum CommandMapModifier : uint8_t {
    CommandMapModifierNone = 0,
    CommandMapModifierShift = 1u << 0u,
    CommandMapModifierControl = 1u << 1u,
    CommandMapModifierAlt = 1u << 2u,
};

enum CommandMapContext : uint8_t {
    CommandMapContextNone = 0,
    CommandMapContextGame = 1u << 0u,
};

struct CommandMapBinding final {
    CommandMapAction action = CommandMapAction::Unknown;
    uint8_t ordinal = 0;
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    CommandMapTransition transition = CommandMapTransition::Down;
    uint8_t modifiers = CommandMapModifierNone;
    uint8_t contexts = CommandMapContextNone;
};

class CommandMapRuntime final {
public:
    void reloadIfNeeded();
    void resetActiveBindings() noexcept;
    [[nodiscard]] std::optional<CommandMapBinding> match(
        SDL_Scancode scancode,
        bool keyDown,
        SDL_Keymod modifiers,
        bool inGame,
        bool repeat) noexcept;

private:
    bool load();
    bool parse(container::StringView content);

    uint64_t m_contentRevision = UINT64_MAX;
    container::Vector<CommandMapBinding> m_bindings;
    container::Array<std::optional<CommandMapBinding>, SDL_SCANCODE_COUNT>
        m_activeBindings{};
};

} // namespace app::input
