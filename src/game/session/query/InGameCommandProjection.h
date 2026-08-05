#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/command/CommandButtonStore.h"
#include "game/command/CommandBarOverrides.h"
#include "game/player/PlayerTypes.h"
#include "game/selection/PendingWorldCommandMode.h"
#include "game/selection/LocalSelectionState.h"
#include "game/script/presentation/ScriptCommandBarPresentationConsumer.h"

#include <cstddef>
#include <cstdint>

namespace engine {
class GameSession;
class GameSessionCommandQueryPort;
class GameSessionEconomyQueryPort;
namespace selection {
class LocalSelectionState;
}
}

namespace engine::session_query {

class InGameCommandQuerySource;

struct InGameCommandEvaluationContext final {
    const InGameCommandQuerySource& source;
    const GameSessionCommandQueryPort& commands;
    const GameSessionEconomyQueryPort& economy;
    uint64_t confirmedTick = 0;
    uint32_t logicFramesPerSecond = 30;
    // Invalid selects the observer-local player for UI projection. Confirmed
    // command admission supplies the command envelope's player explicitly so
    // the same pure rule remains valid for replay/network inputs.
    engine::PlayerId player = engine::INVALID_PLAYER_ID;
};

enum class InGameCommandAvailabilityReason : uint8_t {
    None,
    MissingButton,
    UnsupportedCommand,
    MissingContentReference,
    UnauthorizedActor,
    ActorUnavailable,
    ScriptDisabled,
    Unmanned,
    Disabled,
    Underpowered,
    MustBeStopped,
    SingleUseConsumed,
    MissingCapability,
    MissingPrerequisiteScience,
    MissingPrerequisiteUpgrade,
    ProductUnavailable,
    PrerequisitesNotMet,
    MaximumSimultaneousReached,
    QueueBusy,
    QueueFull,
    ParkingPlacesFull,
    AlreadyComplete,
    AlreadyInProgress,
    InsufficientFunds,
    Cooldown,
    NoPassengers,
    Unsellable,
};

struct InGameCommandCooldownProjection final {
    uint64_t remainingTicks = 0;
    uint64_t totalTicks = 0;
    uint16_t readyPermille = 1000;

    friend bool operator==(const InGameCommandCooldownProjection&,
                           const InGameCommandCooldownProjection&) = default;
};

struct InGameCommandQueueProjection final {
    uint64_t revision = 0;
    uint32_t headProductionId = 0;
    uint16_t count = 0;
    uint16_t capacity = 0;
    uint16_t headProgressPermille = 0;

    friend bool operator==(const InGameCommandQueueProjection&,
                           const InGameCommandQueueProjection&) = default;
};

// Logic-thread-authored value for one physical ControlBar slot.  It is POD-
// shaped and pointer-free so WND/input code can consume it without touching
// ECS, content stores, or reparsing CommandButton strings.
struct InGameCommandSlotAvailability final {
    bool visible = false;
    bool enabled = false;
    bool active = false;
    InGameCommandAvailabilityReason reason =
        InGameCommandAvailabilityReason::MissingButton;
    int64_t cost = 0;
    InGameCommandCooldownProjection cooldown;
    InGameCommandQueueProjection queue;
    uint64_t revision = 0;

    friend bool operator==(const InGameCommandSlotAvailability&,
                           const InGameCommandSlotAvailability&) = default;
};

// Self-contained identity for a command-bar click. Logic can reject a token
// whose revision no longer matches the current projected bar without reading
// a widget, retained content object, or ECS entity handle.
struct InGameCommandActionToken final {
    uint64_t commandBarRevision = 0;
    uint64_t selectionRevision = 0;
    engine::ObjectId selectedObject = engine::INVALID_OBJECT_ID;
    // EXIT_CONTAINER inventory slots carry the passenger separately from the
    // selected container. Other command families leave this invalid.
    engine::ObjectId targetObject = engine::INVALID_OBJECT_ID;
    bool orderWaypoint = false;
    uint32_t orderWaypointSourceSequence = 0;
    engine::selection::LocalOrderWaypointKind orderWaypointKind =
        engine::selection::LocalOrderWaypointKind::Move;
    uint32_t slot = 0;
    // POD compiled at content load. Posting a click neither allocates nor
    // carries a behavior-defining Command/Options string across threads.
    game::CommandButtonDescriptor descriptor;
    InGameCommandSlotAvailability availability;
    // Presentation resource only. It never influences routing or authority;
    // ZH plays this AudioEvent when an enabled CommandButton is clicked.
    container::String unitSpecificSound;

    [[nodiscard]] bool isValid() const noexcept {
        return commandBarRevision != 0 && selectedObject &&
            descriptor.userActivatable() && availability.visible &&
            availability.enabled;
    }

