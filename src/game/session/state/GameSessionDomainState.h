#pragma once

#include "core/container/container_types.h"
#include "core/container/hash_containers.h"
#include "core/ecs/registry.h"

#include "game/base/GameCameraDirector.h"
#include "game/base/FrameCommitResult.h"
#include "game/base/GameSettings.h"
#include "game/base/SimulationRandom.h"
#include "game/ai/StrategicAIRuntime.h"
#include "game/audio/GameAudioEvents.h"
#include "game/command/CommandOutcome.h"
#include "game/command/CommandBarOverrides.h"
#include "game/content/loader/GameDataRegistry.h"
#include "game/fx/runtime/GameFxEventStream.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/object/ai/runtime/ObjectAIPathSequenceSnapshot.h"
#include "game/object/ai/runtime/ObjectAIRuntime.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/economy/ObjectEnergy.h"
#include "game/object/simulation/runtime/ObjectHealthEvents.h"
#include "game/object/simulation/runtime/ObjectMovementEvents.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/combat/ObjectProjectileSystem.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/MatchSetup.h"
#include "game/player/PlayerList.h"
#include "game/render/ClientTerrainObjectStore.h"
#include "presentation/render/RenderGameDataSettings.h"
#include "game/data/presentation/TrackMarksRenderDescriptor.h"
#include "game/scenario/runtime/MissionState.h"
#include "game/scenario/runtime/ScenarioDefinition.h"
#include "game/script/bridge/ScriptObjectIndex.h"
#include "game/script/bridge/ScriptSessionEvents.h"
#include "game/session/state/GameSessionScriptCameraState.h"
#include "game/session/presentation/GameSessionAudioJournal.h"
#include "game/script/legacy/LegacyMapScriptLoader.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/script/presentation/ScriptClientOptionsControls.h"
#include "game/script/presentation/ScriptEnvironmentPresentationControls.h"
#include "game/script/presentation/ScriptLightingPresentationControls.h"
#include "game/script/presentation/ScriptMapPresentationControls.h"
#include "game/script/presentation/ScriptObjectPresentationControls.h"
#include "game/script/presentation/ScriptPresentationCompletionLedger.h"
#include "game/data/presentation/ScriptSkyboxPresentationSettings.h"
#include "game/data/presentation/ScriptTerrainRoadPresentationSettings.h"
#include "game/script/presentation/ScriptUiPresentationControls.h"
#include "game/script/presentation/ScriptViewCompatibilityControls.h"
#include "game/data/presentation/ScriptWaterPresentationSettings.h"
#include "game/script/runtime/ScriptGameplayEventLedger.h"
#include "game/script/runtime/ScriptRuntime.h"
#include "game/render/LocalPlacementPresentationState.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/terrain/TerrainLogic.h"

#include "game/content/runtime/GameContentSnapshot.h"
#include "game/session/ai/GameSessionAISnapshot.h"
#include "game/session/script/GameSessionScriptContracts.h"
#include "game/session/lifecycle/MapObjectImport.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/object/contracts/ObjectTeamRegistry.h"

#include <cstdint>
#include <optional>

namespace engine {

namespace script {
class GameSessionScriptAuthorityPort;
class GameSessionScriptPresentationPort;
class GameSessionScriptQueryPort;
class GameSessionScriptConditionEventCursor;
}
namespace session_query {
class PlayerUiQueryPort;
}
namespace selection {
class LocalSelectionCommandBarPresentationConsumer;
}

class GameSession;
class GameSessionMapImportPort;
class GameSessionScriptUiPort;
class LocalPlacementPresentationPort;
class GameSessionGameplayPublicationPort;
class GameSessionAIDomain;
class GameSessionContainmentPlanTransactions;
class GameSessionCommandQueryPort;
class GameSessionConfirmedCommandPort;
class GameSessionBuildPlacementEvaluator;
class GameSessionRenderExtractionPort;
class GameSessionScenarioBootstrapService;
class GameSessionStrategicAIService;
class GameSessionWorldMaintenanceService;
class GameSessionGameplayEventCollector;
class GameSessionDynamicGeometryEventPublisher;
class GameSessionEvaEventPublisher;
class GameSessionObjectEventPublisher;
class GameSessionDebrisPresentationPublisher;
class GameSessionWeaponEventPublisher;
class GameSessionObjectAmbientAudioLifecycle;
class GameSessionDeletePostambleTransactions;
class GameSessionObjectDeathFeedbackPublisher;
class GameSessionHealthEventPublisher;
class GameSessionAIAttackOrderTransactions;
class GameSessionAINavigationFrameTransactions;
class GameSessionAIInsertionTransactions;
class GameSessionAIMoveOrderTransactions;
class GameSessionAIShadowTransactions;
class GameSessionAISpecialCommandTransactions;
class GameSessionAIOrderPolicy;
class GameSessionAIResolutionTransactions;
class GameSessionNavigationPathAdapter;
class GameSessionProductionPolicyPort;
class GameSessionPlayerOrderTransactions;
class GameSessionPlayerRepairTransactions;
class GameSessionObjectDamageTransactions;
class GameSessionObjectLifecycleTransactions;
class GameSessionMultiplayerVictoryTransactions;
class GameSessionNavigationTransactions;
class GameSessionProjectileSpawnTransactions;
class GameSessionBridgeLifecycleTransactions;
class GameSessionObjectOwnershipTransactions;
class GameSessionObjectProductionTransactions;
class GameSessionCountermeasureTransactions;
class GameSessionObjectProgressionTransactions;
class GameSessionScriptOrderAdmissionTransactions;
class GameSessionScriptScenarioPlanTransactions;
struct MatchResultSnapshot;

struct ObjectAmbientAudioPresentationState final {
    container::String eventName;
    ObjectBodyDamageState damageState = ObjectBodyDamageState::Pristine;
    uint64_t generation = 0;
    bool enabled = true;
    bool automaticEnabled = false;
};

// The economy runtime remains the deterministic owner.  Presentation retains
// just enough of its confirmed state to turn phase entries and successful cash
// commits into one-shot authored sounds without making audio state part of the
// simulation snapshot.
struct ObjectHackInternetAudioPresentationState final {
    ObjectHackInternetRuntimePhase phase =
        ObjectHackInternetRuntimePhase::Idle;
    uint64_t revision = 0;
};

struct ObjectLossRadarPresentationEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    uint64_t confirmedTick = 0;
};

