#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/command/CommandButtonStore.h"
#include "game/command/GameCommand.h"

#include <cstdint>
#include <optional>

namespace engine::selection {

// Local-only command targeting state.  Authored CommandButton strings are
// decoded once when the button is activated; input and confirmation route on
// these enums and never reinterpret Command/Options text.
enum class PendingWorldCommandKind : uint8_t {
    None,
    AttackMove,
    Guard,
    SpecialPower,
    FireWeapon,
    SetRallyPoint,
    CombatDrop,
    IntentionalContact,
};

using PendingWorldTargetKind = game::CommandButtonTargetKind;

enum class PendingWorldCursorKind : uint8_t {
    None,
    AttackMove,
    Guard,
    SpecialPower,
    FireWeapon,
    SetRallyPoint,
    CombatDrop,
    IntentionalContact,
};

enum class PendingWorldVoiceKind : uint8_t {
    None,
    AttackMove,
    Guard,
    SpecialPower,
    FireWeapon,
    CombatDrop,
    IntentionalContact,
};

enum class PendingWorldTargetRelation : uint8_t {
    None = 0,
    Enemy = 1u << 0,
    Ally = 1u << 1,
    Neutral = 1u << 2,
};

[[nodiscard]] constexpr PendingWorldTargetRelation operator|(
    PendingWorldTargetRelation lhs,
    PendingWorldTargetRelation rhs) noexcept {
    return static_cast<PendingWorldTargetRelation>(
        static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

struct PendingWorldCursorDescriptor final {
    PendingWorldCursorKind kind = PendingWorldCursorKind::None;
    // Resource identifiers are data, not behavior protocol.  A later cursor
    // renderer can resolve these without reparsing CommandButton options.
    container::String validResource;
    container::String invalidResource;
    container::String radiusResource;
    // Frozen authoritative world radius. Zero keeps the ordinary pointer-only
    // cursor. Presentation converts to float only when projecting the decal.
    math::q32_32 radiusWorld{};
};

struct PendingWorldVoiceDescriptor final {
    PendingWorldVoiceKind kind = PendingWorldVoiceKind::None;
    container::String unitSpecificSound;
};

struct PendingWorldCommandMode final {
    uint64_t revision = 0;
    // Correlates the local targeting phase and the eventual confirmed command
    // with the original ControlBar activation receipt.
    uint64_t requestSequence = 0;
    uint64_t buttonStableId = 0;
    game::CommandButtonKind commandKind = game::CommandButtonKind::Unknown;
    ObjectId sourceObject = INVALID_OBJECT_ID;
    // Shortcut powers resolve a provider from the local player's complete
    // object set without changing the user's visible selection.
    bool sourceMayBeUnselected = false;
    // Frozen at command-button activation. A user may release Shift while
    // aiming a targeted power; the eventual confirmed command must still be
    // appended to the route which the hotkey activation requested.
    bool queued = false;
    PendingWorldCommandKind kind = PendingWorldCommandKind::None;
    PendingWorldTargetKind targetKind = PendingWorldTargetKind::None;
    PendingWorldTargetRelation allowedRelations =
        PendingWorldTargetRelation::None;
    bool allowShrubberyTarget = false;
    bool allowMineTarget = false;
    // Immutable authored content identity used by the existing command
    // dispatcher. It is never inspected to choose the local input behavior.
    container::String commandButtonName;
    // Pointer-free gameplay hook attached to the eventual confirmed command.
    uint64_t postAcceptButtonStableId = 0;
    game::CommandButtonKind postAcceptCommandKind =
        game::CommandButtonKind::Unknown;
    CommandPostAcceptAction postAccept = CommandPostAcceptAction::None;
    ObjectId postAcceptActor = INVALID_OBJECT_ID;
    PendingWorldCursorDescriptor cursor;
    PendingWorldVoiceDescriptor voice;

    [[nodiscard]] bool active() const noexcept {
        return revision != 0 && sourceObject &&
            kind != PendingWorldCommandKind::None &&
            targetKind != PendingWorldTargetKind::None;
    }

    [[nodiscard]] bool acceptsObject() const noexcept {
        return targetKind == PendingWorldTargetKind::Object ||
            targetKind == PendingWorldTargetKind::ObjectOrPosition ||
            targetKind == PendingWorldTargetKind::Contextual;
    }

    [[nodiscard]] bool acceptsPosition() const noexcept {
        return targetKind == PendingWorldTargetKind::Position ||
            targetKind == PendingWorldTargetKind::ObjectOrPosition ||
            targetKind == PendingWorldTargetKind::Contextual;
    }

    friend bool operator==(const PendingWorldCommandMode&,
                           const PendingWorldCommandMode&) = default;
};

// Returns a mode only for a recognized command family which requires a world
// target. No-target variants remain immediate commands for K-006/K-007.
[[nodiscard]] std::optional<PendingWorldCommandMode>
resolvePendingWorldCommandMode(
    const game::CommandButtonTemplate& button,
    ObjectId sourceObject);

} // namespace engine::selection
