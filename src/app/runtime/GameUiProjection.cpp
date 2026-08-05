#include "GameUiProjection.h"

#include "game/session/core/GameSession.h"
#include "game/session/query/LocalSelectionQueryPort.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/PlayerUiProjection.h"
#include "game/session/query/SessionPlayerQuery.h"
#include "game/session/query/SessionRuntimeQuery.h"
#include "game/session/query/WorldCommandComposer.h"
#include "game/session/query/WorldCommandQueryPort.h"

#include <algorithm>
#include <limits>
#include <memory>

namespace app::runtime {
GameUiProjection GameUiProjectionPublisher::build(
    engine::GameLogic& gameLogic) {
    GameUiProjection projection;
    projection.revision = m_nextRevision++;
    if (m_nextRevision == 0) m_nextRevision = 1;
    projection.sessionRevision = gameLogic.sessionRevision();
    projection.gameState = gameLogic.getState();
    projection.loadingStage = gameLogic.loadingStage();
    projection.loadingRevision = gameLogic.loadingRevision();
    projection.loadingProgress = std::clamp(
        gameLogic.loadingProgress(), 0.0f, 1.0f);
    projection.loadingStatus = gameLogic.loadingStatus();
    projection.loadingError = gameLogic.loadingError();
    projection.startupSceneProgress = gameLogic.startupSceneProgress();
    projection.loadingLastProgress = gameLogic.loadingLastProgress();
    projection.loadingDeadlineRemainingMilliseconds =
        gameLogic.loadingDeadlineRemainingMilliseconds();
    projection.resultTransitionError = gameLogic.resultTransitionError();
    projection.disconnect = gameLogic.disconnectStatus();

    const engine::GameStartInfo& currentStart =
        gameLogic.getCurrentGameInfo();
    const bool startChanged =
        !m_cachedStartInfo ||
        m_cachedStartSessionRevision != projection.sessionRevision ||
        m_cachedStartLoadingRevision != projection.loadingRevision ||
        m_cachedStartInfo->mode != currentStart.mode ||
        m_cachedStartInfo->mapName != currentStart.mapName ||
        m_cachedStartInfo->replayFileName != currentStart.replayFileName;
    if (startChanged) {
        m_cachedStartInfo =
            std::make_shared<const engine::GameStartInfo>(currentStart);
        m_cachedStartSessionRevision = projection.sessionRevision;
        m_cachedStartLoadingRevision = projection.loadingRevision;
    }
    projection.startInfo = m_cachedStartInfo;

    engine::GameSession* session = gameLogic.currentSession();
    projection.hasSession = session != nullptr;
    projection.commandOutcomes = gameLogic.commandOutcomes();
    projection.hoveredObject = gameLogic.localSelection().hovered();
    projection.pendingWorldCommand =
        gameLogic.localSelection().pendingWorldCommand();
    if (session) {
        const auto localPlayer = session->playerQuery().localPlayer();
        const auto selected = gameLogic.localSelection().selected();
        projection.localSelectedObjects.assign(
            selected.begin(), selected.end());
        const auto worldCommands = session->worldCommandQuery();
        const auto anySelected = [&](auto&& predicate) {
            return std::any_of(selected.begin(), selected.end(), predicate);
        };
        if (localPlayer && localPlayer->commandPlayer && !selected.empty()) {
            projection.hoveredObjectForceAttackableBySelection =
                projection.hoveredObject &&
                anySelected([&](engine::ObjectId actor) {
                    return worldCommands.actorCanForceAttackTarget(
                        actor, projection.hoveredObject);
            });
            if (projection.hoveredObject) {
                const engine::PlayerRepairTargetAction repairAction =
                    session->commandQuery().repairSelectionTargetAction(
                        localPlayer->id, selected,
                        projection.hoveredObject);
                const bool canEnter = anySelected(
                    [&](engine::ObjectId actor) {
                        return worldCommands.actorCanEnterContainer(
                            actor, projection.hoveredObject);
                    });
                const engine::selection::LocalSelectionObjectSnapshot
                    hoveredContext = session->localSelectionQuery().inspect(
                        localPlayer->id, projection.hoveredObject);
                // Match WorldCommandComposer: repair/resume and repair-dock
                // context are resolved before ordinary containment/attack.
                if (repairAction ==
                        engine::PlayerRepairTargetAction::DoRepair) {
                    projection.worldCursorHint = WorldCursorHint::DoRepair;
                } else if (repairAction == engine::
                               PlayerRepairTargetAction::
                                   ResumeConstruction) {
                    projection.worldCursorHint =
                        WorldCursorHint::ResumeConstruction;
                } else if (repairAction == engine::
                               PlayerRepairTargetAction::GetRepaired) {
                    projection.worldCursorHint =
                        WorldCursorHint::GetRepaired;
                } else if (canEnter) {
                    projection.worldCursorHint = hoveredContext.healPad
                        ? WorldCursorHint::GetHealed
                        : hoveredContext.structure && !hoveredContext.local
                            ? WorldCursorHint::EnterAggressive
                            : WorldCursorHint::EnterFriendly;
                } else {
                switch (worldCommands.contextualTarget(
                    localPlayer->id, projection.hoveredObject)) {
                case engine::selection::WorldContextTargetAction::Attack:
                    projection.worldCursorHint = anySelected(
                        [&](engine::ObjectId actor) {
                            return worldCommands.actorCanAttackTarget(
                                actor, projection.hoveredObject);
                        }) ? WorldCursorHint::AttackObject
                           : WorldCursorHint::Invalid;
                    break;
                case engine::selection::WorldContextTargetAction::Enter:
                    projection.worldCursorHint = WorldCursorHint::Invalid;
                    break;
                case engine::selection::WorldContextTargetAction::Ground:
                    projection.worldCursorHint = anySelected(
                        [&](engine::ObjectId actor) {
                            return worldCommands.actorCanMove(actor);
                        }) ? WorldCursorHint::Move : WorldCursorHint::Invalid;
                    break;
                case engine::selection::WorldContextTargetAction::Reserved:
                    projection.worldCursorHint = WorldCursorHint::Invalid;
                    break;
                }
                }
            } else {
                projection.worldCursorHint = anySelected(
                    [&](engine::ObjectId actor) {
                        return worldCommands.actorCanMove(actor);
                    }) ? WorldCursorHint::Move : WorldCursorHint::Normal;
            }
        }
        if (projection.hoveredObject) {
            const engine::selection::LocalSelectionObjectSnapshot hovered =
                session->localSelectionQuery().inspect(
                    engine::INVALID_PLAYER_ID, projection.hoveredObject);
            if (hovered.live) {
                projection.hoveredObjectDisplayNameLabel =
                    hovered.displayNameLabel;
            }
        }
        if (localPlayer && localPlayer->commandPlayer &&
            projection.pendingWorldCommand.active() &&
            projection.pendingWorldCommand.acceptsObject() &&
            projection.hoveredObject &&
            session->runtimeQuery().isLiveObject(
                projection.hoveredObject)) {
            projection.pendingHoveredObjectValid =
                engine::selection::pendingWorldTargetRelationAllowed(
                    projection.pendingWorldCommand, *session,
                    localPlayer->id, projection.hoveredObject);
        }
        const engine::GameSessionPresentationSnapshot presentation =
            session->presentationPort().snapshot();
        projection.confirmedTick = session->confirmedTick();
        projection.presentationEpoch = presentation.scriptEpoch;
        projection.audioPresentationEpoch = presentation.audioEpoch;
        const engine::session_query::SessionUiContentProjection uiContent =
            session->playerUiQuery().content();
        projection.mappedImageContentLayers = uiContent.mappedImageLayers;
        projection.mapStringContentLayer = uiContent.mapStrings;
        projection.gameplayInputEnabled = presentation.gameplayInputEnabled;
        const engine::RenderVisualDescriptor& visual =
            presentation.renderSettings->visual;
        projection.cameraInputSettings = visual.camera;
        projection.cameraInputSettings.viewportHeightScale = 1.0f;
        projection.inputSettings = visual.input;
        projection.powerBarSettings = visual.controlBarPower;
        projection.cameraState = presentation.camera.sanitized();
        projection.hasCameraState = true;
        projection.cameraPlayableMinimum =
            presentation.cameraPlayableMinimum;
        projection.cameraPlayableMaximum =
            presentation.cameraPlayableMaximum;
        projection.hasCameraPlayableExtent =
            presentation.hasCameraPlayableExtent;
        projection.cameraManualInputAllowed =
            presentation.cameraManualInputAllowed;
        projection.cameraScriptMovementActive =
            presentation.cameraScriptMovementActive;
        projection.cameraVisualSpeedMultiplier =
            presentation.visualSpeedMultiplier;
        projection.scriptCameraCommands =
            presentation.scriptCameraCommands;
        projection.scriptCameraCommandsTrimmedThroughSequence =
            presentation.scriptCameraCommandsTrimmedThroughSequence;
        projection.scriptCameraMovementRevision =
            presentation.scriptCameraMovementRevision;
        projection.tacticalViewportHeightScale = 1.0f;
        const engine::selection::LocalPlacementPreviewSnapshot& placement =
            presentation.placement;
        projection.localPlacementActive = presentation.localPlacementActive;
        projection.localPlacementPreviewIdentity =
            placement.previewIdentity;
        projection.localPlacement = placement;
        projection.localConstructionRouteNodes =
            gameLogic.localConstructionRouteNodes();
        projection.selectedLocalConstructionRouteNode =
            gameLogic.selectedLocalConstructionRouteNode();
        if (localPlayer && localPlayer->commandPlayer) {
            const engine::session_query::InGameCommandQuerySource source =
                engine::session_query::inGameCommandQuerySource(*session);
            for (engine::ObjectId object :
                 source.ownedObjects(localPlayer->id)) {
                auto builder = source.objectBuilderConstruction(object);
                if (builder.isBuilder) {
                    const size_t localRouteCount =
                        gameLogic.localConstructionRouteCount(object);
                    builder.queuedBuildCount = static_cast<uint16_t>(
                        std::min<size_t>(
                            std::numeric_limits<uint16_t>::max(),
                            std::max<size_t>(builder.queuedBuildCount,
                                             localRouteCount)));
                    projection.localBuilders.push_back(builder);
                }
            }
        }
        projection.scriptFrameRateLimit = presentation.frameRateLimit;
        projection.localFastForwardActive =
            presentation.localFastForwardActive;
        projection.scriptUi = m_scriptUiPublisher.build(
            *session, projection.sessionRevision);
        projection.commandUi = m_commandUiPublisher.build(
            *session, gameLogic.localSelection());
        if (projection.selectedLocalConstructionRouteNode) {
            const auto& route =
                *projection.selectedLocalConstructionRouteNode;
            auto& command = projection.commandUi;
            command.hasSelection = true;
            command.multiSelection = false;
            command.selectedCount = 1;
            command.selectedObject = route.builder;
            command.selectedObjectType = route.objectType;
            command.selectedPortraitImage.clear();
            command.upgradeCameos = {};
            command.selectedUnderConstruction = false;
            command.constructionProgressPermille = 0;
            command.selectedOrderWaypoint = false;
            command.hasCommandSet = false;
            command.slots = {};
            command.availability = {};
            command.actionTokens = {};
            command.inventorySlots = {};
            command.inventoryPassengers = {};
            command.actionActors = {};
            command.productionQueue = {};
            const engine::session_query::InGameCommandQuerySource source =
                engine::session_query::inGameCommandQuerySource(*session);
            const game::CommandButtonTemplate* cancel =
                source.findCommandButton("Command_CancelConstruction");
            if (cancel && cancel->descriptor.kind ==
                    game::CommandButtonKind::DozerConstructCancel) {
                command.hasCommandSet = true;
                command.slots[0] = {
                    .visible = true,
                    .commandButtonName = cancel->name,
                    .buttonImage = cancel->buttonImage,
                    .textLabel = cancel->textLabel,
                    .descriptionLabel = cancel->descriptionLabel,
                    .borderType = cancel->borderType,
                };
                command.availability[0] = {
                    .visible = true,
                    .enabled = true,
                    .reason = engine::session_query::
                        InGameCommandAvailabilityReason::None,
                    .revision = projection.revision,
                };
            }
        }
        projection.sciencePurchase =
            session->playerUiQuery().sciencePurchase(projection.revision);
        projection.specialPowerShortcuts =
            session->playerUiQuery().specialPowerShortcuts(
                projection.revision);
        projection.power = session->playerUiQuery().power();
        projection.money = session->playerUiQuery().money();
        if (projection.commandUi.hasSelection) {
            projection.hasCommandBarSelection = true;
            projection.selectedCommandBarObject =
                projection.commandUi.selectedObject;
            projection.selectedCommandBarObjectType =
                projection.commandUi.selectedObjectType;
        }
    }
    else {
        m_scriptUiPublisher.reset();
    }

    projection.matchResultRevision = gameLogic.matchResultRevision();
    if (projection.matchResultRevision != m_cachedMatchResultRevision) {
        const auto& result = gameLogic.matchResult();
        m_cachedMatchResult = result
            ? std::make_shared<const engine::MatchResultSnapshot>(*result)
            : nullptr;
        m_cachedMatchResultRevision = projection.matchResultRevision;
    }
    projection.matchResult = m_cachedMatchResult;
    projection.canResultNext =
        gameLogic.canResultAction(engine::GameResultAction::Next);
    projection.canResultRetry =
        gameLogic.canResultAction(engine::GameResultAction::Retry);
    return projection;
}

} // namespace app::runtime