// Radar::tryEvent keeps the last ten seconds of matching stealth feedback
// even though the visible triangle lives for only four seconds. This is
// observer-local presentation history and never enters simulation state.
struct ObjectStealthRadarFeedbackHistoryEvent final {
    LogicFixedVec3 position{};
    int32_t eventType = 0;
    uint64_t confirmedTick = 0;
};

// One deferred EVA announcement decided inside an authoritative transaction
// that owns no publication port. The decision - which announcement, and for
// which observer - is still made where RefCode makes it; only the audio
// emission waits for this frame's EVA publication pass.
struct PendingEvaAnnouncement final {
    audio::EvaEventType type = audio::EvaEventType::Count;
    uint64_t confirmedTick = 0;
    uint64_t variationKey = 0;
};

// Drawable::flashAsSelected is a separate envelope from script colour flashes.
// Keep its one-shot generation observer-local so sabotage feedback cannot
// replace an authored SCRIPT_FLASH_NAMED/TEAM presentation.
struct ObjectSelectionFlashPresentationEvent final {
    uint64_t identity = 0;
    uint64_t startTick = 0;
    uint64_t expireTick = 0;
};

// Confirmed object feedback is retained independently from diagnostic event
// history. Both source systems copy their world anchor at admission, matching
// InGameUI::addWorldAnimation/addFloatingText: later movement or destruction
// of the gameplay object cannot drag or erase an already-created effect.
struct ObjectWorldAnimationPresentationEvent final {
    uint64_t identity = 0;
    ObjectId object = INVALID_OBJECT_ID;
    math::vec3 worldAnchor{};
    container::String animationName;
    uint64_t startTick = 0;
    uint64_t expireTick = 0;
    uint32_t logicFramesPerSecond = 30;
    float zRisePerSecond = 0.0f;
};

struct ObjectFloatingTextPresentationEvent final {
    uint64_t identity = 0;
    ObjectId object = INVALID_OBJECT_ID;
    math::vec3 worldAnchor{};
    int64_t amount = 0;
    uint32_t color = 0xffffffffu;
    uint64_t startTick = 0;
    uint64_t timeoutTick = 0;
    uint64_t expireTick = 0;
    uint32_t logicFramesPerSecond = 30;
    float moveUpPerSecond = 30.0f;
    float vanishPerSecond = 3.0f;
};

// Durable counterpart of the ZH priority BuildList node created by selected
// AI/script building opcodes. It owns only stable values: no ECS entity,
// builder pointer, placement preview, or production handle survives a tick.
enum class GameSessionPriorityBuildState : uint8_t {
    Unbuilt,
    Reserved,
    Constructing,
    Completed,
    RebuildDelay,
    Exhausted,
};

struct GameSessionPriorityBuildEntry final {
    PlayerId player = INVALID_PLAYER_ID;
    container::String objectType;
    math::q32_32 anchorX{};
    math::q32_32 anchorY{};
    math::q32_32 yawRadians{};
    container::String scriptName;
    uint32_t sourceSideOrdinal = UINT32_MAX;
    uint32_t sourceBuildListOrdinal = UINT32_MAX;
    uint32_t sourceSequence = 0;
    uint64_t createdTick = 0;
    uint64_t nextAttemptTick = 0;
    uint32_t attemptCount = 0;
    uint32_t placementSearchOrdinal = 0;
    GameSessionPriorityBuildState state =
        GameSessionPriorityBuildState::Unbuilt;
    ObjectId reservedBuilder = INVALID_OBJECT_ID;
    ObjectId constructedObject = INVALID_OBJECT_ID;
    int32_t remainingRebuilds = 0;
    uint64_t strategicPlanId = 0;
    bool authoredBuildList = false;
};

namespace render {
struct TerrainRenderSnapshot;
}

namespace detail {
class GameSessionDomainComposition;
class GameSessionWeaponEventDrain;
class GameSessionGameplayTransactionDrain;
}

class GameRenderExtraction;
class MultiplayerRuleset;

class GameSessionFrameCommitState {
private:
    friend class GameSessionLifecycleTransactionPort;
    friend class GameSessionScriptFrameTransactions;
    friend class GameSessionFramePort;
    friend class GameSession;
    friend class GameSessionGameplayPublicationPort;
    friend class GameSessionMapImportPort;

    FrameCommitResult m_result;
    uint32_t m_pendingDegradationMask = 0;
    uint32_t m_pendingDegradationCount = 0;
    SimulationFault m_pendingFault;
    uint32_t m_pendingAdditionalFaultCount = 0;
    bool m_open = false;
};