    friend bool operator==(const InGameCommandActionToken&,
                           const InGameCommandActionToken&) = default;
};

// Multi-selection commands are presented once, but ZH enables the shared
// button when any selected object can execute it.  Keep the chosen executable
// actor beside the aggregate value so click revalidation and routing do not
// accidentally fall back to the first (possibly disabled) selection member.
struct InGameCommandAggregateAvailability final {
    engine::ObjectId actor = engine::INVALID_OBJECT_ID;
    InGameCommandSlotAvailability availability;
    uint16_t availableActorCount = 0;
};

struct PendingWorldCommandRevalidation final {
    bool descriptorCurrent = false;
    InGameCommandSlotAvailability availability;
};

enum class InGameProductionQueueItemKind : uint8_t {
    Unit,
    PlayerUpgrade,
    ObjectUpgrade,
};

struct InGameProductionQueueActionToken final {
    static constexpr size_t kMaximumBatchCancellationCount = 5;

    uint64_t selectionRevision = 0;
    engine::ObjectId producer = engine::INVALID_OBJECT_ID;
    uint32_t productionId = 0;
    InGameProductionQueueItemKind kind =
        InGameProductionQueueItemKind::Unit;
    container::String upgradeName;
    container::Array<uint32_t, kMaximumBatchCancellationCount>
        cancellationProductionIds{};
    uint8_t cancellationProductionIdCount = 0;

    [[nodiscard]] bool isValid() const noexcept {
        if (selectionRevision == 0 || !producer || productionId == 0 ||
            cancellationProductionIdCount == 0 ||
            cancellationProductionIdCount >
                kMaximumBatchCancellationCount ||
            (kind != InGameProductionQueueItemKind::Unit &&
             upgradeName.empty()) ||
            cancellationProductionIds[
                cancellationProductionIdCount - 1u] != productionId) {
            return false;
        }
        for (uint8_t index = 0;
             index < cancellationProductionIdCount; ++index) {
            if (cancellationProductionIds[index] == 0) return false;
        }
        return true;
    }

    friend bool operator==(const InGameProductionQueueActionToken&,
                           const InGameProductionQueueActionToken&) = default;
};

struct InGameProductionQueueItemProjection final {
    InGameProductionQueueActionToken action;
    container::String buttonImage;
    container::String textLabel;
    uint16_t progressPermille = 0;
    uint16_t queuedCount = 1;

    friend bool operator==(const InGameProductionQueueItemProjection&,
                           const InGameProductionQueueItemProjection&) = default;
};

struct InGameProductionQueueProjection final {
    static constexpr size_t kMaximumVisibleItems = 9;

    uint64_t revision = 0;
    engine::ObjectId producer = engine::INVALID_OBJECT_ID;
    uint16_t count = 0;
    uint16_t capacity = 0;
    uint16_t visibleItemCount = 0;
    container::Array<InGameProductionQueueItemProjection,
                     kMaximumVisibleItems> items{};

    [[nodiscard]] bool visible() const noexcept {
        return producer && visibleItemCount != 0;
    }

    friend bool operator==(const InGameProductionQueueProjection&,
                           const InGameProductionQueueProjection&) = default;
};

struct InGameBeaconProjection final {
    engine::ObjectId object = engine::INVALID_OBJECT_ID;
    bool isBeacon = false;
    bool locallyControlled = false;
    container::String caption;
    uint64_t revision = 0;

    friend bool operator==(const InGameBeaconProjection&,
                           const InGameBeaconProjection&) = default;
};

// Owned logic-thread projection for command-bar and Beacon UI. All strings,
// slots, stamps and identifiers are values; no session, ECS, archetype, or
// content pointer survives build().
struct InGameCommandProjection final {
    static constexpr size_t kSlotCount =
        engine::script::ScriptCommandBarPresentationConsumer::kSlotCount;
    using SlotArray =
        engine::script::ScriptCommandBarPresentationConsumer::SlotArray;
    using ActionTokenArray =
        container::Array<InGameCommandActionToken, kSlotCount>;
    using AvailabilityArray =
        container::Array<InGameCommandSlotAvailability, kSlotCount>;
    using InventoryPassengerArray =
        container::Array<engine::ObjectId, kSlotCount>;
    using InventorySlotArray = container::Array<bool, kSlotCount>;
    using ActionActorArray =
        container::Array<engine::ObjectId, kSlotCount>;

    bool hasSelection = false;
    bool multiSelection = false;
    uint32_t selectedCount = 0;
    engine::ObjectId selectedObject = engine::INVALID_OBJECT_ID;
    container::String selectedObjectType;
    container::String selectedPortraitImage;
    struct UpgradeCameo final {
        container::String buttonImage;
        bool visible = false;
        bool complete = false;

        friend bool operator==(const UpgradeCameo&,
                               const UpgradeCameo&) = default;
    };
    container::Array<UpgradeCameo, 5> upgradeCameos{};

