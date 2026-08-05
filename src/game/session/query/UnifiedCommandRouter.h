#pragma once

#include "game/session/query/InGameCommandProjection.h"

#include "game/command/GameCommand.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "game/selection/PendingWorldCommandMode.h"

#include <cstdint>
#include <optional>

namespace engine {
class GameSession;
namespace selection {
class LocalSelectionState;
}
}

namespace engine::session_query {

// One and only one typed boundary between an authoritative ControlBar click
// and the currently implemented gameplay/local-presentation backends.  The
// caller must re-resolve the live slot and availability before entering this
// router; authored Command/Options strings are never interpreted here.
enum class UnifiedCommandRouteKind : uint8_t {
    Rejected,
    UnsupportedBackend,
    SubmitGameCommand,
    BeginPendingWorldTarget,
    BeginPlacement,
    CancelPlacement,
    ApplySelectionByType,
};

enum class UnifiedCommandRouteRejection : uint8_t {
    None,
    InvalidDescriptor,
    Unavailable,
    InvalidSelection,
    InvalidLocalPlayer,
    UnauthorizedActor,
    MissingRoutePayload,
    CommandCompositionRejected,
};

// Backend gaps are deliberately grouped by behavior owner rather than by UI
// spelling.  K-010 can project these values without parsing diagnostics.
enum class UnifiedCommandBackendGap : uint8_t {
    None,
    GuardMode,
    WaypointMode,
    Beacon,
    SciencePurchase,
    Overcharge,
};

enum class UnifiedCommandPostAcceptHookKind : uint8_t {
    None,
    MarkSingleUseCommandUsed,
};

// This hook is descriptive until command submission exposes a confirmed
// CommandDispatcher acceptance callback.  It must never be applied merely
// because the ControlBar click was routed or queued.
struct UnifiedCommandPostAcceptHook final {
    UnifiedCommandPostAcceptHookKind kind =
        UnifiedCommandPostAcceptHookKind::None;
    engine::ObjectId actor = engine::INVALID_OBJECT_ID;
    uint64_t commandButtonStableId = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return kind != UnifiedCommandPostAcceptHookKind::None && actor &&
            commandButtonStableId != 0;
    }
};

struct UnifiedCommandRouteResult final {
    UnifiedCommandRouteKind kind = UnifiedCommandRouteKind::Rejected;
    UnifiedCommandRouteRejection rejection =
        UnifiedCommandRouteRejection::None;
    UnifiedCommandBackendGap backendGap = UnifiedCommandBackendGap::None;
    game::CommandButtonKind commandKind = game::CommandButtonKind::Unknown;
    // Client-only acknowledgement emitted only after the command has entered
    // the local authoritative queue. It is never serialized with GameCommand.
    container::String localVoiceEvent;
    engine::ObjectId localVoiceObject = engine::INVALID_OBJECT_ID;
    std::optional<engine::GameCommand> command;
    std::optional<engine::selection::PendingWorldCommandMode> pendingTarget;
    container::String placementProduct;
    engine::selection::LocalPlacementBackendKind placementBackend =
        engine::selection::LocalPlacementBackendKind::Build;
    container::String selectionObjectType;
    UnifiedCommandPostAcceptHook postAccept;
};

struct UnifiedShortcutRouteResult final {
    game::CommandButtonDescriptor descriptor;
    engine::ObjectId actor = engine::INVALID_OBJECT_ID;
    UnifiedCommandRouteResult route;
};

enum class UnifiedTokenRouteRejection : uint8_t {
    None,
    InvalidSlot,
    SlotUnavailable,
    DescriptorChanged,
    AvailabilityChanged,
};

struct UnifiedTokenRouteResult final {
    UnifiedTokenRouteRejection rejection = UnifiedTokenRouteRejection::None;
    UnifiedCommandRouteResult route;
};

class UnifiedCommandRouter final {
public:
    [[nodiscard]] static UnifiedShortcutRouteResult routeShortcut(
        const engine::GameSession& session,
        container::StringView commandButtonName,
        uint64_t buttonStableId,
        engine::GameTick tick);

    [[nodiscard]] static UnifiedTokenRouteResult routeActionToken(
        const engine::GameSession& session,
        const engine::selection::LocalSelectionState& selection,
        const InGameCommandActionToken& token,
        engine::GameTick tick);

    [[nodiscard]] static UnifiedCommandRouteResult route(
        const engine::GameSession& session,
        const engine::selection::LocalSelectionState& selection,
        engine::ObjectId actor,
        const game::CommandButtonDescriptor& descriptor,
        const game::CommandButtonTemplate& button,
        const InGameCommandSlotAvailability& availability,
        engine::GameTick tick,
        engine::ObjectId commandTarget = engine::INVALID_OBJECT_ID);
};

} // namespace engine::session_query