// Launch/content ownership is intentionally separate from mutable world and
// presentation state. A replacement match resets this aggregate as a unit.
class GameSessionContentStartState {
private:
    friend class GameSessionLifecycleCascadeTransactions;
    friend class GameSessionNavigationFootprintTransactions;
    friend class GameSessionLifecycleTransactionPort;
    friend class GameSessionScenarioTransactionPort;
    friend class GameSessionOrderAdmissionPolicyPort;
    friend class GameSessionScriptFrameTransactions;
    friend class GameSessionFramePort;
    friend class GameSessionPresentationPort;
    friend class GameSessionGameplayPublicationPort;
    friend class GameSession;
    friend class GameSessionMapImportPort;
    friend class LocalPlacementPresentationPort;
    friend class GameSessionRenderExtractionPort;
    friend class GameSessionMediaPresentationPort;
    friend class GameSessionObjectFeedbackPublisher;
    friend class GameSessionDebrisPresentationPublisher;
    friend class GameSessionWeaponEventPublisher;
    friend class GameSessionConfirmedPresentationUpdater;
    friend class GameSessionScenarioBootstrapService;
    friend class GameSessionStrategicAIService;
    friend class GameSessionClientTerrainPresentationUpdater;
    friend class GameSessionWorldMaintenanceService;
    friend class GameSessionGameplayEventCollector;
    friend class GameSessionEvaEventPublisher;
    friend class GameSessionObjectEventPublisher;
    friend class GameSessionDebrisPresentationPublisher;
    friend class GameSessionWeaponEventPublisher;
    friend class GameSessionObjectAmbientAudioLifecycle;
    friend class GameSessionDeletePostambleTransactions;
    friend class GameSessionBridgeLifecycleTransactions;
    friend class GameSessionObjectDeathFeedbackPublisher;
    friend class GameSessionHealthEventPublisher;
    friend class GameSessionAIAttackOrderTransactions;
    friend class GameSessionAINavigationFrameTransactions;
    friend class GameSessionAIInsertionTransactions;
    friend class GameSessionAIMoveOrderTransactions;
    friend class GameSessionAIShadowTransactions;
    friend class GameSessionAISpecialCommandTransactions;
    friend class GameSessionAIOrderPolicy;
    friend class GameSessionAIResolutionTransactions;
    friend class GameSessionNavigationPathAdapter;
    friend class GameSessionProductionPolicyPort;
    friend class GameSessionPlayerOrderTransactions;
    friend class GameSessionPlayerRepairTransactions;
    friend class GameRenderExtraction;
    friend class GameSessionConfirmedCommandPort;
    friend class GameSessionBuildPlacementEvaluator;
    friend class GameSessionAIDomain;
    friend class GameSessionContainmentPlanTransactions;
    friend class GameSessionObjectDamageTransactions;
    friend class GameSessionObjectLifecycleTransactions;
    friend class GameSessionMultiplayerVictoryTransactions;
    friend class GameSessionNavigationTransactions;
    friend class GameSessionProjectileSpawnTransactions;
    friend class GameSessionBridgeLifecycleTransactions;
    friend class GameSessionObjectOwnershipTransactions;
    friend class GameSessionObjectProductionTransactions;
    friend class GameSessionCountermeasureTransactions;
    friend class GameSessionObjectProgressionTransactions;
    friend class GameSessionObjectSaleTransactions;
    friend class GameSessionCommandQueryPort;
    friend class GameSessionScriptOrderAdmissionTransactions;
    friend class GameSessionScriptScenarioPlanTransactions;
    friend struct MatchResultSnapshot;
    friend class script::GameSessionScriptQueryPort;
    friend class script::GameSessionScriptConditionEventCursor;
    friend class script::GameSessionScriptAuthorityPort;
    friend class script::GameSessionScriptPresentationPort;
    friend class session_query::PlayerUiQueryPort;
    friend class selection::LocalSelectionCommandBarPresentationConsumer;
    friend class detail::GameSessionDomainComposition;
    friend class detail::GameSessionWeaponEventDrain;
    friend class detail::GameSessionGameplayTransactionDrain;

    bool m_active = false;
    bool m_drainingGameplayWork = false;
    detail::GameSessionWeaponEventDrain* m_gameplayDrain = nullptr;
    // Lifetime identity for payloads after producer admission order has been
    // resolved. This is not an ordering source and is never compared with
    // ObjectSimulation's owner-thread admission clock.
    uint64_t m_nextGameplayStorageOrdinal = 1;
    GameStartInfo m_startInfo;
    GameDataRegistry m_data;
    GameContentSnapshot m_contentSnapshot;
    container::SharedPtr<const MultiplayerRuleset> m_ruleset;
    std::optional<ResolvedMatchSetup> m_resolvedMatchSetup;
    SimulationRandom m_simulationRandom;
    PlayerList m_players;
    game::terrain::TerrainLogic m_terrain;
    navigation::NavigationSystem m_navigation;
    container::Vector<navigation::NavigationCellId>
        m_navigationFootprintScratch;
    container::Vector<uint8_t> m_placementReachablePortalScratch;
    container::Vector<uint32_t> m_placementPortalQueueScratch;
    ObjectSimulationRules m_objectSimulationRules;
};