    bool selectedUnderConstruction = false;
    uint16_t constructionProgressPermille = 0;
    bool selectedOrderWaypoint = false;
    uint32_t selectedOrderWaypointSourceSequence = 0;
    engine::selection::LocalOrderWaypointKind selectedOrderWaypointKind =
        engine::selection::LocalOrderWaypointKind::Move;

    bool hasCommandSet = false;
    SlotArray slots{};

    // Source values are retained alongside the monotonic value revision so a
    // receiver can diagnose or revalidate an action without content pointers.
    uint64_t selectionRevision = 0;
    game::CommandBarOverrideMutationStamp commandBarMutation{};
    uint64_t objectCommandSetRevision = 0;
    uint64_t commandBarRevision = 0;
    AvailabilityArray availability{};
    ActionTokenArray actionTokens{};
    // EXIT_CONTAINER is a dynamic inventory control.  An empty slot must use
    // the active faction ControlBarScheme's CommandMarkerImage rather than
    // the static CommandButton cameo; Presentation resolves that local image
    // while this logic-thread snapshot retains only the slot semantics.
    InventorySlotArray inventorySlots{};
    InventoryPassengerArray inventoryPassengers{};
    ActionActorArray actionActors{};
    InGameProductionQueueProjection productionQueue;

    InGameBeaconProjection beacon;
};

class InGameCommandProjectionPublisher final {
public:
    [[nodiscard]] InGameCommandProjection build(
        const engine::GameSession& session,
        const engine::selection::LocalSelectionState& selection);

private:
    uint64_t m_nextCommandBarRevision = 1;
    container::Array<uint64_t, InGameCommandProjection::kSlotCount>
        m_nextSlotRevision{};
    bool m_hasPrevious = false;
    InGameCommandProjection m_previous;
};

// Session integration adapter for the pure command-bar presentation consumer.
// It copies effective per-object CommandSet slot names before returning, so
// the consumer never retains GameSession, ECS, or component-backed views.
[[nodiscard]] bool synchronizeEffectiveCommandBar(
    engine::script::ScriptCommandBarPresentationConsumer& consumer,
    const engine::GameSession& session,
    engine::ObjectId object);

// Shared by projection publication and logic-thread click revalidation.  A
// stale UI token is accepted only when this freshly derived value still
// matches the token's visible state.
[[nodiscard]] InGameCommandSlotAvailability
evaluateInGameCommandAvailability(
    const InGameCommandEvaluationContext& context,
    const engine::selection::LocalSelectionState& selection,
    engine::ObjectId actor,
    const game::CommandButtonTemplate& button,
    bool sourceVisible = true,
    engine::ObjectId commandTarget = engine::INVALID_OBJECT_ID);

[[nodiscard]] InGameCommandSlotAvailability
evaluateInGameCommandAvailability(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    engine::ObjectId actor,
    const game::CommandButtonTemplate& button,
    bool sourceVisible = true,
    engine::ObjectId commandTarget = engine::INVALID_OBJECT_ID);

[[nodiscard]] InGameCommandAggregateAvailability
evaluateInGameMultiCommandAvailability(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    const game::CommandButtonTemplate& button);

[[nodiscard]] InGameCommandAggregateAvailability
evaluateInGameShortcutAvailability(
    const InGameCommandEvaluationContext& context,
    engine::PlayerId player,
    const game::CommandButtonTemplate& button);

[[nodiscard]] InGameCommandAggregateAvailability
evaluateInGameShortcutAvailability(
    const engine::GameSession& session,
    engine::PlayerId player,
    const game::CommandButtonTemplate& button);

[[nodiscard]] PendingWorldCommandRevalidation
revalidatePendingWorldCommand(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    const engine::selection::PendingWorldCommandMode& mode);

// Revalidates a queue row token entirely inside the confirmed game domain.
// App code owns only the copied token and never inspects production ECS data.
[[nodiscard]] bool isCurrentProductionQueueAction(
    const engine::GameSession& session, engine::PlayerId player,
    const InGameProductionQueueActionToken& token) noexcept;

// Deterministically maps an authored EXIT_CONTAINER slot to the corresponding
// sorted containment record. Both projection and click revalidation use this
// helper; no widget-owned ObjectId crosses back into simulation.
[[nodiscard]] engine::ObjectId resolveInGameContainmentPassenger(
    const engine::GameSession& session, engine::ObjectId container,
    container::Span<const engine::script::ScriptCommandBarUiSlot> slots,
    size_t slotIndex);

[[nodiscard]] bool sameInGameCommandAvailabilityState(
    const InGameCommandSlotAvailability& lhs,
    const InGameCommandSlotAvailability& rhs) noexcept;

} // namespace engine::session_query
