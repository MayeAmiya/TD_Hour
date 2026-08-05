#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/player/PlayerTypes.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/selection/LocalSelectionState.h"
#include "math/fixed/q32_32.h"

#include <cstddef>
#include <optional>

namespace game {
class CommandBarOverrideState;
struct CommandBarOverrideMutationStamp;
struct CommandButtonTemplate;
struct WeaponTemplate;
}

namespace engine {

namespace script { class ScriptCommandBarPresentationConsumer; }
namespace selection { class LocalSelectionState; }

class GameContentSnapshot;
class GameSession;
class GameSessionCommandQueryPort;
class GameSessionEconomyQueryPort;
class ObjectLifecycle;
class ObjectOwnershipIndex;
class PlayerRegistry;
struct PlayerState;
struct SpecialPowerDefinition;

enum class InGameProductionReadKind : uint8_t {
    Unit,
    PlayerUpgrade,
    ObjectUpgrade,
};

struct InGameProductionReadItem final {
    static constexpr size_t kMaximumBatchCancellationCount = 5;

    uint32_t productionId = 0;
    InGameProductionReadKind kind = InGameProductionReadKind::Unit;
    container::String upgradeName;
    container::String buttonImage;
    container::String textLabel;
    uint16_t progressPermille = 0;
    // Number of consecutive queue entries represented by this UI item. This
    // is deliberately unrelated to ObjectProductionJob::quantityTotal: one
    // authored production can spawn several objects while still occupying a
    // single queue entry.
    uint16_t queuedCount = 1;
    // Newest five real jobs in this consecutive visual run. A grouped queue
    // item remains presentation-only; cancellation still emits one ordinary
    // deterministic command per concrete production id.
    container::Array<uint32_t, kMaximumBatchCancellationCount>
        cancellationProductionIds{};
    uint8_t cancellationProductionIdCount = 0;
};

struct InGameProductionReadModel final {
    uint64_t revision = 0;
    uint32_t capacity = 0;
    size_t totalCount = 0;
    container::Vector<InGameProductionReadItem> items;
};

struct InGameConstructionReadModel final {
    bool underConstruction = false;
    uint16_t progressPermille = 0;
};

// Value-only builder state used by main-thread local input workflows.  It
// deliberately exposes only the active Build correlation needed to advance a
// client-owned waypoint route; no ECS handle or mutable task storage crosses
// the projection boundary.
struct InGameBuilderConstructionReadModel final {
    ObjectId builder = INVALID_OBJECT_ID;
    bool isBuilder = false;
    bool pendingBuild = false;
    uint32_t sourceSequence = 0;
    uint16_t queuedBuildCount = 0;
};

struct InGameOrderWaypointReadModel final {
    bool exists = false;
    selection::LocalOrderWaypointKind kind =
        selection::LocalOrderWaypointKind::Move;
    container::String objectType;
    container::String portraitImage;
};

struct InGameBeaconReadModel final {
    bool isBeacon = false;
    container::String caption;
    uint64_t revision = 0;
};

struct InGameSelectionUpgradeReadModel final {
    container::String buttonImage;
    bool visible = false;
    bool complete = false;
};

struct InGameSelectionPresentationReadModel final {
    container::String portraitImage;
    container::Array<InGameSelectionUpgradeReadModel, 5> upgrades{};
};

// One live entry of RefCode's InGameUI superweapon timer list. RefCode
// registers a SpecialPowerModule with InGameUI::addSuperweapon when its
// template has PublicTimer + SharedNSync and the owner is a KINDOF_STRUCTURE,
// then draws every registered entry for every player, which is what warns you
// about an *enemy* Particle Cannon / Nuke / Scud Storm. Reported as values so
// the presentation layer never reaches into ECS or the special-power catalog.
struct InGamePublicTimerPowerReadModel final {
    container::String specialPower;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectId source = INVALID_OBJECT_ID;
    uint64_t readyTick = 0;
    bool paused = false;
    // RefCode SuperweaponInfo::m_hiddenByScience / the OBJECT_STATUS_UNDER_
    // CONSTRUCTION skip in the draw loop. Both are reported rather than
    // filtered so the caller can decide, and so a later widget can match
    // RefCode's exact suppression order.
    bool hiddenByScience = false;
    bool underConstruction = false;
};

namespace session_query {

struct InGameCommandEvaluationContext;
struct InGameCommandSlotAvailability;

// Query-domain-only view for ControlBar projection and routing.  Its surface
// is the exact read set shared by those algorithms; it owns no state and
// cannot mutate Session, ECS, players, content or script overrides.
class InGameCommandQuerySource final {
public:
    InGameCommandQuerySource(
        const ecs::registry& registry,
        const PlayerRegistry& players,
        const ObjectOwnershipIndex& ownership,
        const ObjectLifecycle& objects,
        const GameContentSnapshot& content,
        const game::CommandBarOverrideState& commandBarOverrides,
        const container::TreeMap<container::String,
            game::ObjectBuildabilityStatus>& buildabilityOverrides) noexcept
        : m_registry(&registry),
          m_players(&players),
          m_ownership(&ownership),
          m_objects(&objects),
          m_content(&content),
          m_commandBarOverrides(&commandBarOverrides),
          m_buildabilityOverrides(&buildabilityOverrides) {}