// AI state remains concrete and allocation-stable. Domain ports call it
// directly; no virtual dispatch is introduced in confirmed-frame hot paths.
class GameSessionAIState {
private:
    friend class GameSessionLifecycleCascadeTransactions;
    friend class GameSessionLifecycleTransactionPort;
    friend class GameSessionScenarioTransactionPort;
    friend class GameSessionScriptFrameTransactions;
    friend class GameSession;
    friend class LocalPlacementPresentationPort;
    friend class GameSessionConfirmedCommandPort;
    friend class GameSessionBuildPlacementEvaluator;
    friend class GameSessionCommandQueryPort;
    friend class GameSessionScenarioBootstrapService;
    friend class GameSessionStrategicAIService;
    friend class GameSessionAIAttackOrderTransactions;
    friend class GameSessionAINavigationFrameTransactions;
    friend class GameSessionAIInsertionTransactions;
    friend class GameSessionAIMoveOrderTransactions;
    friend class GameSessionAIShadowTransactions;
    friend class GameSessionAISpecialCommandTransactions;
    friend class GameSessionAIOrderPolicy;
    friend class GameSessionAIResolutionTransactions;
    friend class GameSessionNavigationPathAdapter;
    friend class GameSessionProductionPolicyPort;
    friend class GameSessionHealthEventPublisher;
    friend class GameSessionAIDomain;
    friend class GameSessionContainmentPlanTransactions;
    friend class GameSessionObjectOwnershipTransactions;
    friend class GameSessionObjectProgressionTransactions;
    friend class GameSessionObjectSaleTransactions;
    friend class GameSessionPlayerOrderTransactions;
    friend class GameSessionPlayerRepairTransactions;
    friend class GameSessionScriptOrderAdmissionTransactions;
    friend class GameSessionScriptScenarioPlanTransactions;
    friend class script::GameSessionScriptQueryPort;
    friend class script::GameSessionScriptAuthorityPort;
    friend class detail::GameSessionDomainComposition;
    friend class detail::GameSessionWeaponEventDrain;
    friend class detail::GameSessionGameplayTransactionDrain;

    ai::ObjectAIRuntime m_objectAI;
    StrategicAIRuntime m_strategicAI;
    ai::ObjectAIPathSequenceSnapshot m_objectAIPathSequences;
    container::Vector<ai::ObjectAIReadOnlyFact> m_objectAIShadowFacts;
    container::Vector<ai::ObjectAIReadOnlyFact> m_objectAIShadowNextFacts;
    // Value-owned phase inputs.  Combat can synchronously close gameplay
    // transactions while it iterates, so it must never borrow ObjectAI's
    // mutable admission lanes or transient command buffers.
    ai::ObjectAIOrderCapabilitySnapshot m_objectAIOrderCapabilitySnapshot;
    // Command transactions can run outside the fixed Combat/Movement phases.
    // Keep their scratch projections independent: a nested script/player
    // order must never overwrite the value-owned input of an active phase.
    ai::ObjectAIOrderCapabilitySnapshot m_playerOrderCapabilitySnapshot;
    ai::ObjectAIOrderCapabilitySnapshot m_scriptOrderCapabilitySnapshot;
    container::Vector<ai::AIAttackCommand> m_objectAIAttackCommandSnapshot;
    container::Vector<ObjectAIMovementCommand> m_objectAIMovementCommands;
    container::Vector<ai::PathCorrelation> m_objectAIMoveCompletions;
    container::Vector<ai::MovementFeedback> m_movementFeedbackDrainScratch;
    container::Vector<ai::AIFacingFeedback> m_facingFeedbackDrainScratch;
    container::Vector<GameSessionPriorityBuildEntry> m_priorityBuildEntries;
};

// Script-facing visual state is grouped separately from authoritative object
// event journals. It is still session-owned and reset at match boundaries.
class GameSessionScriptPresentationState {
private:
    friend class GameSessionBridgeLifecycleTransactions;
    friend class GameSessionLifecycleCascadeTransactions;
    friend class GameSessionNavigationFootprintTransactions;
    friend class GameSessionLifecycleTransactionPort;
    friend class GameSessionScenarioTransactionPort;
    friend class GameSessionOrderAdmissionPolicyPort;
    friend class GameSessionScriptFrameTransactions;
    friend class GameSessionFramePort;
    friend class GameSessionPresentationPort;
    friend class GameSessionConfirmedCommandPort;
    friend class GameSessionBuildPlacementEvaluator;
    friend class GameSession;
    friend class GameSessionMapImportPort;
    friend class GameSessionScriptUiPort;
    friend class LocalPlacementPresentationPort;
    friend class GameSessionGameplayPublicationPort;
    friend class GameSessionRenderExtractionPort;
    friend class GameSessionMediaPresentationPort;
    friend class GameSessionObjectFeedbackPublisher;
    friend class GameSessionConfirmedPresentationUpdater;
    friend class GameSessionPendingEvacuationTransactions;
    friend class GameSessionScenarioBootstrapService;
    friend class GameSessionStrategicAIService;
    friend class GameSessionGameplayEventCollector;
    friend class GameSessionObjectEventPublisher;
    friend class GameSessionDebrisPresentationPublisher;
    friend class GameSessionObjectAmbientAudioLifecycle;
    friend class GameSessionDeletePostambleTransactions;
    friend class GameSessionObjectDeathFeedbackPublisher;
    friend class GameSessionEvaEventPublisher;
    friend class GameSessionHealthEventPublisher;
    friend class GameSessionAIAttackOrderTransactions;
    friend class GameSessionAINavigationFrameTransactions;
    friend class GameSessionAIInsertionTransactions;
    friend class GameSessionAIMoveOrderTransactions;
    friend class GameSessionAIShadowTransactions;
    friend class GameSessionAISpecialCommandTransactions;
    friend class GameSessionAIOrderPolicy;
    friend class GameSessionAIResolutionTransactions;
    friend class GameSessionNavigationPathAdapter;
    friend class GameSessionProductionPolicyPort;
    friend class GameSessionPlayerOrderTransactions;
    friend class GameSessionPlayerRepairTransactions;
    friend class GameRenderExtraction;
    friend class GameSessionAIDomain;
    friend class GameSessionContainmentPlanTransactions;
    friend class GameSessionObjectDamageTransactions;
    friend class GameSessionObjectLifecycleTransactions;
    friend class GameSessionMultiplayerVictoryTransactions;
    friend class GameSessionNavigationTransactions;
    friend class GameSessionObjectOwnershipTransactions;
    friend class GameSessionObjectProductionTransactions;
    friend class GameSessionCountermeasureTransactions;
    friend class GameSessionObjectProgressionTransactions;
    friend class GameSessionObjectSaleTransactions;
    friend class GameSessionScriptOrderAdmissionTransactions;
    friend class GameSessionScriptScenarioPlanTransactions;
    friend class script::GameSessionScriptQueryPort;
    friend class script::GameSessionScriptConditionEventCursor;
    friend class script::GameSessionScriptAuthorityPort;
    friend class script::GameSessionScriptPresentationPort;
    friend class detail::GameSessionDomainComposition;
    friend class detail::GameSessionWeaponEventDrain;

