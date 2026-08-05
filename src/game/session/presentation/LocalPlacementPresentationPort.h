#pragma once

#include "game/command/GameCommand.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "game/selection/LocalPlacementTerrainPicker.h"
#include "game/session/transaction/GameSessionBuildPlacementEvaluator.h"

#include <optional>

namespace game { struct ObjectArchetype; }

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Local-only placement transaction surface. Preview state remains owned by
// the session presentation domain; the app can only drive this one workflow.
class LocalPlacementPresentationPort final {
public:
    LocalPlacementPresentationPort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation) noexcept;

    [[nodiscard]] selection::LocalPlacementPreviewSnapshot snapshot() const;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool begin(
        ObjectId sourceObject,
        container::StringView productType,
        selection::LocalPlacementBackendKind backend =
            selection::LocalPlacementBackendKind::Build,
        CommandActivationContext activation = {});
    [[nodiscard]] bool updatePose(
        render::RenderVector position,
        math::q32_32 yawRadians) noexcept;
    // Projects the anchored placement gesture into the same world-space
    // contract as RefCode's InGameUI::handleBuildPlacements: the first point
    // owns the building position while the start-to-end vector owns yaw.
    // LINEBUILD templates retain their tiled end-point semantics.
    [[nodiscard]] bool updateFromAnchors(
        render::RenderVector anchorStart,
        render::RenderVector anchorEnd,
        bool forceAttackSnap = false);
    [[nodiscard]] std::optional<render::RenderVector> screenToTerrain(
        const GameCameraState& camera,
        selection::LocalPlacementViewport viewport,
        float screenX,
        float screenY) const noexcept;
    [[nodiscard]] bool planLine(
        render::RenderVector anchorStart,
        render::RenderVector anchorEnd,
        bool forceAttackSnap = false);
    [[nodiscard]] bool refreshLegality(bool finalConfirmation = false);
    [[nodiscard]] std::optional<GameCommand> takeCommand();
    // Converts one Shift-click candidate into a presentation-only local route
    // node. It does not publish a GameCommand or mutate ObjectOrderQueue.
    [[nodiscard]] std::optional<selection::LocalPlacementPreviewSnapshot>
    makeLocalConstructionRouteNodeFromAnchors(
        const selection::LocalPlacementPreviewSnapshot& placement,
        CommandPosition anchorStart, CommandPosition anchorEnd,
        bool hasDirection, bool forceAttackSnap);
    // Final route-node admission happens only once the builder has arrived.
    // The result is one ordinary non-queued deterministic Build command.
    [[nodiscard]] std::optional<GameCommand>
    composeLocalConstructionRouteBuildCommand(
        const selection::LocalPlacementPreviewSnapshot& placement);
    // GameLogic owns the route. This port retains only a presentation mirror
    // which the existing placement-node/link extraction already consumes.
    void setLocalConstructionRoutePreviews(
        container::Span<const selection::LocalPlacementPreviewSnapshot>
            previews);
    [[nodiscard]] std::optional<CommandPosition>
    localConstructionRouteApproachTarget(
        ObjectId builder,
        const selection::LocalPlacementPreviewSnapshot& placement) const
        noexcept;
    [[nodiscard]] bool builderReachedLocalConstructionRouteNode(
        ObjectId builder,
        const selection::LocalPlacementPreviewSnapshot& placement) const
        noexcept;
    [[nodiscard]] bool builderHasPendingConstruction(
        ObjectId builder) const noexcept;
    [[nodiscard]] bool cancel(
        render::RenderEntityId expectedPreviewIdentity = 0) noexcept;

private:
    [[nodiscard]] bool publishLegality(
        selection::LocalPlacementLegality legality,
        container::Span<const render::TerrainBibFootprintInput> obstructions);
    [[nodiscard]] GameSessionBuildPlacementLegalityEvaluation evaluate(
        const selection::LocalPlacementPreviewSnapshot& placement,
        PlayerId player, const game::ObjectArchetype& product,
        bool finalConfirmation,
        bool requireBuilderReachability = true);
    [[nodiscard]] std::optional<GameCommand> composeCommand(
        const selection::LocalPlacementPreviewSnapshot& placement,
        bool requirePublishedLegality, bool finalRevalidation,
        bool queuedPlanning = false);

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionBuildPlacementEvaluator m_evaluator;
    GameSessionBuildPlacementRejection m_lastLoggedRejection =
        GameSessionBuildPlacementRejection::None;
};

} // namespace engine