    [[nodiscard]] const PlayerState* localPlayer() const noexcept;
    [[nodiscard]] const PlayerState* player(PlayerId id) const noexcept;
    [[nodiscard]] std::optional<PlayerId> ownerOf(
        ObjectId object) const noexcept;
    [[nodiscard]] container::Span<const ObjectId> ownedObjects(
        PlayerId player) const noexcept;
    [[nodiscard]] bool playerHasScience(
        PlayerId player, container::StringView science) const noexcept;
    [[nodiscard]] bool playerHasUpgradeComplete(
        PlayerId player, UpgradeContentId upgrade) const noexcept;
    [[nodiscard]] bool playerHasUpgradeInProgress(
        PlayerId player, UpgradeContentId upgrade) const noexcept;
    [[nodiscard]] bool playerSatisfiesProductionPrerequisites(
        PlayerId player, const game::ObjectArchetype& product) const;
    [[nodiscard]] const game::CommandButtonTemplate* findCommandButton(
        container::StringView name) const noexcept;
    [[nodiscard]] container::SharedPtr<const game::ObjectArchetype>
    findObjectArchetype(container::StringView name) const;
    [[nodiscard]] const SpecialPowerDefinition* findSpecialPower(
        container::StringView name) const noexcept;
    [[nodiscard]] const game::WeaponTemplate* findWeapon(
        game::WeaponContentId weapon) const noexcept;
    [[nodiscard]] bool isCommandBarActor(
        ObjectId object, bool multiSelection) const noexcept;
    [[nodiscard]] container::String objectButtonImage(
        ObjectId object) const;
    [[nodiscard]] InGameSelectionPresentationReadModel
    objectSelectionPresentation(ObjectId object) const;
    [[nodiscard]] InGameConstructionReadModel objectConstruction(
        ObjectId object) const noexcept;
    [[nodiscard]] InGameBuilderConstructionReadModel objectBuilderConstruction(
        ObjectId object) const noexcept;
    [[nodiscard]] InGameOrderWaypointReadModel orderWaypoint(
        ObjectId actor, uint32_t sourceSequence) const;
    [[nodiscard]] uint64_t commandSetRevision(
        ObjectId object) const noexcept;
    [[nodiscard]] InGameProductionReadModel productionQueue(
        ObjectId object, size_t maximumItems) const;
    [[nodiscard]] InGameBeaconReadModel beacon(
        ObjectId object) const;
    // Every player's PublicTimer + SharedNSync special powers, in stable
    // (PlayerId, ObjectId, authored instance) order so the projection is
    // frame-stable without sorting strings.
    [[nodiscard]] container::Vector<InGamePublicTimerPowerReadModel>
    publicTimerSpecialPowers() const;
    [[nodiscard]] bool productionQueueActionCurrent(
        ObjectId producer, uint32_t productionId, InGameProductionReadKind kind,
        container::StringView upgradeName) const noexcept;
    [[nodiscard]] math::q32_32 pendingCommandRadius(
        ObjectId actor, const game::CommandButtonTemplate& button) const;
    // The SpecialPowerModule's authored InitiateSound is an acknowledgement
    // cue, not a CommandButton sound.  Keep this narrow confirmed-world read
    // behind the query port so local input never inspects an ECS component.
    [[nodiscard]] container::String specialPowerInitiateSound(
        ObjectId actor, const game::CommandButtonTemplate& button) const;
    [[nodiscard]] bool synchronizeCommandBar(
        script::ScriptCommandBarPresentationConsumer& consumer,
        ObjectId object,
        container::Span<const container::String> effectiveButtons) const;
    [[nodiscard]] InGameCommandSlotAvailability evaluateAvailability(
        const GameSessionCommandQueryPort& commands,
        const GameSessionEconomyQueryPort& economy,
        uint64_t confirmedTick, uint32_t logicFramesPerSecond,
        const selection::LocalSelectionState& selection,
        ObjectId actor,
        const game::CommandButtonTemplate& button,
        bool sourceVisible,
        ObjectId commandTarget,
        PlayerId evaluatingPlayer = INVALID_PLAYER_ID) const;
    [[nodiscard]] game::CommandBarOverrideMutationStamp
    commandBarMutation() const noexcept;
    [[nodiscard]] container::StringView objectTypeName(
        ObjectId object) const noexcept;
    [[nodiscard]] container::StringView effectiveCommandSetName(
        ObjectId object) const noexcept;
    [[nodiscard]] container::StringView effectiveObjectCommandBarButton(
        ObjectId object, size_t slot) const;
    [[nodiscard]] std::optional<game::ObjectBuildabilityStatus>
    effectiveScriptObjectBuildability(
        container::StringView objectType) const noexcept;

private:
    // Raw state is confined to the single command-availability algorithm;
    // every other caller uses the semantic/value methods above.
    [[nodiscard]] const ecs::registry& registry() const noexcept;
    [[nodiscard]] const GameContentSnapshot& contentSnapshot() const noexcept;
    [[nodiscard]] std::optional<ecs::entity> entityFromId(
        ObjectId object) const noexcept;

    const ecs::registry* m_registry = nullptr;
    const PlayerRegistry* m_players = nullptr;
    const ObjectOwnershipIndex* m_ownership = nullptr;
    const ObjectLifecycle* m_objects = nullptr;
    const GameContentSnapshot* m_content = nullptr;
    const game::CommandBarOverrideState* m_commandBarOverrides = nullptr;
    const container::TreeMap<container::String,
        game::ObjectBuildabilityStatus>* m_buildabilityOverrides = nullptr;
};

[[nodiscard]] InGameCommandQuerySource inGameCommandQuerySource(
    const GameSession& session) noexcept;

} // namespace session_query
} // namespace engine