    static constexpr int32_t kDefaultScriptRankLevelLimit = 1000;

    script::ScriptObjectIndex m_scriptObjects;
    scenario::MissionState m_missionState;
    ScriptMultiplayerVictoryState m_scriptMultiplayerVictory;
    script::ScriptRuntime m_scriptRuntime;
    script::ScriptGameplayEventLedger m_scriptGameplayEvents;
    // objectSeesAny() is a serialized, read-only script query. Its complete
    // ordered result is consumed before another query may clear this buffer;
    // match startup/shutdown releases the retained capacity.
    mutable container::Vector<ObjectId> m_scriptSightQueryScratch;
    container::Vector<ScriptOrderExecutionRecord> m_scriptOrderExecutionRecords;
    container::SharedPtr<const script::legacy::LegacyMapScriptSource> m_legacyMapScriptSource;
    script::legacy::LegacyMapScriptLoadReport m_legacyMapScriptLoadReport;
    container::SharedPtr<const scenario::ScenarioDefinition> m_scenarioDefinition;
    GameCameraDirector m_cameraDirector;
    GameCameraInput m_pendingCameraInput;
    selection::LocalPlacementPresentationState m_localPlacementPresentation;
    container::Vector<selection::LocalPlacementPreviewSnapshot>
        m_queuedConstructionPlacements;
    container::Vector<selection::TimedLocalPlacementPreview>
        m_rejectedConstructionPlacements;

    GameSessionScriptCameraState m_scriptCamera;
    bool m_scriptTimeFrozen = false;
    GameSessionAudioJournal m_audioJournal;
    uint64_t m_scriptPresentationEpoch = 0;
    uint64_t m_confirmedTick = 0;
    bool m_hasConfirmedFrame = false;
    uint64_t m_objectFeedbackOrdinal = 0;
    container::Vector<ObjectWorldAnimationPresentationEvent>
        m_objectWorldAnimations;
    container::Vector<ObjectFloatingTextPresentationEvent>
        m_objectFloatingTexts;
    container::Vector<CommandBackendOutcome> m_commandBackendOutcomes;
    container::HashMap<ObjectId, ObjectAmbientAudioPresentationState>
        m_objectAmbientAudio;
    container::HashMap<
        ObjectId, container::Vector<ObjectHackInternetAudioPresentationState>>
        m_hackInternetAudio;

