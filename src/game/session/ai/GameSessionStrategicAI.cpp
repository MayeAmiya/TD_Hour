#include "game/session/ai/GameSessionStrategicAIService.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/command/OrderExecutor.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/definition/ObjectArchetype.h"

#include "core/container/string_utils.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/economy/ObjectEconomyDetail.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/lifecycle/ObjectCreate.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/structure/ObjectSupplyWarehouseCrippling.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/session/transaction/GameSessionPlayerStateTransactions.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] bool hasKindOf(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf expected) noexcept {
    return kinds && game::objectHasKind(kinds->mask, expected);
}

[[nodiscard]] bool matchesStrategicBuildEntry(
    const GameSessionPriorityBuildEntry& entry,
    const StrategicAIBuildPlan& plan) noexcept {
    if (entry.strategicPlanId != 0 || plan.role !=
            StrategicAIBuildRole::Authored) {
        return entry.strategicPlanId == plan.id;
    }
    return entry.sourceSideOrdinal == plan.sourceSideOrdinal &&
        entry.sourceBuildListOrdinal == plan.sourceBuildListOrdinal;
}

[[nodiscard]] bool isLiveStrategicObject(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    ecs::entity entity, ObjectId object) noexcept {
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    const ObjectMapStatusComponent* mapStatus =
        ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    return lifecycle && lifecycle->phase == ObjectLifecyclePhase::Alive &&
        !objects.isPendingDestroy(object) &&
        (!health || !health->effectivelyDead) &&
        (!mapStatus || !mapStatus->offMap) &&
        (!status || !status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Sold)));
}

[[nodiscard]] uint64_t stableBuildPlanId(
    uint32_t sideOrdinal, uint32_t buildOrdinal) noexcept {
    return (static_cast<uint64_t>(sideOrdinal) + 1u) << 32u |
        (static_cast<uint64_t>(buildOrdinal) + 1u);
}

[[nodiscard]] int32_t rebuildCount(
    const scenario::ScenarioBuildIntent& intent) noexcept {
    for (const scenario::RawScenarioField& field : intent.fields) {
        if (field.key != "rebuildCount") continue;
        int32_t parsed = 0;
        const char* begin = field.value.data();
        const char* end = begin + field.value.size();
        const auto result = std::from_chars(begin, end, parsed);
        return result.ec == std::errc{} && result.ptr == end ? parsed : 0;
    }
    return 0;
}