    // UnitLost creates RADAR_EVENT_FAKE: invisible on the radar but retained
    // for VIEW_LAST_RADAR_EVENT. Admission keeps the original 10-second/
    // 250-world-unit dedup history; extraction exposes a four-second event.
    container::Vector<ObjectLossRadarPresentationEvent>
        m_objectLossRadarEvents;
    // Renderer-local beacon pulse retention is presentation state, not part
    // of the script adapter object's identity. It resets with the same epoch
    // as every other detached presentation journal.
    container::Vector<ObjectBeaconClientEvent> m_renderBeaconRadarHistory;
    uint64_t m_renderBeaconRadarEpoch = 0;
    container::Vector<ObjectStealthRadarFeedbackHistoryEvent>
        m_stealthRadarFeedbackHistory;
    // RefCode Radar::tryEvent keeps one 64-entry ring of every radar event and
    // rejects a new one of the same type within 250 world units / 10 seconds of
    // a live entry. Under-attack warnings additionally suppress map-wide, so a
    // cargo plane taking fire across the map cannot re-announce. This history is
    // observer-local: it is written only for the observed player and never read
    // by simulation.
    container::Vector<ObjectStealthRadarFeedbackHistoryEvent>
        m_underAttackRadarFeedbackHistory;
    // Next confirmed tick at which the polled EVA conditions may be offered
    // again. RefCode Eva::update re-polls a predicate only while no EvaCheck
    // for that message is outstanding, and an unplayed check is discarded after
    // ExpirationTimeMS; this mirrors that retry cadence so a sustained outage
    // announces on the authored interval rather than every frame. The authored
    // TimeBetweenChecksMS is still enforced downstream by the audio scheduler.
    container::Array<uint64_t, audio::kEvaEventTypeCount>
        m_evaPolledRetryTicks{};
    // Last observed general's rank for the observing player. Zero means not
    // yet sampled, so a loaded save or a mid-match observer change adopts the
    // current rank silently instead of announcing a promotion.
    int32_t m_evaObservedRankLevel = 0;
    container::Vector<PendingEvaAnnouncement> m_pendingEvaAnnouncements;
    container::HashMap<ObjectId, ObjectSelectionFlashPresentationEvent>
        m_objectSelectionFlashes;
    game::FxInvocationEventStream m_fxInvocations;
    container::Vector<script::ScriptSessionEvent> m_scriptSessionEvents;
    uint64_t m_scriptPresentationSequence = 0;
    script::ScriptLetterboxPresentationState m_scriptLetterboxPresentation;
    container::Vector<script::ScriptScreenShakeImpulse> m_scriptScreenShakeJournal;
    uint64_t m_scriptScreenShakeJournalTrimmedThroughSequence = 0;
    container::Vector<script::ScriptLocalizedCameraShakeImpulse> m_scriptLocalizedCameraShakeJournal;
    uint64_t m_scriptLocalizedCameraShakeJournalTrimmedThroughSequence = 0;
    container::Vector<script::ScriptMoveCameraToSelectionPresentation>
        m_scriptMoveCameraToSelectionRequests;
    container::Vector<script::ScriptCameraPresentationCommand>
        m_scriptCameraPresentationJournal;
    uint64_t m_scriptCameraPresentationJournalTrimmedThroughSequence = 0;
    uint64_t m_scriptCameraMovementRevision = 0;
    uint64_t m_scriptCameraCompletedRevision = 0;
    script::ScriptCameraSlavePresentationState m_scriptCameraSlavePresentation;
    container::Vector<script::ScriptForceObjectSelectionPresentation>
        m_scriptForceObjectSelectionRequests;
    script::ScriptScreenFadePresentationState m_scriptScreenFadePresentation;
    script::ScriptBlackAndWhitePresentationState m_scriptBlackAndWhitePresentation;
    container::Vector<script::ScriptBlackAndWhitePresentationState> m_scriptBlackAndWhiteJournal;
    uint64_t m_scriptBlackAndWhiteJournalTrimmedThroughSequence = 0;
    script::ScriptMotionBlurPresentationState m_scriptMotionBlurPresentation;
    container::Vector<script::ScriptMotionBlurPresentationState> m_scriptMotionBlurJournal;
    uint64_t m_scriptMotionBlurJournalTrimmedThroughSequence = 0;
    script::ScriptSkyboxPresentationState m_scriptSkyboxPresentation;
    script::ScriptSkyboxTextureSet m_scriptSkyboxPresentationTextures;
    script::ScriptWaterPresentationSettings m_scriptWaterPresentationSettings;
    script::ScriptTerrainRoadPresentationSettings m_scriptTerrainRoadPresentationSettings;
    TrackMarksPresentationSettings m_trackMarksPresentationSettings;
    RenderGameDataSettings m_renderGameDataSettings;
    container::SharedPtr<const RenderGameDataSettings> m_renderGameDataSettingsSnapshot;
    container::SharedPtr<const ResolvedRenderFeatureSnapshot> m_renderFeatureQualitySnapshot;
    ResolvedRenderDisplaySnapshot m_initialRenderDisplaySnapshot;
    script::ScriptTreeSwayPresentationState m_scriptTreeSwayPresentation;
    script::ScriptWeatherPresentationState m_scriptWeatherPresentation;
    script::ScriptInfantryLightingPresentationState m_scriptInfantryLightingPresentation;
    script::ScriptUiPresentationState m_scriptUiPresentation;
    game::CommandBarOverrideState m_scriptCommandBarOverrides;
    container::TreeMap<container::String, game::ObjectBuildabilityStatus>
        m_scriptObjectBuildabilityOverrides;

    struct ScriptAttackPriorityRule final {
        script::ScriptAttackPriorityMutationKind mutation =
            script::ScriptAttackPriorityMutationKind::ObjectType;
        container::String selector;
        game::ObjectKindOf selectorKind = game::ObjectKindOf::Count;
        int32_t priority = 1;
        uint64_t sequence = 0;
    };
    struct ScriptAttackPrioritySetRuntime final {
        uint32_t id = 0;
        int32_t defaultPriority = 1;
        uint64_t revision = 1;
        container::Vector<ScriptAttackPriorityRule> rules;
    };
    container::TreeMap<container::String, ScriptAttackPrioritySetRuntime>
        m_scriptAttackPrioritySets;
    container::Vector<ScriptAttackPrioritySetRuntime*> m_scriptAttackPriorityById;
    uint64_t m_scriptAttackPrioritySequence = 0;
    bool m_objectsReceiveDifficultyBonuses = true;
    bool m_chooseVictimAlwaysNormal = false;
    container::TreeMap<container::String, LogicFixedVec3> m_scriptToppleDirections;
    int32_t m_scriptRankLevelLimit = kDefaultScriptRankLevelLimit;
    bool m_scoreAccumulationEnabled = true;
    script::ScriptClientOptionsState m_scriptClientOptions;
    script::ScriptMapPresentationState m_scriptMapPresentation;
    script::ScriptObjectPresentationState m_scriptObjectPresentation;
    script::ScriptViewCompatibilityState m_scriptViewCompatibility;
    container::Vector<script::ScriptPresentationCompletion>
        m_pendingScriptPresentationCompletions;
    struct PendingVisualAnimationCompletion final {
        ObjectId object = INVALID_OBJECT_ID;
        uint32_t channelIndex = 0;
        uint64_t generation = 0;
        uint8_t phase = 0;
        uint8_t kind = 0;
        uint64_t simulationFrame = 0;
        float completedDurationSeconds = 0.0f;
    };
    struct PendingVisualAnimationAdmission final {
        ObjectId object = INVALID_OBJECT_ID;
        uint32_t channelIndex = 0;
        uint64_t generation = 0;
        uint64_t simulationFrame = 0;
    };
    // Packed ObjectId/channel -> latest renderer-published generation.
    // ObjectId is a uint32 session identity, so this key is collision-free.
    container::TreeMap<uint64_t, PendingVisualAnimationAdmission>
        m_pendingVisualAnimationAdmissions;
    container::Vector<PendingVisualAnimationCompletion>
        m_pendingVisualAnimationCompletions;
    container::Vector<container::String> m_pendingScriptMusicLoops;
    script::ScriptPresentationCompletionLedger m_scriptPresentationCompletions;
};

// Object and presentation edges are intentionally grouped by producer domain,
// rather than hidden in a catch-all Requests/Events bucket.
class GameSessionObjectEventState {
private:
    friend class GameSessionObjectLifecycleTransactions;
    friend class GameSessionLifecycleCascadeTransactions;
    friend class GameSessionLifecycleTransactionPort;
    friend class GameSessionScriptFrameTransactions;
    friend class GameSessionFramePort;
    friend class GameSession;
    friend class GameSessionRenderExtractionPort;
    friend class GameSessionScenarioBootstrapService;
    friend class GameSessionDebrisPresentationPublisher;
    friend class GameSessionDeletePostambleTransactions;
    friend class GameSessionHealthEventPublisher;
    friend class GameSessionAIDomain;
    friend class script::GameSessionScriptConditionEventCursor;
    friend class detail::GameSessionDomainComposition;
    friend class detail::GameSessionWeaponEventDrain;
    friend class GameRenderExtraction;

    struct UpgradeRadarPresentationEvent final {
        ObjectId producer = INVALID_OBJECT_ID;
        PlayerId player = INVALID_PLAYER_ID;
        math::vec3 position{};
        uint64_t confirmedTick = 0;
        uint32_t sourceSequence = 0;
    };

    container::Vector<UpgradeRadarPresentationEvent> m_upgradeRadarHistory;
    uint64_t m_upgradeRadarEpoch = 0;

    container::Vector<ObjectRadiusDecalEvent> m_projectileRadiusDecalEvents;
    // Gameplay consumes lifecycle/health edges synchronously while they are
    // still owned by the confirmed-frame transaction. Presentation observers
    // share these non-destructive, frame-local journals.
    container::Vector<ObjectLifecycleEvent> m_frameLifecycleEvents;
    container::Vector<ObjectHealthEvent> m_frameHealthEvents;
    // Synchronous producer drain storage. It is cleared after publication but
    // retains capacity across repeated damage reactions.
    // consumeObjectHealthEvents() is intentionally non-reentrant: the damage
    // publication depth gate must keep an inner drain from clearing an outer
    // iteration. Match startup/shutdown releases the retained high water.
    container::Vector<ObjectHealthEvent> m_healthDrainScratch;
    container::Vector<script::ScriptWorldTeamUnitDestroyedEvent>
        m_teamUnitDestroyedHookEvents;
    container::Vector<script::ScriptWorldObjectHookEvent> m_objectHookEvents;

};

struct GameSessionPathObjectSnapshotCacheEntry final {
    uint64_t confirmedTick = 0;
    uint64_t spatialRevision = 0;
    navigation::NavigationLayerId layer;
    PlayerId subjectOwner = INVALID_PLAYER_ID;
    int64_t subjectRadiusRaw = 0;
    uint8_t crusherLevel = 0;
    bool unmanned = false;
    container::Vector<ai::AIPathObjectCellSnapshot> cells;
};