[[nodiscard]] uint32_t fixedSecondsToTicks(
    math::q32_32 seconds, uint32_t framesPerSecond) noexcept {
    if (seconds <= math::q32_32{}) return 1;
    const uint32_t boundedFps = std::min<uint32_t>(
        framesPerSecond,
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    const math::q32_32 ticks = seconds *
        math::q32_32{static_cast<int32_t>(boundedFps)};
    if (ticks.raw() <= 0) return 1;
    const uint64_t raw = static_cast<uint64_t>(ticks.raw());
    const uint64_t whole = raw >> 32u;
    const uint64_t rounded = whole + ((raw & 0xffffffffull) != 0 ? 1u : 0u);
    return static_cast<uint32_t>(std::min<uint64_t>(
        std::max<uint64_t>(1, rounded),
        std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] int64_t strategicTeamStartCash(
    int64_t estimatedCost, math::q32_32 fraction) noexcept {
    if (estimatedCost <= 0 || fraction <= math::q32_32{}) return 0;
    const int32_t bounded = static_cast<int32_t>(std::min<int64_t>(
        estimatedCost, std::numeric_limits<int32_t>::max()));
    const math::q32_32 required = math::q32_32{bounded} * fraction;
    if (required.raw() <= 0) return 0;
    // RefCode stores team cost in Int and applies TeamResourcesToStart with
    // compound assignment, so positive fractional currency is truncated.
    return required.raw() >> 32u;
}

struct StrategicAIBuildRotation final {
    math::q32_32 cosine{int32_t{1}};
    math::q32_32 sine{};
};

// AISkirmishPlayer::adjustBuildList divides the playable map into a 3x3
// grid, adds the corresponding eighth-turn to the build-list's legacy 135
// degree basis, and rotates every authored offset around the build-list
// command center.  Keep the table fixed-only: this is authoritative placement
// data and must not depend on a platform sin/cos implementation.
[[nodiscard]] StrategicAIBuildRotation skirmishBuildRotation(
    math::q32_32 baseX, math::q32_32 baseY,
    const game::terrain::TerrainExtentRaw& extent) noexcept {
    const math::q32_32 minimumX =
        math::q32_32::from_raw(extent.minimumX);
    const math::q32_32 minimumY =
        math::q32_32::from_raw(extent.minimumY);
    const math::q32_32 width =
        math::q32_32::from_raw(extent.maximumX) - minimumX;
    const math::q32_32 height =
        math::q32_32::from_raw(extent.maximumY) - minimumY;
    const math::q32_32 oneThird = math::q32_32::from_fraction(1, 3);

    uint32_t gridIndex = 0;
    if (baseX > minimumX + width * oneThird) ++gridIndex;
    if (baseX > minimumX + width * oneThird * math::q32_32{int32_t{2}})
        ++gridIndex;
    if (baseY > minimumY + height * oneThird) gridIndex += 3;
    if (baseY > minimumY + height * oneThird * math::q32_32{int32_t{2}})
        gridIndex += 3;

    // Eighth-turns after RefCode's unconditional +3*PI/4 basis adjustment.
    static constexpr uint8_t kTurns[9] = {3, 4, 5, 2, 3, 6, 1, 0, 7};
    constexpr int64_t kSqrtHalfRaw = 3037000499ll;
    const math::q32_32 one{int32_t{1}};
    const math::q32_32 diagonal =
        math::q32_32::from_raw(kSqrtHalfRaw);
    switch (kTurns[gridIndex]) {
    case 0: return {.cosine = one};
    case 1: return {.cosine = diagonal, .sine = diagonal};
    case 2: return {.sine = one};
    case 3: return {.cosine = -diagonal, .sine = diagonal};
    case 4: return {.cosine = -one};
    case 5: return {.cosine = -diagonal, .sine = -diagonal};
    case 6: return {.sine = -one};
    case 7: return {.cosine = diagonal, .sine = -diagonal};
    }
    return {};
}

} // namespace

bool GameSessionStrategicAIService::initialize() {
    const GameMode strategicMode =
        m_content.m_startInfo.mode == GameMode::Replay &&
            m_content.m_resolvedMatchSetup
        ? m_content.m_resolvedMatchSetup->mode
        : m_content.m_startInfo.mode;
    const AISimulationRules& aiRules =
        m_content.m_objectSimulationRules.ai;
    container::Vector<StrategicAIPlayerDescriptor> descriptors;
    descriptors.reserve(m_content.m_players.playerCount());
    for (const PlayerId player :
         m_content.m_players.activePlayerIds()) {
        const PlayerState* state = m_content.m_players.get(player);
        if (!state || state->controller != PlayerControllerKind::Ai ||
            !state->isSimulationParticipant()) {
            continue;
        }
        const bool autonomous = strategicMode == GameMode::Skirmish ||
            strategicMode == GameMode::Challenge ||
            aiRules.forceSkirmishAI;
        descriptors.push_back({
            .player = player,
            .difficulty = state->aiDifficulty == AiDifficulty::None
                ? AiDifficulty::Normal : state->aiDifficulty,
            .autonomousSkirmish = autonomous,
        });
    }

    const uint32_t framesPerSecond = static_cast<uint32_t>(
        std::max(1, m_content.m_startInfo.gameSpeedFPS));
    if (!m_ai.m_strategicAI.initialize(
            {
                .logicFramesPerSecond = framesPerSecond,
                .economyIntervalTicks = std::max<uint32_t>(1, framesPerSecond / 2),
                .structureIntervalTicks = fixedSecondsToTicks(
                    aiRules.structureSeconds, framesPerSecond),
                .productionIntervalTicks = fixedSecondsToTicks(
                    aiRules.teamSeconds, framesPerSecond),
                .tacticalIntervalTicks = framesPerSecond * 2u,
                .enemyReviewIntervalTicks = framesPerSecond * 5u,
                .rebuildDelayTicks = fixedSecondsToTicks(
                    aiRules.rebuildDelayTimeSeconds, framesPerSecond),
                .wealthy = aiRules.wealthy,
                .poor = aiRules.poor,
                .structuresWealthyRate = aiRules.structuresWealthyRate,
                .structuresPoorRate = aiRules.structuresPoorRate,
                .teamsWealthyRate = aiRules.teamsWealthyRate,
                .teamsPoorRate = aiRules.teamsPoorRate,
                .teamResourcesToStart = aiRules.teamResourcesToStart,
                .baseDefenseExtraDistance =
                    aiRules.skirmishBaseDefenseExtraDistance,
            }, descriptors)) {
        return false;
    }

    // Scenario BuildList is immutable source data. Freeze it into the
    // player-level planner now; the runtime will only emit ordinary build
    // requests after the confirmed-frame gate opens.
    if (m_presentation.m_scenarioDefinition) {
        for (const scenario::ScenarioBuildIntent& intent :
             m_presentation.m_scenarioDefinition->buildIntents()) {
            if (!m_ai.m_strategicAI.findBrain(intent.resolvedOwner) ||
                intent.templateName.empty() ||
                !intent.fixedPoseValid) {
                continue;
            }
            StrategicAIBuildPlan plan{
                .id = stableBuildPlanId(
                    intent.sourceSideOrdinal,
                    intent.sourceBuildListOrdinal),
                .player = intent.resolvedOwner,
                .objectType = intent.templateName,
                .anchorX = intent.x,
                .anchorY = intent.y,
                .yawRadians = intent.angle,
                .scriptName = intent.structureName,
                .sourceSideOrdinal = intent.sourceSideOrdinal,
                .sourceBuildListOrdinal = intent.sourceBuildListOrdinal,
                .state = StrategicAIBuildState::Unbuilt,
                .remainingRebuilds = rebuildCount(intent),
            };
            static_cast<void>(m_ai.m_strategicAI.addBuildPlan(
                std::move(plan)));
        }
    }
    return true;
}

void GameSessionStrategicAIService::update() {
    if (!m_content.m_active ||
        !m_presentation.m_hasConfirmedFrame) {
        return;
    }
    const uint64_t tick = m_presentation.m_confirmedTick;
    m_scenarioPlans.updatePendingScenarioTeamReinforcements();
    // Mirror the construction authority's exact lifecycle result into the
    // pure planner before it makes another decision. Source ordinals are the
    // stable BuildList identity; no ECS pointer crosses this boundary.
    for (const StrategicAIBuildPlan& planView :
         m_ai.m_strategicAI.buildPlans()) {
        const auto found = std::find_if(
            m_ai.m_priorityBuildEntries.begin(),
            m_ai.m_priorityBuildEntries.end(),
            [&planView](const GameSessionPriorityBuildEntry& entry) noexcept {
                return matchesStrategicBuildEntry(entry, planView);
            });
        if (found == m_ai.m_priorityBuildEntries.end())
            continue;
        const bool present =
            (found->state == GameSessionPriorityBuildState::Constructing ||
             found->state == GameSessionPriorityBuildState::Completed) &&
            static_cast<bool>(found->constructedObject);
        const bool underConstruction =
            found->state == GameSessionPriorityBuildState::Constructing;
        if ((planView.state == StrategicAIBuildState::Unbuilt ||
             planView.state == StrategicAIBuildState::Reserved) && present) {
            static_cast<void>(m_ai.m_strategicAI
                .acknowledgeBuildAdmission(
                    planView.id, true, found->reservedBuilder,
                    found->constructedObject, tick, false));
        } else if (planView.state == StrategicAIBuildState::Reserved &&
                   found->state ==
                       GameSessionPriorityBuildState::Exhausted) {
            static_cast<void>(m_ai.m_strategicAI
                .acknowledgeBuildAdmission(
                    planView.id, false, INVALID_OBJECT_ID,
                    INVALID_OBJECT_ID,
                    std::numeric_limits<uint64_t>::max(), true));
        } else if (planView.state == StrategicAIBuildState::Constructing ||
                   planView.state == StrategicAIBuildState::Completed) {
            static_cast<void>(m_ai.m_strategicAI
                .observeBuildObject(
                    planView.id, present, present, underConstruction, tick));
        }
    }
    container::Vector<StrategicAIPlayerSnapshot> snapshots;
    snapshots.reserve(m_content.m_players.playerCount());

    for (const PlayerId player :
         m_content.m_players.activePlayerIds()) {
        const StrategicAIPlayerBrain* brain =
            m_ai.m_strategicAI.findBrain(player);
        const PlayerState* playerState =
            m_content.m_players.get(player);
        if (!brain || !playerState) continue;

        StrategicAIPlayerSnapshot snapshot{
            .player = player,
            .cash = playerState->cash,
            .energyProduction = playerState->energy.production,
            .energyConsumption = playerState->energy.consumption,
        };
        const AISkirmishBuildListRule* authoredBuildList =
            m_content.m_objectSimulationRules.ai.skirmishBuildList(
                playerState->side);
        const AISkirmishBuildStructureRule* authoredTemplateAnchor = nullptr;
        if (authoredBuildList) {
            for (const AISkirmishBuildStructureRule& structure :
                 authoredBuildList->structures) {
                const container::SharedPtr<const game::ObjectArchetype>
                    archetype = m_content.m_contentSnapshot
                        .findObjectArchetype(structure.objectType);
                if (archetype && game::objectHasKind(
                        archetype->kindOfMask,
                        game::ObjectKindOf::CommandCenter)) {
                    authoredTemplateAnchor = &structure;
                    break;
                }
            }
            if (!authoredTemplateAnchor &&
                !authoredBuildList->structures.empty()) {
                authoredTemplateAnchor =
                    &authoredBuildList->structures.front();
            }
        }
        const uint32_t authoredSideOrdinal =
            0x80000000u | snapshot.player.value;
        if (const AISideInfoRule* side =
                m_content.m_objectSimulationRules.ai.sideInfo(
                    playerState->side)) {
            const size_t difficultyIndex = brain->difficulty ==
                    AiDifficulty::Easy
                ? 0u
                : brain->difficulty == AiDifficulty::Hard ? 2u : 1u;
            snapshot.desiredGatherersPerCenter =
                side->resourceGatherers[difficultyIndex];
            snapshot.preferredBaseDefenseStructure =
                side->baseDefenseStructure;
            const ScienceCatalog* sciences =
                m_content.m_contentSnapshot.scienceCatalog();
            int32_t selectedSkillset =
                playerState->progress.selectedSkillset;
            if (playerState->sciences.purchasePoints > 0 &&
                selectedSkillset < 0) {
                selectedSkillset = 0;
                if (brain->autonomousSkirmish) {
                    size_t lastContiguous = 0;
                    for (size_t index = 1;
                         index < side->skillSets.size(); ++index) {
                        if (side->skillSets[index].empty()) break;
                        lastContiguous = index;
                    }
                    selectedSkillset =
                        m_content.m_simulationRandom.integerInclusive(
                            0, static_cast<int32_t>(lastContiguous));
                }
                static_cast<void>(m_content.m_players.setSelectedSkillset(
                    player, selectedSkillset));
            }
            const size_t skillSetIndex = selectedSkillset >= 0 &&
                    selectedSkillset <
                        static_cast<int32_t>(side->skillSets.size())
                ? static_cast<size_t>(selectedSkillset) : 0u;
            if (sciences && playerState->sciences.purchasePoints > 0) {
                for (const container::String& scienceName :
                     side->skillSets[skillSetIndex]) {
                    const ScienceDefinition* science =
                        sciences->find(scienceName);
                    if (science && m_content.m_players
                            .canPurchaseScience(player, *science)) {
                        snapshot.scienceOptions.push_back(science->name);
                    }
                }
            }
        }
        math::q32_32 baseSumX{};
        math::q32_32 baseSumY{};
        uint32_t baseAnchorCount = 0;
        math::q32_32 builderFallbackX{};
        math::q32_32 builderFallbackY{};
        bool hasBuilderFallback = false;
        math::q32_32 commandCenterX{};
        math::q32_32 commandCenterY{};
        bool hasCommandCenterAnchor = false;
        container::Vector<std::pair<math::q32_32, math::q32_32>>
            supplyCenterPositions;
        struct SupplyCenterServiceCandidate final {
            StrategicAISupplyCenterSnapshot service;
            math::q32_32 x{};
            math::q32_32 y{};
            math::q32_32 serviceRadius{};
        };
        container::Vector<SupplyCenterServiceCandidate>
            supplyCenterServices;
        struct GathererServiceCandidate final {
            ObjectId gatherer = INVALID_OBJECT_ID;
            ObjectId preferredDock = INVALID_OBJECT_ID;
            bool suppressed = false;
        };
        container::Vector<GathererServiceCandidate> gathererServices;
        container::Vector<std::pair<math::q32_32, math::q32_32>>
            ownedStructurePositions;
        const std::optional<ObjectTeamId> defaultTeam =
            m_world.m_objectTeams.defaultTeam(player);
        const container::Span<const ObjectId> owned =
            m_world.m_ownership.objects(player);
        for (const ObjectId object : owned) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(object);
            if (!entity || !isLiveStrategicObject(
                    m_world.m_registry,
                    m_world.m_objects, *entity, object)) {
                continue;
            }
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(
                    m_world.m_registry, *entity);
            const ObjectFixedTransformComponent* fixedTransform =
                ecs::try_get<ObjectFixedTransformComponent>(
                    m_world.m_registry, *entity);
            const bool hasAuthoritativePosition =
                fixedTransform && fixedTransform->authoritative;
            const bool structure =
                hasKindOf(kinds, game::ObjectKindOf::Structure);
            const bool defaultTeamMember = defaultTeam &&
                m_world.m_objectTeams.teamOf(object) ==
                    defaultTeam;
            if (structure) {
                ++snapshot.ownedStructureCount;
                if (hasAuthoritativePosition) {
                    baseSumX += fixedTransform->position.x;
                    baseSumY += fixedTransform->position.y;
                    ++baseAnchorCount;
                    ownedStructurePositions.emplace_back(
                        fixedTransform->position.x,
                        fixedTransform->position.y);
                }
                if (hasKindOf(kinds, game::ObjectKindOf::FsPower))
                    ++snapshot.powerPlantCount;
                if (hasKindOf(kinds, game::ObjectKindOf::CommandCenter)) {
                    ++snapshot.commandCenterCount;
                    if (!hasCommandCenterAnchor &&
                        hasAuthoritativePosition) {
                        commandCenterX = fixedTransform->position.x;
                        commandCenterY = fixedTransform->position.y;
                        hasCommandCenterAnchor = true;
                    }
                }
                if (hasKindOf(kinds, game::ObjectKindOf::FsSupplyCenter))
                {
                    ++snapshot.supplyCenterCount;
                    if (hasAuthoritativePosition) {
                        supplyCenterPositions.emplace_back(
                            fixedTransform->position.x,
                            fixedTransform->position.y);
                        const ObjectGeometryComponent* geometry =
                            ecs::try_get<ObjectGeometryComponent>(
                                m_world.m_registry, *entity);
                        const uint32_t desired =
                            snapshot.desiredGatherersPerCenter ==
                                    std::numeric_limits<uint32_t>::max()
                                ? std::numeric_limits<uint32_t>::max()
                                : snapshot.desiredGatherersPerCenter + 1u;
                        supplyCenterServices.push_back({
                            .service = {
                                .center = object,
                                .desiredGatherers = desired,
                            },
                            .x = fixedTransform->position.x,
                            .y = fixedTransform->position.y,
                            // RefCode SUPPLY_CENTER_CLOSE_DIST is twenty
                            // ten-unit pathfind cells plus the center radius.
                            .serviceRadius = math::q32_32{int32_t{200}} +
                                (geometry
                                    ? math::q32_32::max(
                                          math::q32_32{},
                                          geometry->boundingCircleRadiusFixed)
                                    : math::q32_32{}),
                        });
                    }
                }
                if (hasKindOf(kinds, game::ObjectKindOf::FsFactory) ||
                    hasKindOf(kinds, game::ObjectKindOf::FsBarracks) ||
                    hasKindOf(kinds, game::ObjectKindOf::FsWarfactory) ||
                    hasKindOf(kinds, game::ObjectKindOf::FsAirfield)) {
                    ++snapshot.productionFacilityCount;
                }
                if (hasKindOf(kinds, game::ObjectKindOf::FsBaseDefense))
                    ++snapshot.baseDefenseCount;
            } else {
                ++snapshot.ownedUnitCount;
            }
            if (hasKindOf(kinds, game::ObjectKindOf::Harvester)) {
                const ObjectEconomyComponent* gathererEconomy =
                    ecs::try_get<ObjectEconomyComponent>(
                        m_world.m_registry, *entity);
                if (gathererEconomy &&
                    !gathererEconomy->supplyTrucks.empty()) {
                    ++snapshot.harvesterCount;
                    const ObjectSupplyTruckRuntime& supply =
                        gathererEconomy->supplyTrucks.front();
                    gathererServices.push_back({
                        .gatherer = object,
                        .preferredDock = supply.preferredDock,
                        .suppressed = supply.scriptIdleSuppressed ||
                            supply.externalIdleSuppressed,
                    });
                }
            }
            const ObjectBuilderComponent* builder =
                    ecs::try_get<ObjectBuilderComponent>(
                        m_world.m_registry, *entity);
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(
                    m_world.m_registry, *entity);
            const bool hasBuilderModule =
                builder && !builder->runtimes.empty();
            const bool availableBuilder = hasBuilderModule &&
                !isObjectDisabledBy(
                    m_world.m_registry, *entity,
                    ObjectDisabledReason::Unmanned, tick) &&
                (!status || !status->hasAny(
                    game::objectStatusBit(
                        game::ObjectStatusFlag::UnderConstruction)));
            snapshot.hasBuilder =
                snapshot.hasBuilder || availableBuilder;
            const bool usableBuilder = availableBuilder &&
                !m_world.m_objectSimulation
                     .isAnyObjectBuilderTaskPending(
                         m_world.m_registry,
                         m_world.m_objects, object);
            if (usableBuilder) {
                snapshot.hasUsableBuilder = true;
                if (!hasBuilderFallback && hasAuthoritativePosition) {
                    builderFallbackX = fixedTransform->position.x;
                    builderFallbackY = fixedTransform->position.y;
                    hasBuilderFallback = true;
                }
            }

            const bool attackCapable =
                m_ai.m_objectAI.hasOrderCapability(
                    object, ai::ObjectAIOrderCapability::Attack);
            const ObjectOrderQueueComponent* orders =
                ecs::try_get<ObjectOrderQueueComponent>(
                    m_world.m_registry, *entity);
            // AISkirmishPlayer never commandeers a unit already owned by a
            // Scenario Team. The fallback autonomous team may recruit only
            // from the player's default Team; reassignment immediately drops
            // the object from its live/idle pool on the next confirmed tick.
            if (!structure && attackCapable && defaultTeamMember) {
                snapshot.liveCombatUnits.push_back(object);
                if (!orders || orders->orders.empty()) {
                    snapshot.idleCombatUnits.push_back(object);
                }
            }

            const ObjectProductionComponent* production =
                ecs::try_get<ObjectProductionComponent>(
                    m_world.m_registry, *entity);
            const bool productionReady = production &&
                (!status || !status->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::UnderConstruction)));
            const container::StringView commandSetName =
                effectiveObjectCommandSetName(
                    m_world.m_registry, *entity);
            const game::CommandSetTemplate* commandSet =
                m_content.m_contentSnapshot.findCommandSet(
                    commandSetName);
            if (production) {
                snapshot.liveProducers.push_back(object);
                for (const ObjectProductionJob& job : production->jobs) {
                    if (job.productionId != 0) {
                        snapshot.activeProductionHandles.push_back({
                            .producer = object,
                            .productionId = job.productionId,
                        });
                    }
                }
            }
            if (!commandSet) continue;
            for (const container::String& buttonName : commandSet->commands) {
                const game::CommandButtonTemplate* button =
                    m_content.m_contentSnapshot
                        .findCommandButton(buttonName);
                if (!button || !button->descriptor.requiredReferencesPresent)
                    continue;
                if (usableBuilder && button->descriptor.kind ==
                        game::CommandButtonKind::DozerConstruct &&
                    !button->object.empty()) {
                    const container::SharedPtr<const game::ObjectArchetype>
                        product = m_content.m_contentSnapshot
                            .findObjectArchetype(button->object);
                        if (!product || !game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::Structure) ||
                        !canObjectBuildTemplate(
                            m_world.m_registry, *entity,
                            m_content.m_contentSnapshot,
                            m_presentation.m_scriptCommandBarOverrides,
                            m_content.m_players, player,
                            *product)) {
                        continue;
                    }
                    StrategicAIStructureOption option{
                        .builder = object,
                        .productType = product->templateData.name,
                        .cost = calculateObjectBuildCost(
                            *product, *playerState,
                            m_world.m_registry,
                            m_world.m_objects),
                        .energyProduction =
                            game::objectHasKind(
                                product->kindOfMask,
                                game::ObjectKindOf::CashGenerator)
                            ? 0 : product->templateData.energyProduction,
                        .supplyCenter = game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::FsSupplyCenter),
                        .commandCenter = game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::CommandCenter),
                        .productionFacility = game::objectHasKind(
                                product->kindOfMask,
                                game::ObjectKindOf::FsFactory) ||
                            game::objectHasKind(
                                product->kindOfMask,
                                game::ObjectKindOf::FsBarracks) ||
                            game::objectHasKind(
                                product->kindOfMask,
                                game::ObjectKindOf::FsWarfactory) ||
                            game::objectHasKind(
                                product->kindOfMask,
                                game::ObjectKindOf::FsAirfield),
                        .baseDefense = game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::FsBaseDefense),
                    };
                    if (authoredBuildList && authoredTemplateAnchor) {
                        for (size_t authoredIndex = 0;
                             authoredIndex < authoredBuildList->structures.size();
                             ++authoredIndex) {
                            const AISkirmishBuildStructureRule& authored =
                                authoredBuildList->structures[authoredIndex];
                            if (!container::asciiEqualIgnoreCase(
                                    authored.objectType,
                                    option.productType)) {
                                continue;
                            }
                            const uint32_t authoredBuildOrdinal =
                                static_cast<uint32_t>(authoredIndex + 1u);
                            const bool alreadyClaimed = std::any_of(
                                m_ai.m_strategicAI.buildPlans().begin(),
                                m_ai.m_strategicAI.buildPlans().end(),
                                [authoredSideOrdinal,
                                 authoredBuildOrdinal](
                                    const StrategicAIBuildPlan& plan) {
                                    return plan.sourceSideOrdinal ==
                                            authoredSideOrdinal &&
                                        plan.sourceBuildListOrdinal ==
                                            authoredBuildOrdinal;
                                });
                            if (alreadyClaimed) continue;
                            option.hasAuthoredPlacement = true;
                            option.authoredOffsetX =
                                authored.x - authoredTemplateAnchor->x;
                            option.authoredOffsetY =
                                authored.y - authoredTemplateAnchor->y;
                            option.authoredYawRadians = authored.yawRadians;
                            option.authoredSideOrdinal = authoredSideOrdinal;
                            option.authoredBuildOrdinal =
                                authoredBuildOrdinal;
                            break;
                        }
                    }
                    snapshot.structureOptions.push_back(std::move(option));
                } else if (productionReady && button->descriptor.kind ==
                        game::CommandButtonKind::UnitBuild &&
                    !button->object.empty()) {
                    const container::SharedPtr<const game::ObjectArchetype>
                        product = m_content.m_contentSnapshot
                            .findObjectArchetype(button->object);
                    if (!product || !canObjectBuildTemplate(
                            m_world.m_registry, *entity,
                            m_content.m_contentSnapshot,
                            m_presentation.m_scriptCommandBarOverrides,
                            m_content.m_players, player,
                            *product)) {
                        continue;
                    }
                    const bool mobileCombatKind =
                        game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::Infantry) ||
                        game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::Vehicle) ||
                        game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::Aircraft);
                    const bool authoredAttackCapability =
                        game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::CanAttack) ||
                        game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::SpawnsAreTheWeapons) ||
                        !product->templateData.weapons.empty();
                    snapshot.productionOptions.push_back({
                        .producer = object,
                        .productType = product->templateData.name,
                        .cost = calculateObjectBuildCost(
                            *product, *playerState,
                            m_world.m_registry,
                            m_world.m_objects),
                        .queueDepth = static_cast<uint32_t>(
                            production->jobs.size()),
                        .harvester = game::objectHasKind(
                            product->kindOfMask,
                            game::ObjectKindOf::Harvester),
                        .builder = game::objectHasKind(
                            product->kindOfMask, game::ObjectKindOf::Dozer),
                        .combatUnit = !game::objectHasKind(
                                product->kindOfMask,
                                game::ObjectKindOf::Harvester) &&
                            !game::objectHasKind(
                                product->kindOfMask,
                                game::ObjectKindOf::Dozer) &&
                            mobileCombatKind && authoredAttackCapability,
                    });
                }
            }
        }
        std::sort(
            supplyCenterServices.begin(), supplyCenterServices.end(),
            [](const SupplyCenterServiceCandidate& left,
               const SupplyCenterServiceCandidate& right) noexcept {
                return left.service.center < right.service.center;
            });
        const auto supplySources = ecs::view<
            const ObjectIdentityComponent,
            const ObjectLifecycleComponent,
            const ObjectKindOfComponent,
            const ObjectFixedTransformComponent>(m_world.m_registry);
        for (SupplyCenterServiceCandidate& center :
             supplyCenterServices) {
            const math::q32_32 serviceRadiusSquared =
                center.serviceRadius * center.serviceRadius;
            for (const ecs::entity supplyEntity : supplySources) {
                const ObjectIdentityComponent& identity =
                    supplySources.template get<
                        const ObjectIdentityComponent>(supplyEntity);
                const ObjectLifecycleComponent& lifecycle =
                    supplySources.template get<
                        const ObjectLifecycleComponent>(supplyEntity);
                const ObjectKindOfComponent& supplyKinds =
                    supplySources.template get<
                        const ObjectKindOfComponent>(supplyEntity);
                if (lifecycle.phase != ObjectLifecyclePhase::Alive ||
                    !object_economy_detail::isAliveObject(
                        m_world.m_registry, m_world.m_objects,
                        supplyEntity, identity.id) ||
                    object_economy_detail::hasBlockingStatus(
                        m_world.m_registry, supplyEntity, tick) ||
                    !hasKindOf(&supplyKinds,
                               game::ObjectKindOf::SupplySource)) {
                    continue;
                }
                const ObjectSupplyAnchorComponent* supplyAnchor =
                    ecs::try_get<ObjectSupplyAnchorComponent>(
                        m_world.m_registry, supplyEntity);
                const ObjectDockCrippleComponent* supplyCripple =
                    ecs::try_get<ObjectDockCrippleComponent>(
                        m_world.m_registry, supplyEntity);
                if (!supplyAnchor || !supplyAnchor->supplyWarehouseReady ||
                    (supplyCripple && supplyCripple->crippled())) {
                    continue;
                }
                const OwnerComponent* supplyOwner =
                    ecs::try_get<OwnerComponent>(
                        m_world.m_registry, supplyEntity);
                if (supplyOwner && supplyOwner->player &&
                    m_content.m_players.relationship(
                        player, supplyOwner->player) ==
                        PlayerRelationship::Enemies) {
                    continue;
                }
                const ObjectEconomyComponent* supplyEconomy =
                    ecs::try_get<ObjectEconomyComponent>(
                        m_world.m_registry, supplyEntity);
                if (!supplyEconomy || !supplyEconomy->plan ||
                    supplyEconomy->supplyWarehouseDocks.empty() ||
                    std::none_of(
                        supplyEconomy->supplyWarehouseDocks.begin(),
                        supplyEconomy->supplyWarehouseDocks.end(),
                        [](const ObjectSupplyWarehouseDockRuntime& dock)
                            noexcept { return dock.boxesStored != 0; })) {
                    continue;
                }
                const ObjectFixedTransformComponent& supplyTransform =
                    supplySources.template get<
                        const ObjectFixedTransformComponent>(supplyEntity);
                if (!supplyTransform.authoritative) continue;
                const math::q32_32 dx =
                    supplyTransform.position.x - center.x;
                const math::q32_32 dy =
                    supplyTransform.position.y - center.y;
                if (dx * dx + dy * dy <= serviceRadiusSquared) {
                    center.service.hasViableSupply = true;
                    break;
                }
            }
        }
        for (const GathererServiceCandidate& gatherer : gathererServices) {
            const auto center = std::lower_bound(
                supplyCenterServices.begin(), supplyCenterServices.end(),
                gatherer.preferredDock,
                [](const SupplyCenterServiceCandidate& candidate,
                   ObjectId dock) noexcept {
                    return candidate.service.center < dock;
                });
            if (center != supplyCenterServices.end() &&
                center->service.center == gatherer.preferredDock &&
                center->service.hasViableSupply) {
                if (center->service.assignedGatherers !=
                    std::numeric_limits<uint32_t>::max()) {
                    ++center->service.assignedGatherers;
                }
            } else if (!gatherer.suppressed) {
                snapshot.looseGatherers.push_back(gatherer.gatherer);
            }
        }
        for (const SupplyCenterServiceCandidate& center :
             supplyCenterServices) {
            snapshot.supplyCenters.push_back(center.service);
            if (!center.service.hasViableSupply) continue;
            const uint32_t remaining =
                std::numeric_limits<uint32_t>::max() -
                    snapshot.desiredGathererCount;
            snapshot.desiredGathererCount += std::min(
                remaining, center.service.desiredGatherers);
        }
        std::sort(snapshot.looseGatherers.begin(),
                  snapshot.looseGatherers.end());
        std::sort(snapshot.liveCombatUnits.begin(),
                  snapshot.liveCombatUnits.end());
        std::sort(snapshot.idleCombatUnits.begin(),
                  snapshot.idleCombatUnits.end());
        std::sort(snapshot.liveProducers.begin(),
                  snapshot.liveProducers.end());
        std::sort(snapshot.activeProductionHandles.begin(),
                  snapshot.activeProductionHandles.end());
        snapshot.activeProductionHandles.erase(
            std::unique(snapshot.activeProductionHandles.begin(),
                        snapshot.activeProductionHandles.end()),
            snapshot.activeProductionHandles.end());
        std::sort(
            snapshot.structureOptions.begin(),
            snapshot.structureOptions.end(),
            [](const StrategicAIStructureOption& left,
               const StrategicAIStructureOption& right) noexcept {
                if (left.productType != right.productType)
                    return left.productType < right.productType;
                return left.builder < right.builder;
            });
        snapshot.structureOptions.erase(
            std::unique(
                snapshot.structureOptions.begin(),
                snapshot.structureOptions.end(),
                [](const StrategicAIStructureOption& left,
                   const StrategicAIStructureOption& right) noexcept {
                    return left.builder == right.builder &&
                        left.productType == right.productType;
                }),
            snapshot.structureOptions.end());
        if (hasCommandCenterAnchor) {
            snapshot.baseAnchorX = commandCenterX;
            snapshot.baseAnchorY = commandCenterY;
            snapshot.hasBaseAnchor = true;
        } else if (baseAnchorCount != 0) {
            snapshot.baseAnchorX = baseSumX /
                math::q32_32{static_cast<int32_t>(baseAnchorCount)};
            snapshot.baseAnchorY = baseSumY /
                math::q32_32{static_cast<int32_t>(baseAnchorCount)};
            snapshot.hasBaseAnchor = true;
        } else if (hasBuilderFallback) {
            snapshot.baseAnchorX = builderFallbackX;
            snapshot.baseAnchorY = builderFallbackY;
            snapshot.hasBaseAnchor = true;
        }
        if (snapshot.hasBaseAnchor) {
            math::q32_32 farthestSquared{};
            for (const auto& position : ownedStructurePositions) {
                const math::q32_32 dx =
                    position.first - snapshot.baseAnchorX;
                const math::q32_32 dy =
                    position.second - snapshot.baseAnchorY;
                farthestSquared = math::q32_32::max(
                    farthestSquared, dx * dx + dy * dy);
            }
            snapshot.baseRadius = math::q32_32::sqrt(farthestSquared);
        }
        if (snapshot.hasBaseAnchor && authoredBuildList &&
            authoredTemplateAnchor &&
            m_content.m_objectSimulationRules.ai.rotateSkirmishBases) {
            const StrategicAIBuildRotation rotation = skirmishBuildRotation(
                snapshot.baseAnchorX, snapshot.baseAnchorY,
                m_content.m_terrain.map().playableExtentRaw());
            for (StrategicAIStructureOption& option :
                 snapshot.structureOptions) {
                if (!option.hasAuthoredPlacement) continue;
                const math::q32_32 x = option.authoredOffsetX;
                const math::q32_32 y = option.authoredOffsetY;
                option.authoredOffsetX =
                    x * rotation.cosine - y * rotation.sine;
                option.authoredOffsetY =
                    y * rotation.cosine + x * rotation.sine;
                // RefCode deliberately keeps BuildListInfo::angle unchanged.
            }
        }
        snapshot.idleCombatUnitCount = static_cast<uint32_t>(
            snapshot.idleCombatUnits.size());

        if (snapshot.hasBaseAnchor) {
            bool foundSupply = false;
            math::q32_32 bestSupplyDistance{};
            ObjectId bestSupply = INVALID_OBJECT_ID;
            const auto supplies = ecs::view<
                const ObjectIdentityComponent,
                const ObjectLifecycleComponent,
                const ObjectKindOfComponent,
                const ObjectFixedTransformComponent>(
                    m_world.m_registry);
            for (const ecs::entity entity : supplies) {
                const ObjectLifecycleComponent& lifecycle =
                    supplies.template get<const ObjectLifecycleComponent>(
                        entity);
                const ObjectKindOfComponent& kinds =
                    supplies.template get<const ObjectKindOfComponent>(
                        entity);
                if (lifecycle.phase != ObjectLifecyclePhase::Alive ||
                    !hasKindOf(&kinds,
                               game::ObjectKindOf::SupplySource)) {
                    continue;
                }
                const OwnerComponent* supplyOwner =
                    ecs::try_get<OwnerComponent>(
                        m_world.m_registry, entity);
                if (supplyOwner && supplyOwner->player &&
                    m_content.m_players.relationship(
                        player, supplyOwner->player) ==
                        PlayerRelationship::Enemies) {
                    continue;
                }
                const ObjectEconomyComponent* economy =
                    ecs::try_get<ObjectEconomyComponent>(
                        m_world.m_registry, entity);
                if (economy && !economy->supplyWarehouseDocks.empty() &&
                    std::none_of(
                        economy->supplyWarehouseDocks.begin(),
                        economy->supplyWarehouseDocks.end(),
                        [](const ObjectSupplyWarehouseDockRuntime& dock)
                            noexcept { return dock.boxesStored != 0; })) {
                    continue;
                }
                const ObjectIdentityComponent& identity =
                    supplies.template get<const ObjectIdentityComponent>(
                        entity);
                const ObjectFixedTransformComponent& fixedTransform =
                    supplies.template get<const ObjectFixedTransformComponent>(
                        entity);
                if (!fixedTransform.authoritative) continue;
                const math::q32_32 x = fixedTransform.position.x;
                const math::q32_32 y = fixedTransform.position.y;
                const ObjectGeometryComponent* supplyGeometry =
                    ecs::try_get<ObjectGeometryComponent>(
                        m_world.m_registry, entity);
                const math::q32_32 safeRadius =
                    math::q32_32::max(
                        math::q32_32{},
                        m_content.m_objectSimulationRules.ai
                            .supplyCenterSafeRadius) +
                    (supplyGeometry
                        ? math::q32_32::max(
                              math::q32_32{},
                              supplyGeometry->boundingCircleRadiusFixed)
                        : math::q32_32{});
                const math::q32_32 safeRadiusSquared =
                    safeRadius * safeRadius;
                bool safe = true;
                for (const PlayerId enemy :
                     m_content.m_players.activePlayerIds()) {
                    if (m_content.m_players.relationship(player, enemy) !=
                        PlayerRelationship::Enemies) {
                        continue;
                    }
                    for (const ObjectId threat :
                         m_world.m_ownership.objects(enemy)) {
                        const std::optional<ecs::entity> threatEntity =
                            m_world.m_objects.entityFromId(threat);
                        if (!threatEntity || !isLiveStrategicObject(
                                m_world.m_registry, m_world.m_objects,
                                *threatEntity, threat)) {
                            continue;
                        }
                        const ObjectKindOfComponent* threatKinds =
                            ecs::try_get<ObjectKindOfComponent>(
                                m_world.m_registry, *threatEntity);
                        if (hasKindOf(threatKinds,
                                      game::ObjectKindOf::Harvester) ||
                            hasKindOf(threatKinds,
                                      game::ObjectKindOf::Dozer)) {
                            continue;
                        }
                        const ObjectStatusComponent* threatStatus =
                            ecs::try_get<ObjectStatusComponent>(
                                m_world.m_registry, *threatEntity);
                        if (threatStatus && threatStatus->hasAny(
                                game::objectStatusBit(
                                    game::ObjectStatusFlag::Stealthed)) &&
                            !threatStatus->hasAny(
                                game::objectStatusBit(
                                    game::ObjectStatusFlag::Detected) |
                                game::objectStatusBit(
                                    game::ObjectStatusFlag::Disguised))) {
                            continue;
                        }
                        const ObjectFixedTransformComponent* threatTransform =
                            ecs::try_get<ObjectFixedTransformComponent>(
                                m_world.m_registry, *threatEntity);
                        if (!threatTransform ||
                            !threatTransform->authoritative) {
                            continue;
                        }
                        const math::q32_32 threatDx =
                            threatTransform->position.x - x;
                        const math::q32_32 threatDy =
                            threatTransform->position.y - y;
                        if (threatDx * threatDx + threatDy * threatDy <=
                            safeRadiusSquared) {
                            safe = false;
                            break;
                        }
                    }
                    if (!safe) break;
                }
                if (!safe) continue;
                const math::q32_32 dx = x - snapshot.baseAnchorX;
                const math::q32_32 dy = y - snapshot.baseAnchorY;
                const math::q32_32 distance = dx * dx + dy * dy;
                const math::q32_32 servicedRadius{250};
                const math::q32_32 servicedRadiusSquared =
                    servicedRadius * servicedRadius;
                const bool alreadyServiced = std::any_of(
                    supplyCenterPositions.begin(),
                    supplyCenterPositions.end(),
                    [x, y, servicedRadiusSquared](const auto& center)
                        noexcept {
                        const math::q32_32 centerDx = center.first - x;
                        const math::q32_32 centerDy = center.second - y;
                        return centerDx * centerDx + centerDy * centerDy <=
                            servicedRadiusSquared;
                    });
                if (alreadyServiced) continue;
                if (!foundSupply || distance < bestSupplyDistance ||
                    (distance == bestSupplyDistance &&
                     identity.id < bestSupply)) {
                    foundSupply = true;
                    bestSupplyDistance = distance;
                    bestSupply = identity.id;
                    snapshot.supplyAnchorX = x;
                    snapshot.supplyAnchorY = y;
                }
            }
            snapshot.hasSupplyAnchor = foundSupply;
            snapshot.supplyExpansionNeeded = foundSupply &&
                snapshot.supplyCenterCount != 0;
        }

        const bool teamSelectionDue =
            m_ai.m_strategicAI.teamSelectionDue(player, tick);
        if (brain->autonomousSkirmish &&
            m_presentation.m_scenarioDefinition) {
            for (const scenario::ScriptTeamDefinition& definition :
                 m_presentation.m_scenarioDefinition->scriptTeams()) {
                if (definition.resolvedOwner != player ||
                    definition.isPlayerDefault) {
                    continue;
                }
                if (teamSelectionDue &&
                    m_ai.m_strategicAI.teamConditionEvaluationDue(
                        definition.id, tick)) {
                    const auto evaluated =
                        m_presentation.m_scriptRuntime
                            .evaluateNamedConditionForPlayer(
                                definition.plan.productionCondition,
                                player);
                    m_ai.m_strategicAI.observeTeamCondition(
                        definition.id,
                        evaluated && evaluated->difficultyAllowed
                            ? std::optional<bool>{evaluated->value}
                            : std::nullopt,
                        evaluated ? evaluated->evaluationDelayTicks : 0,
                        tick);
                }

                StrategicAITeamProductionOption option{
                    .definition = definition.id,
                    .name = definition.name,
                    .priority = definition.plan.productionPriority,
                    .maximumInstances = definition.maximumInstances,
                    .conditionSatisfied =
                        m_ai.m_strategicAI.teamConditionValue(definition.id),
                };
                const container::Span<const ObjectTeamId> instances =
                    m_world.m_objectTeams.scenarioTeamInstances(
                        definition.id);
                for (const ObjectTeamId instance : instances) {
                    const ObjectTeamRecord* record =
                        m_world.m_objectTeams.find(instance);
                    if (!record) continue;
                    if (option.instanceCount !=
                        std::numeric_limits<uint32_t>::max()) {
                        ++option.instanceCount;
                    }
                    option.assemblyInProgress =
                        option.assemblyInProgress ||
                        record->assemblyKind != ObjectTeamAssemblyKind::None;
                    option.priority = m_world.m_objectTeams
                        .productionPriority(instance)
                        .value_or(option.priority);
                }

                bool hasRoster = false;
                bool allRosterTypesHaveFactory = true;
                bool anyIdleFactory = false;
                int64_t estimatedCost = 0;
                for (const scenario::ScenarioTeamUnitPlan& unit :
                     definition.plan.units) {
                    if (unit.maximumUnits == 0) continue;
                    hasRoster = true;
                    const StrategicAIProductionOption* producer = nullptr;
                    for (const StrategicAIProductionOption& candidate :
                         snapshot.productionOptions) {
                        if (candidate.productType != unit.templateName) {
                            continue;
                        }
                        anyIdleFactory = anyIdleFactory ||
                            candidate.queueDepth == 0;
                        if (!producer || candidate.cost < producer->cost ||
                            (candidate.cost == producer->cost &&
                             candidate.producer < producer->producer)) {
                            producer = &candidate;
                        }
                    }
                    if (!producer) {
                        allRosterTypesHaveFactory = false;
                        continue;
                    }
                    const uint64_t countSum =
                        static_cast<uint64_t>(unit.minimumUnits) +
                        static_cast<uint64_t>(unit.maximumUnits);
                    const uint64_t unitCost = producer->cost > 0
                        ? static_cast<uint64_t>(producer->cost) : 0;
                    const uint64_t maximum = static_cast<uint64_t>(
                        std::numeric_limits<int64_t>::max());
                    const auto saturatedProduct = [maximum](
                        uint64_t left, uint64_t right) noexcept {
                        return left != 0 && right > maximum / left
                            ? maximum : left * right;
                    };
                    uint64_t contribution = 0;
                    if ((countSum & 1u) == 0) {
                        contribution = saturatedProduct(
                            unitCost, countSum / 2u);
                    } else if ((unitCost & 1u) == 0) {
                        contribution = saturatedProduct(
                            unitCost / 2u, countSum);
                    } else {
                        contribution = saturatedProduct(
                            unitCost / 2u, countSum);
                        const uint64_t remainder = countSum / 2u;
                        contribution = remainder > maximum - contribution
                            ? maximum : contribution + remainder;
                    }
                    estimatedCost = contribution >
                            static_cast<uint64_t>(
                                std::numeric_limits<int64_t>::max() -
                                estimatedCost)
                        ? std::numeric_limits<int64_t>::max()
                        : estimatedCost +
                            static_cast<int64_t>(contribution);
                }
                // AIPlayer::isPossibleToBuildTeam evaluates each roster row
                // as thingCost * ((min + max) / 2.0f), then compound-adds the
                // result to an Int. Preserve the half-unit contribution per
                // row without carrying fractions across roster rows.
                option.estimatedCost = estimatedCost;
                option.buildableWithIdleFactory = hasRoster &&
                    allRosterTypesHaveFactory && anyIdleFactory;

                if (definition.plan.automaticallyReinforce &&
                    !option.assemblyInProgress) {
                    bool prototypeProductionBusy = false;
                    for (const ObjectTeamId instance : instances) {
                        if (m_world.m_objectProduction.pendingUnitCountForTeam(
                                m_world.m_registry, instance, {},
                                UINT32_MAX) != 0) {
                            prototypeProductionBusy = true;
                            break;
                        }
                    }
                    std::optional<StrategicAITeamReinforcementOption>
                        reinforcement;
                    if (!prototypeProductionBusy) {
                        for (const ObjectTeamId instance : instances) {
                            const ObjectTeamRecord* record =
                                m_world.m_objectTeams.find(instance);
                            if (!record || !record->active ||
                                record->members.empty()) {
                                continue;
                            }
                            for (const scenario::ScenarioTeamUnitPlan& unit :
                                 definition.plan.units) {
                                if (unit.maximumUnits == 0) continue;
                                const container::SharedPtr<const
                                    game::ObjectArchetype> product =
                                    m_content.m_contentSnapshot
                                        .findObjectArchetype(
                                            unit.templateName);
                                if (!product) continue;
                                uint32_t count = 0;
                                for (const ObjectId member :
                                     record->members.values()) {
                                    const std::optional<ecs::entity> entity =
                                        m_world.m_objects.entityFromId(member);
                                    const ThingTemplateComponent* type = entity
                                        ? ecs::try_get<ThingTemplateComponent>(
                                              m_world.m_registry, *entity)
                                        : nullptr;
                                    if (type && type->archetype &&
                                        game::legacyThingTemplatesEquivalent(
                                            type->archetype->templateData,
                                            product->templateData) &&
                                        count != std::numeric_limits<
                                            uint32_t>::max()) {
                                        ++count;
                                    }
                                }
                                if (count >= unit.maximumUnits) continue;
                                const bool hasIdleFactory = std::any_of(
                                    snapshot.productionOptions.begin(),
                                    snapshot.productionOptions.end(),
                                    [&unit](
                                        const StrategicAIProductionOption&
                                            candidate) noexcept {
                                        return candidate.queueDepth == 0 &&
                                            candidate.producer &&
                                            candidate.productType ==
                                                unit.templateName;
                                    });
                                if (!hasIdleFactory) continue;
                                // RefCode retains the final missing roster
                                // type/instance encountered for a prototype.
                                reinforcement =
                                    StrategicAITeamReinforcementOption{
                                        .definition = definition.id,
                                        .team = instance,
                                        .productType = unit.templateName,
                                        .priority = option.priority,
                                    };
                            }
                        }
                    }
                    if (reinforcement) {
                        snapshot.teamReinforcementOptions.push_back(
                            std::move(*reinforcement));
                    }
                }
                snapshot.teamProductionOptions.push_back(std::move(option));
            }

            if (teamSelectionDue) {
                int32_t highestPriority =
                    std::numeric_limits<int32_t>::min();
                uint32_t tied = 0;
                for (const StrategicAITeamProductionOption& option :
                     snapshot.teamProductionOptions) {
                    const bool belowLimit = option.maximumInstances > 0 &&
                        option.instanceCount < static_cast<uint32_t>(
                            option.maximumInstances);
                    if (!option.conditionSatisfied || !belowLimit ||
                        option.assemblyInProgress ||
                        !option.buildableWithIdleFactory ||
                        snapshot.cash < strategicTeamStartCash(
                            option.estimatedCost,
                            m_content.m_objectSimulationRules.ai
                                .teamResourcesToStart)) {
                        continue;
                    }
                    if (option.priority > highestPriority) {
                        highestPriority = option.priority;
                        tied = 0;
                    }
                    if (option.priority == highestPriority) ++tied;
                }
                if (tied > 1) {
                    snapshot.teamPriorityTieBreakIndex =
                        static_cast<uint32_t>(
                            m_content.m_simulationRandom.integerInclusive(
                                0, static_cast<int32_t>(tied - 1)));
                }
            }
        }

        // ObjectOwnershipIndex and activePlayerIds are stable-order
        // projections. Prefer an enemy structure as the long-term assault
        // target, while independently selecting the nearest enemy unit in
        // the base threat circle for the defense team.
        for (const PlayerId enemy :
             m_content.m_players.activePlayerIds()) {
            if (m_content.m_players.relationship(
                    player, enemy) != PlayerRelationship::Enemies) {
                continue;
            }
            StrategicAIEnemyCandidate enemyState{.player = enemy};
            math::q32_32 minimumX{};
            math::q32_32 maximumX{};
            math::q32_32 minimumY{};
            math::q32_32 maximumY{};
            bool foundStructurePosition = false;
            for (const ObjectId candidate :
                 m_world.m_ownership.objects(enemy)) {
                const std::optional<ecs::entity> entity =
                    m_world.m_objects.entityFromId(candidate);
                if (!entity || !isLiveStrategicObject(
                        m_world.m_registry, m_world.m_objects,
                        *entity, candidate)) {
                    continue;
                }
                enemyState.hasObjects = true;
                const ObjectKindOfComponent* kinds =
                    ecs::try_get<ObjectKindOfComponent>(
                        m_world.m_registry, *entity);
                const bool structure = hasKindOf(
                    kinds, game::ObjectKindOf::Structure);
                enemyState.hasUnits = enemyState.hasUnits ||
                    hasKindOf(kinds, game::ObjectKindOf::Infantry) ||
                    hasKindOf(kinds, game::ObjectKindOf::Vehicle) ||
                    hasKindOf(kinds, game::ObjectKindOf::Aircraft);
                enemyState.hasBuildFacility =
                    enemyState.hasBuildFacility ||
                    hasKindOf(kinds, game::ObjectKindOf::FsFactory) ||
                    hasKindOf(kinds, game::ObjectKindOf::FsBarracks) ||
                    hasKindOf(kinds, game::ObjectKindOf::FsWarfactory) ||
                    hasKindOf(kinds, game::ObjectKindOf::FsAirfield);
                if (!structure) continue;
                const ObjectFixedTransformComponent* transform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        m_world.m_registry, *entity);
                if (!transform || !transform->authoritative) continue;
                const math::q32_32 x = transform->position.x;
                const math::q32_32 y = transform->position.y;
                if (!foundStructurePosition) {
                    minimumX = maximumX = x;
                    minimumY = maximumY = y;
                    foundStructurePosition = true;
                } else {
                    minimumX = math::q32_32::min(minimumX, x);
                    maximumX = math::q32_32::max(maximumX, x);
                    minimumY = math::q32_32::min(minimumY, y);
                    maximumY = math::q32_32::max(maximumY, y);
                }
            }
            if (foundStructurePosition) {
                enemyState.centerX = minimumX +
                    (maximumX - minimumX) /
                        math::q32_32{int32_t{2}};
                enemyState.centerY = minimumY +
                    (maximumY - minimumY) /
                        math::q32_32{int32_t{2}};
            } else {
                enemyState.centerX = snapshot.baseAnchorX;
                enemyState.centerY = snapshot.baseAnchorY;
            }
            snapshot.enemyCandidates.push_back(enemyState);
        }

        ObjectId fallbackTarget = INVALID_OBJECT_ID;
        uint8_t bestAssaultPriority = std::numeric_limits<uint8_t>::max();
        math::q32_32 bestAssaultDistance{};
        bool foundAssaultTarget = false;
        bool foundThreat = false;
        math::q32_32 bestThreatDistance{};
        const math::q32_32 threatRadius{700};
        const math::q32_32 threatRadiusSquared =
            threatRadius * threatRadius;
        for (const PlayerId enemy :
             m_content.m_players.activePlayerIds()) {
            if (m_content.m_players.relationship(
                    player, enemy) != PlayerRelationship::Enemies) {
                continue;
            }
            for (const ObjectId candidate :
                 m_world.m_ownership.objects(enemy)) {
                const std::optional<ecs::entity> entity =
                    m_world.m_objects.entityFromId(candidate);
                if (!entity || !isLiveStrategicObject(
                        m_world.m_registry,
                        m_world.m_objects,
                        *entity, candidate)) {
                    continue;
                }
                const ObjectKindOfComponent* kinds =
                    ecs::try_get<ObjectKindOfComponent>(
                        m_world.m_registry, *entity);
                const ObjectFixedTransformComponent* fixedTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        m_world.m_registry, *entity);
                const bool focusedAssaultTarget = !brain->currentEnemy ||
                    brain->currentEnemy == enemy;
                uint8_t assaultPriority = 4;
                if (hasKindOf(kinds, game::ObjectKindOf::CommandCenter)) {
                    assaultPriority = 0;
                } else if (hasKindOf(kinds, game::ObjectKindOf::FsFactory) ||
                           hasKindOf(kinds, game::ObjectKindOf::FsBarracks) ||
                           hasKindOf(kinds, game::ObjectKindOf::FsWarfactory) ||
                           hasKindOf(kinds, game::ObjectKindOf::FsAirfield) ||
                           hasKindOf(kinds, game::ObjectKindOf::Harvester)) {
                    assaultPriority = 1;
                } else if (hasKindOf(kinds, game::ObjectKindOf::Structure)) {
                    assaultPriority = 2;
                } else if (focusedAssaultTarget && !fallbackTarget) {
                    fallbackTarget = candidate;
                }
                math::q32_32 assaultDistance{};
                if (fixedTransform && fixedTransform->authoritative &&
                    snapshot.hasBaseAnchor) {
                    const math::q32_32 dx = fixedTransform->position.x -
                        snapshot.baseAnchorX;
                    const math::q32_32 dy = fixedTransform->position.y -
                        snapshot.baseAnchorY;
                    assaultDistance = dx * dx + dy * dy;
                }
                if (focusedAssaultTarget && assaultPriority < 4 &&
                    (!foundAssaultTarget ||
                     assaultPriority < bestAssaultPriority ||
                     (assaultPriority == bestAssaultPriority &&
                      (assaultDistance < bestAssaultDistance ||
                       (assaultDistance == bestAssaultDistance &&
                        candidate < snapshot.preferredEnemyTarget))))) {
                    foundAssaultTarget = true;
                    bestAssaultPriority = assaultPriority;
                    bestAssaultDistance = assaultDistance;
                    snapshot.preferredEnemyTarget = candidate;
                }
                const bool threatensBase =
                    m_ai.m_objectAI.hasOrderCapability(
                        candidate, ai::ObjectAIOrderCapability::Attack);
                if (threatensBase && fixedTransform &&
                    fixedTransform->authoritative && snapshot.hasBaseAnchor) {
                    const math::q32_32 dx = fixedTransform->position.x -
                        snapshot.baseAnchorX;
                    const math::q32_32 dy = fixedTransform->position.y -
                        snapshot.baseAnchorY;
                    const math::q32_32 distance = dx * dx + dy * dy;
                    if (distance <= threatRadiusSquared &&
                        (!foundThreat || distance < bestThreatDistance ||
                         (distance == bestThreatDistance &&
                          candidate < snapshot.threatTarget))) {
                        foundThreat = true;
                        bestThreatDistance = distance;
                        snapshot.threatTarget = candidate;
                    }
                }
            }
        }
        snapshot.baseThreatened = foundThreat;
        if (!snapshot.preferredEnemyTarget)
            snapshot.preferredEnemyTarget = fallbackTarget;

        snapshots.push_back(std::move(snapshot));
    }

    container::Vector<StrategicAIAction> actions;
    m_ai.m_strategicAI.update(
        snapshots, tick, actions);
    const uint64_t retryTick = tick + static_cast<uint64_t>(std::max(
        1, m_content.m_startInfo.gameSpeedFPS));
    for (StrategicAIAction& action : actions) {
        switch (action.kind) {
        case StrategicAIActionKind::BuildStructure: {
            const StrategicAIBuildPlan* plan =
                m_ai.m_strategicAI.findBuildPlan(
                    action.buildPlanId);
            if (!plan) break;
            const container::SharedPtr<const game::ObjectArchetype> product =
                m_content.m_contentSnapshot
                    .findObjectArchetype(plan->objectType);
            if (!product) {
                static_cast<void>(m_ai.m_strategicAI
                    .acknowledgeBuildAdmission(
                        action.buildPlanId, false, INVALID_OBJECT_ID,
                        INVALID_OBJECT_ID,
                        std::numeric_limits<uint64_t>::max(), true));
                break;
            }
            const auto existing = std::find_if(
                m_ai.m_priorityBuildEntries.begin(),
                m_ai.m_priorityBuildEntries.end(),
                [plan](const GameSessionPriorityBuildEntry& entry) noexcept {
                    return matchesStrategicBuildEntry(entry, *plan);
                });
            if (existing !=
                m_ai.m_priorityBuildEntries.end()) {
                const bool admitted =
                    (existing->state ==
                         GameSessionPriorityBuildState::Constructing ||
                     existing->state ==
                         GameSessionPriorityBuildState::Completed) &&
                    existing->constructedObject;
                if (admitted || existing->state ==
                        GameSessionPriorityBuildState::Exhausted) {
                    static_cast<void>(m_ai.m_strategicAI
                        .acknowledgeBuildAdmission(
                            action.buildPlanId, admitted,
                            existing->reservedBuilder,
                            existing->constructedObject,
                            std::max(retryTick, existing->nextAttemptTick),
                            existing->state ==
                                GameSessionPriorityBuildState::Exhausted));
                }
                break;
            }
            // The existing durable priority BuildList remains the construction
            // authority. A later lifecycle binding will replace this retry
            // acknowledgement with the exact builder/site pair.
            const bool queued = m_scenarioPlans.buildScriptObjectNearAnchor(
                action.player, plan->objectType,
                plan->anchorX, plan->anchorY,
                action.sequence, tick, plan->yawRadians,
                plan->scriptName, plan->sourceSideOrdinal,
                plan->sourceBuildListOrdinal, plan->id,
                plan->role == StrategicAIBuildRole::Authored,
                plan->remainingRebuilds);
            // Once queued, the durable priority BuildList owns retries,
            // placement search, funds and builder recovery.  Keep the pure
            // planner Reserved until that authority reports either a concrete
            // construction or Exhausted; returning it to Unbuilt here would
            // count a successful handoff as a failed admission and could
            // enqueue the same plan again before lifecycle binding.
            if (!queued) {
                static_cast<void>(m_ai.m_strategicAI
                    .acknowledgeBuildAdmission(
                        action.buildPlanId, false, INVALID_OBJECT_ID,
                        INVALID_OBJECT_ID,
                        std::numeric_limits<uint64_t>::max(), true));
            }
            break;
        }
        case StrategicAIActionKind::ProduceUnit: {
            const GameSessionProductionCommandResult result =
                m_production.queueProduction(
                action.producer, action.player, action.productType,
                action.sequence, tick);
            static_cast<void>(m_ai.m_strategicAI
                .acknowledgeProduction(
                    action.player, action.workOrderId,
                    result.accepted, result.productionId, retryTick));
            break;
        }
        case StrategicAIActionKind::PurchaseScience: {
            const ScienceCatalog* sciences =
                m_content.m_contentSnapshot.scienceCatalog();
            const ScienceDefinition* science = sciences
                ? sciences->find(action.productType) : nullptr;
            if (science) {
                static_cast<void>(GameSessionPlayerStateTransactions{
                    m_content.m_players}.purchaseScience(
                        action.player, *science));
            }
            break;
        }
        case StrategicAIActionKind::BuildScenarioTeam: {
            const bool accepted = m_scenarioPlans.buildScriptTeam(
                action.productType, action.sequence, tick, false);
            m_ai.m_strategicAI.acknowledgeScenarioTeamBuild(
                action.player, accepted);
            break;
        }
        case StrategicAIActionKind::ReinforceScenarioTeam: {
            const bool accepted = m_scenarioPlans.reinforceScriptTeam(
                action.objectTeam, action.productType,
                action.sequence, tick);
            m_ai.m_strategicAI.acknowledgeScenarioTeamBuild(
                action.player, accepted);
            break;
        }
        case StrategicAIActionKind::AssignGathererDock:
            if (!action.actors.empty()) {
                static_cast<void>(m_objectState
                    .assignSupplyTruckPreferredDock(
                        action.actors.front(), action.target, tick));
            }
            break;
        case StrategicAIActionKind::RepairStructure: {
            const std::optional<ecs::entity> target =
                m_world.m_objects.entityFromId(action.target);
            const ObjectBridgeComponent* bridge = target
                ? ecs::try_get<ObjectBridgeComponent>(
                      m_world.m_registry, *target)
                : nullptr;
            const ObjectHealthComponent* health = target
                ? ecs::try_get<ObjectHealthComponent>(
                      m_world.m_registry, *target)
                : nullptr;
            const bool stillNeedsRepair = target && bridge && health &&
                (health->effectivelyDead || health->damageState !=
                    ObjectBodyDamageState::Pristine);
            if (stillNeedsRepair) {
                static_cast<void>(m_scenarioPlans
                    .requestPlayerRepairStructure(
                        action.player, action.target, tick));
            }
            m_ai.m_strategicAI.acknowledgeBridgeRepair(
                action.player, action.target, stillNeedsRepair,
                retryTick);
            break;
        }
        case StrategicAIActionKind::Defend:
        case StrategicAIActionKind::Attack: {
            m_ai.m_objectAI.captureOrderCapabilitySnapshot(
                m_ai.m_playerOrderCapabilitySnapshot);
            const OrderExecutionResult result =
                OrderExecutor::executeStrategicAttack(
                    m_world.m_registry, m_content.m_players,
                    m_world.m_objects, action.player,
                    static_cast<GameTick>(tick), action.sequence,
                    action.actors, action.target,
                    m_ai.m_playerOrderCapabilitySnapshot);
            m_ai.m_strategicAI.acknowledgeTacticalOrder(
                action.strategicTeamId, result.accepted, retryTick);
            break;
        }
        }
    }
}

} // namespace engine