class GameSessionWorldState {
private:
    friend class GameSessionCountermeasureTransactions;
    friend class GameSessionGameplayPublicationPort;
    friend class GameSessionAIOrderPolicy;
    friend class GameSessionLifecycleCascadeTransactions;
    friend class GameSessionNavigationFootprintTransactions;
    friend class GameSessionLifecycleTransactionPort;
    friend class GameSessionScenarioTransactionPort;
    friend class GameSessionOrderAdmissionPolicyPort;
    friend class GameSessionScriptFrameTransactions;
    friend class GameSessionFramePort;
    friend class GameSessionPresentationPort;
    friend class GameSession;
    friend class GameSessionMapImportPort;
    friend class LocalPlacementPresentationPort;
    friend class GameSessionRenderExtractionPort;
    friend class GameSessionMediaPresentationPort;
    friend class GameSessionObjectFeedbackPublisher;
    friend class GameSessionConfirmedPresentationUpdater;
    friend class GameSessionClientTerrainPresentationUpdater;
    friend class GameSessionPendingEvacuationTransactions;
    friend class GameSessionScenarioBootstrapService;
    friend class GameSessionWorldMaintenanceService;
    friend class GameSessionStrategicAIService;
    friend class GameSessionPlayerOrderTransactions;
    friend class GameSessionPlayerRepairTransactions;
    friend class GameSessionGameplayEventCollector;
    friend class GameSessionDynamicGeometryEventPublisher;
    friend class GameSessionObjectEventPublisher;
    friend class GameSessionDebrisPresentationPublisher;
    friend class GameSessionWeaponEventPublisher;
    friend class GameSessionObjectAmbientAudioLifecycle;
    friend class GameSessionDeletePostambleTransactions;
    friend class GameSessionAIAttackOrderTransactions;
    friend class GameSessionAIMoveOrderTransactions;
    friend class GameSessionAIShadowTransactions;
    friend class GameSessionAIInsertionTransactions;
    friend class GameSessionAINavigationFrameTransactions;
    friend class GameSessionAISpecialCommandTransactions;
    friend class GameSessionAIResolutionTransactions;
    friend class GameSessionNavigationPathAdapter;
    friend class GameSessionHealthEventPublisher;
    friend class GameSessionProjectileSpawnTransactions;
    friend class GameSessionBridgeLifecycleTransactions;
    friend class GameRenderExtraction;
    friend class selection::LocalSelectionCommandBarPresentationConsumer;
    friend class GameSessionConfirmedCommandPort;
    friend class GameSessionCommandQueryPort;
    friend class GameSessionBuildPlacementEvaluator;
    friend class GameSessionAIDomain;
    friend class GameSessionContainmentPlanTransactions;
    friend class GameSessionObjectDamageTransactions;
    friend class GameSessionObjectLifecycleTransactions;
    friend class GameSessionMultiplayerVictoryTransactions;
    friend class GameSessionObjectOwnershipTransactions;
    friend class GameSessionObjectTargetRemapTransactions;
    friend class GameSessionObjectProductionTransactions;
    friend class GameSessionObjectProgressionTransactions;
    friend class GameSessionObjectSaleTransactions;
    friend class GameSessionScriptOrderAdmissionTransactions;
    friend class GameSessionScriptScenarioPlanTransactions;
    friend class script::GameSessionScriptQueryPort;
    friend class script::GameSessionScriptAuthorityPort;
    friend class script::GameSessionScriptPresentationPort;
    friend class detail::GameSessionDomainComposition;
    friend class detail::GameSessionWeaponEventDrain;

    game::terrain::MapVisibilityAuthority m_mapVisibility;
    bool m_sessionShroudEnabled = true;
    uint32_t m_visibilityUnlookPersistenceTicks = 0;
    uint32_t m_visibilityFogTransitionTicks = 0;
    MapObjectSpawnReport m_mapObjectSpawnReport;
    ClientTerrainObjectStore m_clientTerrainObjects;
    ecs::registry m_registry;
    ObjectLifecycle m_objects{m_registry};
    ObjectOwnershipIndex m_ownership;
    ObjectTeamRegistry m_objectTeams;
    ObjectCombatSystem m_objectCombat;
    ObjectProjectileSystem m_objectProjectiles;
    ObjectEnergySystem m_objectEnergy;
    ObjectProductionSystem m_objectProduction;
    container::Vector<ObjectProductionSpawnIntent>
        m_pendingProductionSpawns;
    container::Vector<ObjectProductionUpgradeCompletionIntent>
        m_pendingProductionUpgrades;
    ObjectSimulation m_objectSimulation;
    ObjectModelConditionAuthorityState m_modelConditionAuthority;
    uint32_t m_initialContainmentSpawnDepth = 0;
    ObjectSpatialIndex m_spatialIndex;
    // Request-admission cache only. It is rebuilt from confirmed ECS state on
    // the first matching request of a tick and is never part of simulation
    // snapshots/digests; every retained cell remains copied into PathRequest.
    container::Vector<GameSessionPathObjectSnapshotCacheEntry>
        m_pathObjectSnapshotCache;
};

// Session storage is composed rather than inherited. The behavior hierarchy
// can retain its stable public API while each exact behavior class receives
// access only to the state partitions that explicitly name it as a friend.
class GameSessionStateRoot final {
private:
    friend class GameSession;
    friend class GameSessionRenderExtractionPort;
    friend class GameSessionConfirmedCommandPort;
    friend class GameSessionAIDomain;
    friend struct MatchResultSnapshot;
    friend class GameRenderExtraction;
    friend class selection::LocalSelectionCommandBarPresentationConsumer;
    friend class detail::GameSessionDomainComposition;
    friend class detail::GameSessionWeaponEventDrain;
    friend class detail::GameSessionGameplayTransactionDrain;

    GameSessionContentStartState& contentState() noexcept { return m_content; }
    const GameSessionContentStartState& contentState() const noexcept {
        return m_content;
    }
    GameSessionAIState& aiState() noexcept { return m_ai; }
    const GameSessionAIState& aiState() const noexcept { return m_ai; }
    GameSessionScriptPresentationState& presentationState() noexcept {
        return m_presentation;
    }
    const GameSessionScriptPresentationState& presentationState() const noexcept {
        return m_presentation;
    }
    GameSessionObjectEventState& objectEventState() noexcept { return m_objectEvents; }
    const GameSessionObjectEventState& objectEventState() const noexcept {
        return m_objectEvents;
    }
    GameSessionWorldState& worldState() noexcept { return m_world; }
    const GameSessionWorldState& worldState() const noexcept { return m_world; }
    GameSessionFrameCommitState& frameState() noexcept { return m_frame; }
    const GameSessionFrameCommitState& frameState() const noexcept {
        return m_frame;
    }

    GameSessionContentStartState m_content;
    GameSessionAIState m_ai;
    GameSessionScriptPresentationState m_presentation;
    GameSessionObjectEventState m_objectEvents;
    GameSessionWorldState m_world;
    GameSessionFrameCommitState m_frame;
};

} // namespace engine
