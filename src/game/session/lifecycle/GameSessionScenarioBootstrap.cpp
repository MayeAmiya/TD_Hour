#include "game/session/lifecycle/GameSessionScenarioBootstrapService.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"

#include "debug/debug.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/object/creation/ObjectOclSpreadPlacement.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/player/FactionTemplate.h"
#include "game/scenario/runtime/LegacyScenarioCompiler.h"
#include "game/scenario/source/LegacySkirmishScriptSource.h"
#include "game/script/runtime/ScriptProgram.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace engine {
namespace {

[[nodiscard]] script::ScriptDifficulty scriptDifficultyFromGameDifficulty(int difficulty) noexcept {
    switch (difficulty) {
    case DIFFICULTY_EASY: return script::ScriptDifficulty::Easy;
    case DIFFICULTY_HARD: return script::ScriptDifficulty::Hard;
    case DIFFICULTY_NORMAL:
    default: return script::ScriptDifficulty::Normal;
    }
}

[[nodiscard]] std::optional<script::ScriptDifficulty> scriptDifficultyFromAiDifficulty(
    AiDifficulty difficulty) noexcept {
    switch (difficulty) {
    case AiDifficulty::Easy: return script::ScriptDifficulty::Easy;
    case AiDifficulty::Normal: return script::ScriptDifficulty::Normal;
    case AiDifficulty::Hard: return script::ScriptDifficulty::Hard;
    case AiDifficulty::None: return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] int32_t scenarioBuildRebuildCount(
    const scenario::ScenarioBuildIntent& intent) noexcept {
    for (const scenario::RawScenarioField& field : intent.fields) {
        if (field.key != "rebuildCount") continue;
        int32_t value = 0;
        const char* begin = field.value.data();
        const char* end = begin + field.value.size();
        const auto parsed = std::from_chars(begin, end, value);
        return parsed.ec == std::errc{} && parsed.ptr == end
            ? value : 0;
    }
    return 0;
}

void appendLegacyScriptLoadError(script::legacy::LegacyMapScriptLoadReport& report,
                                 container::String message) {
    report.hasError = true;
    ++report.diagnosticCount;
    report.diagnostics.push_back({
        .severity = script::legacy::LegacyMapScriptLoadDiagnosticSeverity::Error,
        .message = std::move(message),
    });
}

[[nodiscard]] bool isSuccessfulRosterCompatibilityWarning(
    container::StringView warning) noexcept {
    constexpr container::StringView kUnavailablePlayerTemplate =
        " references unavailable PlayerTemplate '";
    constexpr container::StringView kRetainedGenericOwner =
        "'; retained as a generic map owner";
    return warning.find(kUnavailablePlayerTemplate) != container::StringView::npos &&
           warning.find(kRetainedGenericOwner) != container::StringView::npos;
}

void logLegacyMapScriptLoadReport(const script::legacy::LegacyMapScriptLoadReport& report,
                                  bool installed) {
    if (report.sourcePath.empty()) return;
    TD_LOG_INFO("[GameSession] Legacy map scripts: path='{}' parsed={} complete={} degraded={} program={} installed={} source={} runnable={} "
                "blocked={} diagnostics={} suppressed={}",
                report.sourcePath, report.sourceParsed, report.sourceComplete,
                report.degraded(), report.programProduced, installed,
                report.sourceScriptCount, report.runnableScriptCount, report.blockedScriptCount,
                report.diagnosticCount, report.suppressedDiagnosticCount);

    constexpr size_t kMaximumLoggedDiagnostics = 12;
    const size_t count = std::min(report.diagnostics.size(), kMaximumLoggedDiagnostics);
    for (size_t index = 0; index < count; ++index) {
        const script::legacy::LegacyMapScriptLoadDiagnostic& diagnostic = report.diagnostics[index];
        if (diagnostic.severity ==
            script::legacy::LegacyMapScriptLoadDiagnosticSeverity::Error) {
            TD_LOG_ERROR(
                "[GameSession] Legacy map script error: {} (offset={} size={} path='{}')",
                diagnostic.message, diagnostic.sourceOffset,
                diagnostic.sourceSize, diagnostic.chunkPath);
        } else if (diagnostic.severity ==
                   script::legacy::LegacyMapScriptLoadDiagnosticSeverity::Warning) {
            if (installed && report.blockedScriptCount == 0u) {
                TD_LOG_DEBUG(
                    "[GameSession] Legacy map script compatibility: {} (offset={} size={} path='{}')",
                    diagnostic.message, diagnostic.sourceOffset,
                    diagnostic.sourceSize, diagnostic.chunkPath);
            } else {
                TD_LOG_WARN(
                    "[GameSession] Legacy map script warning: {} (offset={} size={} path='{}')",
                    diagnostic.message, diagnostic.sourceOffset,
                    diagnostic.sourceSize, diagnostic.chunkPath);
            }
        } else {
            TD_LOG_DEBUG(
                "[GameSession] Legacy map script info: {} (offset={} size={} path='{}')",
                diagnostic.message, diagnostic.sourceOffset,
                diagnostic.sourceSize, diagnostic.chunkPath);
        }
    }
    const size_t unlogged = report.diagnostics.size() - count + report.suppressedDiagnosticCount;
    if (unlogged != 0) {
        if (installed && report.blockedScriptCount == 0u) {
            TD_LOG_DEBUG(
                "[GameSession] Legacy map script compatibility diagnostics suppressed from log: {}",
                unlogged);
        } else {
            TD_LOG_WARN(
                "[GameSession] Legacy map script diagnostics suppressed from log: {}",
                unlogged);
        }
    }
}

void logLegacyScenarioCompileResult(const scenario::LegacyScenarioCompileResult& result,
                                    bool applied,
                                    const ScenarioRosterApplyReport* rosterReport = nullptr) {
    TD_LOG_INFO("[GameSession] Legacy SidesList scenario: sides={} players={} teams={} buildIntents={} usable={} "
                "applied={} diagnostics={}",
                result.sourceSideCount, result.mappedPlayerCount, result.scriptTeamCount,
                result.buildIntentCount, result.usable(), applied, result.diagnostics.size());
    constexpr size_t kMaximumLoggedDiagnostics = 12;
    const size_t count = std::min(result.diagnostics.size(), kMaximumLoggedDiagnostics);
    for (size_t index = 0; index < count; ++index) {
        const scenario::LegacyScenarioDiagnostic& diagnostic = result.diagnostics[index];
        if (diagnostic.severity == scenario::LegacyScenarioDiagnosticSeverity::Error) {
            TD_LOG_ERROR("[GameSession] Legacy SidesList error: {} (offset={} path='{}')",
                         diagnostic.message, diagnostic.sourceOffset, diagnostic.sourcePath);
        } else if (diagnostic.severity == scenario::LegacyScenarioDiagnosticSeverity::Warning) {
            if (applied && result.usable()) {
                TD_LOG_DEBUG(
                    "[GameSession] Legacy SidesList compatibility: {} (offset={} path='{}')",
                    diagnostic.message, diagnostic.sourceOffset,
                    diagnostic.sourcePath);
            } else {
                TD_LOG_WARN(
                    "[GameSession] Legacy SidesList warning: {} (offset={} path='{}')",
                    diagnostic.message, diagnostic.sourceOffset,
                    diagnostic.sourcePath);
            }
        } else {
            TD_LOG_INFO("[GameSession] Legacy SidesList info: {} (offset={} path='{}')",
                        diagnostic.message, diagnostic.sourceOffset, diagnostic.sourcePath);
        }
    }
    if (result.diagnostics.size() > count) {
        if (applied && result.usable()) {
            TD_LOG_DEBUG(
                "[GameSession] Legacy SidesList compatibility diagnostics suppressed from log: {}",
                result.diagnostics.size() - count);
        } else {
            TD_LOG_WARN(
                "[GameSession] Legacy SidesList diagnostics suppressed from log: {}",
                result.diagnostics.size() - count);
        }
    }
    if (!rosterReport) return;
    for (const container::String& warning : rosterReport->warnings) {
        if (isSuccessfulRosterCompatibilityWarning(warning)) {
            TD_LOG_DEBUG("[GameSession] Scenario roster compatibility: {}", warning);
        } else {
            TD_LOG_WARN("[GameSession] Scenario roster warning: {}", warning);
        }
    }
    for (const container::String& error : rosterReport->errors) {
        TD_LOG_ERROR("[GameSession] Scenario roster error: {}", error);
    }
}

// Q32.32 2*pi, byte-identical to the constant compiled into
// math::fixed_sincos and ObjectOclSpreadPlacement.
constexpr math::q32_32 kTwoPiFixed = math::q32_32::from_raw(26986075409ll);

// PartitionManager::findPositionAround draws
// GameLogicRandomValueReal(0, TWO_PI) for the start angle of every starting
// unit (FindPositionOptions leaves startAngle == RANDOM_START_ANGLE).
// Reproducing that with SimulationRandom would tie the shared gameplay stream
// to how many roster entries a particular map/faction combination happened to
// materialize during bootstrap. Use the codebase's splitmix-over-stable-
// identity idiom instead (as in ObjectAIRuntime::waypointBranchChoice): the
// angle is a pure projection of confirmed roster identity, so it is identical
// on every peer and in every replay while still keeping several starting units
// off one another's candidate ring.
[[nodiscard]] math::q32_32 startingUnitPlacementAngle(
    PlayerId player, size_t authoredUnitIndex) noexcept {
    uint64_t value = static_cast<uint64_t>(player.value);
    value ^= static_cast<uint64_t>(authoredUnitIndex) +
        0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    return math::q32_32::from_raw(static_cast<int64_t>(
        value % static_cast<uint64_t>(kTwoPiFixed.raw())));
}

// SET_NORMAL surface admission for a not-yet-created starting unit. The
// LocomotorSurface bits are defined to equal the NavigationMovement bits (see
// the static_asserts in objectCreationNavigationMovementMask), so the frozen
// locomotor mask is usable directly. Ground is the legacy fallback: RefCode's
// findPositionAround has no locomotor concept at all and only ever tests
// ground legality for these spawns.
[[nodiscard]] navigation::NavigationMovementMask startingUnitMovementMask(
    const game::ObjectArchetype& archetype,
    const GameContentSnapshot& content) {
    navigation::NavigationMovementMask mask = 0;
    for (const game::FrozenLocomotorTemplate& locomotor :
         object_simulation_detail::collectRuntimeLocomotors(
             archetype.templateData, content,
             game::LocomotorSetSlot::Normal)) {
        mask |= locomotor.surfaces;
    }
    return mask != 0 ? mask : navigation::NavigationMovement::Ground;
}

} // namespace


void GameSessionScenarioBootstrapService::installScriptProgram(container::SharedPtr<const script::ScriptProgram> program) {
    container::Vector<container::String> trackedAreas;
    if (program) {
        for (const script::ScriptDefinition& definition : program->scripts()) {
            for (const script::ScriptAndClause& clause : definition.anyOf) {
                for (const script::ScriptCondition& condition : clause.allOf) {
                    if (const auto* area =
                            std::get_if<script::ScriptAreaTransitionCondition>(&condition)) {
                        trackedAreas.push_back(area->areaName);
                    }
                }
            }
        }
    }
    m_presentation.m_scriptRuntime.setProgram(std::move(program));
    // Program replacement is a map/script restart boundary. Never carry a
    // previous map's FREEZE_TIME, Hulk lifetime policy, or pending
    // camera-freeze arm into the newly installed runtime.
    m_presentation.m_scriptTimeFrozen = false;
    m_world.m_objectSimulation.setHulkLifetimeOverrideFrames(std::nullopt);
    // A replacement starts a fresh map-script policy domain.  RefCode's
    // GameLogic::reset initializes this flag to TRUE for every new game;
    // never leak a cinematic DISABLE_SCORING into a later installed map.
    m_presentation.m_scoreAccumulationEnabled = true;
    m_content.m_players.setScoreAccumulationEnabled(true);
    m_presentation.m_scriptCamera.disarmTimeFreeze();
    // A program replacement is also a condition-consumption boundary. An
    // event naturally completed by the previous map/script program must not
    // satisfy a same-named HAS_FINISHED_* condition in the new one.
    m_presentation.m_pendingScriptPresentationCompletions.clear();
    m_presentation.m_pendingScriptMusicLoops.clear();
    m_presentation.m_scriptPresentationCompletions.reset(m_presentation.m_scriptPresentationEpoch);
    m_presentation.m_scriptGameplayEvents.reset();
    m_presentation.m_scriptGameplayEvents.configureTrackedAreas(std::move(trackedAreas));
    m_presentation.m_scriptRuntime.setDifficulty(scriptDifficultyFromGameDifficulty(m_content.m_startInfo.difficulty));
    // A world query bridge is stack-bound to a confirmed tick. Never retain a
    // prior bridge through map reload/program replacement.
    m_presentation.m_scriptRuntime.setContext({});
}

void GameSessionScenarioBootstrapService::loadLegacyMapScriptProgram() {
    m_presentation.m_legacyMapScriptSource.reset();
    m_presentation.m_legacyMapScriptLoadReport = {};
    if (!m_content.m_terrain.isLoaded()) return;

    const game::MapSourceHandle source =
        m_content.m_terrain.startupMapSource();
    const game::MapContentIdentity& identity =
        m_content.m_terrain.contentIdentity();
    m_presentation.m_legacyMapScriptLoadReport.sourcePath = identity.resolvedPath;
    if (!source) {
        appendLegacyScriptLoadError(m_presentation.m_legacyMapScriptLoadReport,
                                    "Terrain startup map source is unavailable for legacy script parsing");
        logLegacyMapScriptLoadReport(m_presentation.m_legacyMapScriptLoadReport, false);
        return;
    }

    script::legacy::LegacyScriptCompileOptions compileOptions;
    compileOptions.logicFramesPerSecond = static_cast<uint32_t>(std::max(1, m_content.m_startInfo.gameSpeedFPS));
    const script::legacy::LegacyMapScriptLoader loader({}, compileOptions);

    // Parse before compiling: Zero Hour's whole skirmish AI tactical layer
    // lives in a standalone `Data\Scripts\SkirmishScripts.scb`, and RefCode
    // grafts it onto the map's sides in
    // SidesList::prepareForMP_or_Skirmish() before ScriptEngine ever sees the
    // program.  The compiler resolves Script/Group/Team hook names once
    // against its final input, so the graft has to happen here.
    const script::legacy::LegacyMapScriptParser parser(loader.parseOptions());
    script::legacy::LegacyMapScriptParseResult parsed = parser.parse(source->bytes());
    container::SharedPtr<const script::legacy::LegacyMapScriptSource> effectiveSource =
        parsed.source;
    if (parsed.source) {
        if (container::SharedPtr<const script::legacy::LegacyMapScriptSource> substituted =
                substituteLegacySkirmishScripts(*parsed.source, parser)) {
            effectiveSource = std::move(substituted);
        }
    }

    script::legacy::LegacyMapScriptLoadResult loaded = loader.loadSource(
        std::move(effectiveSource), parsed.complete, parsed.diagnostics,
        source->identity().resolvedPath);
    m_presentation.m_legacyMapScriptSource = std::move(loaded.source);
    m_presentation.m_legacyMapScriptLoadReport = std::move(loaded.report);
    if (loaded.program) {
        installScriptProgram(std::move(loaded.program));
    }
    logLegacyMapScriptLoadReport(m_presentation.m_legacyMapScriptLoadReport,
                                 static_cast<bool>(m_presentation.m_scriptRuntime.program()));
}

container::SharedPtr<const script::legacy::LegacyMapScriptSource>
GameSessionScenarioBootstrapService::substituteLegacySkirmishScripts(
    const script::legacy::LegacyMapScriptSource& mapSource,
    const script::legacy::LegacyMapScriptParser& parser) const {
    // RefCode calls prepareForMP_or_Skirmish() only for a multiplayer session
    // or a skirmish/skirmish-replay one (GameLogic.cpp:1377).  This is the
    // same canonical predicate the session already uses for its multiplayer
    // shroud policy, so a campaign or Challenge session never reaches the
    // substitution at all.
    const GameMode mode = m_content.m_resolvedMatchSetup
        ? m_content.m_resolvedMatchSetup->mode : m_content.m_startInfo.mode;
    if (mode != GameMode::Skirmish && !m_content.m_startInfo.network.enabled) return nullptr;
    if (!m_content.m_resolvedMatchSetup || !m_content.m_ruleset) return nullptr;

    // The second and decisive guard: a map that authored its own scripts is
    // left completely alone.  Every campaign and Generals-Challenge map
    // authors scripts, and every shipped skirmish map ships 14 empty
    // ScriptLists, which is exactly how the original tells them apart.
    if (script::legacy::legacyMapAuthorsScripts(mapSource)) return nullptr;

    script::legacy::LegacySkirmishSubstitutionOptions options;
    options.roster.reserve(m_content.m_resolvedMatchSetup->players.size());
    for (const ResolvedPlayerSetup& player : m_content.m_resolvedMatchSetup->players) {
        if (player.participation != PlayerParticipationKind::Participant) continue;
        if (player.controller != PlayerControllerKind::Human &&
            player.controller != PlayerControllerKind::Ai) {
            continue;
        }
        const FactionTemplate* faction = m_content.m_ruleset->findFaction(player.faction);
        if (!faction || faction->observer) continue;
        options.roster.push_back({
            .factionTemplateName = faction->name,
            .factionSide = faction->side,
            .factionBaseSide = faction->baseSide,
            .isHuman = player.controller == PlayerControllerKind::Human,
        });
    }
    if (options.roster.empty()) return nullptr;

    const script::legacy::LegacyMapScriptParseResult skirmish =
        parser.parseFile(script::legacy::kLegacySkirmishScriptPath);
    if (!skirmish.source || !script::legacy::isLegacyStandaloneScriptFile(*skirmish.source)) {
        TD_LOG_WARN(
            "[GameSession] Skirmish script file '{}' is unavailable or unusable; the skirmish AI script layer will not run",
            script::legacy::kLegacySkirmishScriptPath);
        return nullptr;
    }

    script::legacy::LegacySkirmishSubstitutionReport report;
    container::SharedPtr<const script::legacy::LegacyMapScriptSource> substituted =
        script::legacy::graftLegacySkirmishScripts(mapSource, *skirmish.source, options, report);
    for (const container::String& message : report.messages) {
        TD_LOG_WARN("[GameSession] Skirmish script substitution: {}", message);
    }
    if (!substituted) {
        TD_LOG_WARN("[GameSession] Skirmish script substitution produced no program for '{}'",
                    m_content.m_startInfo.mapName);
        return nullptr;
    }
    TD_LOG_INFO(
        "[GameSession] Grafted '{}' onto the map SidesList: {} ScriptLists, {} Team records ({} not owned by a participating side)",
        script::legacy::kLegacySkirmishScriptPath, report.graftedScriptListCount,
        report.graftedTeamCount, report.skippedTeamCount);
    return substituted;
}

bool GameSessionScenarioBootstrapService::applyLegacyScenarioDefinition() {
    m_presentation.m_scenarioDefinition.reset();
    if (!m_presentation.m_legacyMapScriptSource) return true;
    if (!m_content.m_resolvedMatchSetup || !m_content.m_ruleset) {
        TD_LOG_ERROR("[GameSession] Cannot apply SidesList without a resolved match setup and ruleset");
        return false;
    }

    const scenario::LegacyScenarioCompiler compiler;
    scenario::LegacyScenarioCompileResult compiled = compiler.compile(
        *m_presentation.m_legacyMapScriptSource, *m_content.m_resolvedMatchSetup, *m_content.m_ruleset);
    if (!compiled.usable()) {
        logLegacyScenarioCompileResult(compiled, false);
        return false;
    }

    ScenarioRosterApplyReport rosterReport;
    // Campaign/Challenge SidesList relations are authored and directed. The
    // original starts those players Neutral and only applies playerEnemies /
    // playerAllies. Skirmish/network sessions instead inherit the resolved
    // lobby alliance partition before any authored overrides are applied.
    const GameMode resolvedMode = m_content.m_resolvedMatchSetup->mode;
    const ScenarioDiplomacyBaseline diplomacyBaseline =
        resolvedMode == GameMode::Skirmish || m_content.m_startInfo.network.enabled
        ? ScenarioDiplomacyBaseline::PreserveMatchAlliances
        : ScenarioDiplomacyBaseline::AuthoredNeutral;
    if (!m_content.m_players.applyScenarioDefinition(
            *compiled.definition, *m_content.m_ruleset,
            diplomacyBaseline, &rosterReport)) {
        logLegacyScenarioCompileResult(compiled, false, &rosterReport);
        return false;
    }
    m_presentation.m_scenarioDefinition = compiled.definition;
    // RefCode's ScriptEngine walks ScriptList by SidesList position, not by
    // an assumed-unique player name.  Bind those immutable source positions
    // to the session roster after ScenarioDefinition has accepted duplicate
    // names with first-match lookup semantics.
    container::Vector<script::ScriptSidePlayerBinding> scriptPlayerBindings;
    scriptPlayerBindings.reserve(m_presentation.m_scenarioDefinition->sides().size());
    for (const scenario::ScenarioSideBinding& side : m_presentation.m_scenarioDefinition->sides()) {
        const PlayerState* player = m_content.m_players.get(side.player);
        scriptPlayerBindings.push_back({
            .sourceSideOrdinal = side.sourceSideOrdinal,
            .player = side.player,
            .effectiveDifficulty = player && player->controller == PlayerControllerKind::Ai
                ? scriptDifficultyFromAiDifficulty(player->aiDifficulty)
                : std::nullopt,
        });
    }
    m_presentation.m_scriptRuntime.setSidePlayerBindings(std::move(scriptPlayerBindings));
    logLegacyScenarioCompileResult(compiled, true, &rosterReport);
    return true;
}

bool GameSessionScenarioBootstrapService::initializeObjectTeams() {
    m_world.m_objectTeams.reset();
    if (!m_world.m_objectTeams.initializePlayerDefaults(m_content.m_players.activePlayerIds())) {
        TD_LOG_ERROR("[GameSession] Could not create default ObjectTeams for the materialized player roster");
        return false;
    }
    if (!m_presentation.m_scenarioDefinition) return true;

    for (const scenario::ScriptTeamDefinition& definition : m_presentation.m_scenarioDefinition->scriptTeams()) {
        if (!m_content.m_players.get(definition.resolvedOwner)) {
            TD_LOG_ERROR("[GameSession] Scenario Team '{}' has no materialized owner {}",
                         definition.name, definition.resolvedOwner.value);
            m_world.m_objectTeams.reset();
            return false;
        }
        if (!m_world.m_objectTeams.configureScenarioTeamProductionPolicy(
                definition.id, definition.plan.productionPriority,
                definition.plan.productionPrioritySuccessIncrease,
                definition.plan.productionPriorityFailureDecrease)) {
            TD_LOG_ERROR(
                "[GameSession] Could not configure Scenario Team '{}' production priority",
                definition.name);
            m_world.m_objectTeams.reset();
            return false;
        }

        if (definition.isPlayerDefault) {
            // Player::setDefaultTeam() points team<playerName> to the same
            // Team instance used by ordinary ownership/production. Preserve
            // that identity instead of introducing a second Scenario group.
            const std::optional<ObjectTeamId> playerDefault =
                m_world.m_objectTeams.defaultTeam(definition.resolvedOwner);
            if (!playerDefault || !m_world.m_objectTeams.bindScenarioTeamAlias(definition.id, *playerDefault)) {
                TD_LOG_ERROR("[GameSession] Could not bind default Scenario Team '{}' (definition={} owner={})",
                             definition.name, definition.id.value, definition.resolvedOwner.value);
                m_world.m_objectTeams.reset();
                return false;
            }
            continue;
        }

        if (definition.isSingleton &&
            !m_world.m_objectTeams.createScenarioTeamInstance(definition.id, definition.name,
                                                       definition.resolvedOwner, false)) {
            TD_LOG_ERROR("[GameSession] Could not initialize singleton Scenario Team '{}' (definition={} owner={})",
                         definition.name, definition.id.value, definition.resolvedOwner.value);
            m_world.m_objectTeams.reset();
            return false;
        }
    }
    return true;
}

void GameSessionScenarioBootstrapService::materializeMatchStartingBases() {
    if (!m_content.m_active || !m_content.m_ruleset ||
        !m_content.m_terrain.isLoaded()) {
        return;
    }
    // RefCode gates the entire placeNetworkBuildingsForPlayer loop on
    // `TheGameInfo`, which exists only for GAME_SKIRMISH / GAME_LAN /
    // GAME_INTERNET and for replays of those. That single condition is also how
    // it avoids generating a second base on a campaign map: GAME_SINGLE_PLAYER
    // and GAME_CHALLENGE leave TheGameInfo null, so a mission map keeps only
    // the structures its own SidesList BuildList authored. Mirror the same
    // distinction with this session's canonical match mode rather than probing
    // the scenario for authored build entries, so a skirmish map that happens
    // to author InitiallyBuilt structures still behaves exactly as it does in
    // the original game.
    const GameMode recordedMode =
        m_content.m_startInfo.mode == GameMode::Replay &&
            m_content.m_resolvedMatchSetup
        ? m_content.m_resolvedMatchSetup->mode
        : m_content.m_startInfo.mode;
    if (!m_content.m_startInfo.network.enabled &&
        recordedMode != GameMode::Skirmish) {
        return;
    }

    const container::Span<const game::terrain::MultiplayerStartPosition>
        starts = m_content.m_terrain.multiplayerStartPositions();
    size_t requested = 0;
    size_t buildingCount = 0;
    size_t unitCount = 0;
    size_t missingWaypointCount = 0;
    size_t centerFallbackCount = 0;
    size_t rejectedCount = 0;

    // activePlayerIds() is sorted ascending and PlayerId::fromSlot keeps the
    // roster order identical to RefCode's `for (i = 0; i < MAX_SLOTS; ++i)`
    // slot walk, so placement order is canonical without a second sort.
    for (const PlayerId playerId : m_content.m_players.activePlayerIds()) {
        const PlayerState* player = m_content.m_players.get(playerId);
        // isCommandPlayer() is the modern spelling of RefCode's "slot occupied
        // and not PLAYERTEMPLATE_OBSERVER" pair of tests: it requires a lobby
        // slot, Participant participation and a Human/Ai controller, so
        // observers and scenario-only/neutral owners are skipped exactly as the
        // original loop skips them.
        if (!player || !player->isCommandPlayer()) continue;
        const FactionTemplate* faction =
            m_content.m_ruleset->findFaction(player->faction);
        if (!faction) continue;
        const container::String& buildingName =
            faction->simulation.startingBuilding;
        if (buildingName.empty()) {
            // RefCode asserts and returns; a faction that authors no
            // StartingBuilding (FactionObserver, civilian-only mods) also
            // receives no starting units.
            TD_LOG_WARN(
                "[GameSession] Player {} faction '{}' authors no StartingBuilding",
                playerId.value, faction->name);
            continue;
        }
        ++requested;
        // A participant with more players than the map has Player_N_Start
        // waypoints is left unresolved by match setup; RefCode asserts on the
        // missing waypoint and returns without placing anything.
        const game::terrain::MultiplayerStartPosition* start = nullptr;
        if (player->startPosition >= 0) {
            for (const game::terrain::MultiplayerStartPosition& candidate :
                 starts) {
                if (candidate.index != player->startPosition) continue;
                start = &candidate;
                break;
            }
        }
        if (!start) {
            ++missingWaypointCount;
            TD_LOG_WARN(
                "[GameSession] Player {} resolved start position {} has no Player_N_Start waypoint; no starting base placed",
                playerId.value, player->startPosition);
            continue;
        }

        const container::SharedPtr<const game::ObjectArchetype> building =
            m_content.m_contentSnapshot.findObjectArchetype(buildingName);
        if (!building) {
            ++rejectedCount;
            TD_LOG_WARN(
                "[GameSession] Player {} StartingBuilding '{}' is absent from this session's content snapshot",
                playerId.value, buildingName);
            continue;
        }

        const math::q32_32 startX =
            math::q32_32::from_raw(start->positionRaw[0]);
        const math::q32_32 startY =
            math::q32_32::from_raw(start->positionRaw[1]);
        // RefCode re-samples the waypoint Z from TheTerrainLogic at this point
        // instead of trusting the authored value.
        const math::q32_32 startZ = math::q32_32::from_raw(
            m_content.m_terrain.groundHeightRaw(startX.raw(), startY.raw()));

        // placeObjectAtPosition orients every spawn to the template's
        // PlacementViewAngle and never flattens terrain for these structures:
        // multiplayer start waypoints are authored on buildable ground.
        // onStructureConstructionComplete(builder=null, isRebuild=false) then
        // scores the con-yard as built and charges its cost to the score
        // keeper, which is what scoreAsBuilt/scoreConstructionCost express.
        // Academy production is deliberately not recorded: RefCode only calls
        // AcademyStats::recordProduction from DozerAIUpdate and
        // ProductionUpdate.
        const GameSessionObjectSpawnResult spawnedBuilding =
            m_lifecycle.spawnObject({
                .templateName = buildingName,
                .owner = playerId,
                .transform = ObjectFixedTransformComponent{
                    .position = {startX, startY, startZ},
                    .yawRadians =
                        building->templateData.placementViewAngleRadiansFixed,
                    .authoritative = true,
                },
                .origin = ObjectCreationOrigin::Scenario,
                .confirmedTick = m_presentation.m_confirmedTick,
                .scoreAsBuilt = true,
                .scoreConstructionCost = true,
            });
        if (!spawnedBuilding) {
            ++rejectedCount;
            TD_LOG_WARN(
                "[GameSession] Player {} StartingBuilding '{}' could not be created",
                playerId.value, buildingName);
            continue;
        }
        ++buildingCount;

        // The live geometry is authoritative because a rebuild-hole or map
        // override may replace the template footprint.
        math::q32_32 buildingRadius =
            building->templateData.geometry.boundingSphereRadiusFixed;
        if (spawnedBuilding.entity) {
            if (const ObjectGeometryComponent* geometry =
                    ecs::try_get<ObjectGeometryComponent>(
                        m_world.m_registry, *spawnedBuilding.entity)) {
                buildingRadius = geometry->boundingSphereRadiusFixed;
            }
        }
        buildingRadius =
            math::q32_32::max(math::q32_32{}, buildingRadius);

        // Without a rally waypoint RefCode offsets the unit centre by half the
        // con-yard bounding sphere along -Y; Player_N_Rally replaces the whole
        // position when it exists.
        math::q32_32 rallyX = startX;
        math::q32_32 rallyY =
            startY - buildingRadius / math::q32_32{int32_t{2}};
        const container::String rallyWaypointName = "Player_" +
            std::to_string(player->startPosition + 1) + "_Rally";
        if (const game::terrain::WaypointRecord* rally =
                m_content.m_terrain.waypointByName(rallyWaypointName)) {
            rallyX = math::q32_32::from_raw(rally->positionRaw[0]);
            rallyY = math::q32_32::from_raw(rally->positionRaw[1]);
        }
        const math::q32_32 rallyZ = math::q32_32::from_raw(
            m_content.m_terrain.groundHeightRaw(rallyX.raw(), rallyY.raw()));

        const math::q32_32 minimumRadius = buildingRadius *
            math::q32_32{int32_t{7}} / math::q32_32{int32_t{10}};
        const math::q32_32 maximumRadius = buildingRadius *
            math::q32_32{int32_t{13}} / math::q32_32{int32_t{10}};

        // StartingUnit0..9 is a fixed authored array; iterating it by index is
        // both RefCode's order and the canonical deterministic order.
        for (size_t unitIndex = 0;
             unitIndex < faction->simulation.startingUnits.size();
             ++unitIndex) {
            const container::String& unitName =
                faction->simulation.startingUnits[unitIndex];
            if (unitName.empty()) continue;
            const container::SharedPtr<const game::ObjectArchetype> unit =
                m_content.m_contentSnapshot.findObjectArchetype(unitName);
            if (!unit) {
                ++rejectedCount;
                TD_LOG_WARN(
                    "[GameSession] Player {} StartingUnit{} '{}' is absent from this session's content snapshot",
                    playerId.value, unitIndex, unitName);
                continue;
            }

            // findObjectOclSpreadPlacement is this codebase's deterministic
            // stand-in for PartitionManager::findPositionAround: same 5-unit
            // concentric rings and +/- angle ping-pong, but admission tests the
            // complete bounding-circle footprint against navigation instead of
            // a 5-unit point sphere. The con-yard footprint is already
            // published into navigation by the spawn above, so no starting unit
            // can land inside it.
            const ObjectOclSpreadPlacementResult placement =
                findObjectOclSpreadPlacement(
                    m_content.m_navigation.layers(),
                    {
                        .center = {rallyX.raw(), rallyY.raw(), rallyZ.raw()},
                        .minimumRadius = minimumRadius,
                        .maximumRadius = maximumRadius,
                        .startAngleRadians =
                            startingUnitPlacementAngle(playerId, unitIndex),
                        .footprintRadius = math::q32_32::max(
                            math::q32_32{},
                            unit->templateData.geometry
                                .boundingCircleRadiusFixed),
                        .movementMask = startingUnitMovementMask(
                            *unit, m_content.m_contentSnapshot),
                    },
                    m_content.m_navigationFootprintScratch);
            if (!placement.found()) {
                // findPositionAround has one success path that ignores the ring
                // search entirely: an off-extent centre yields `*result =
                // *center`. Take the same centre for an exhausted search rather
                // than RefCode's "Could not find position" skip, because this
                // admission requires the unit's whole bounding circle to fit
                // traversable cells where RefCode only tested a point. Losing a
                // faction's only StartingUnit would leave the owner without a
                // dozer/worker and, for an AI, without any builder at all;
                // overlapping spawns are separated by ordinary collision
                // resolution on the first confirmed frames.
                ++centerFallbackCount;
            }
            const math::q32_32 unitX =
                math::q32_32::from_raw(placement.position.xRaw);
            const math::q32_32 unitY =
                math::q32_32::from_raw(placement.position.yRaw);
            game::terrain::TerrainPathfindLayerId unitLayer =
                game::terrain::kGroundPathfindLayer;
            std::optional<uint32_t> initialPathfindLayer;
            math::q32_32 unitZ = math::q32_32::from_raw(
                m_content.m_terrain.groundHeightRaw(
                    unitX.raw(), unitY.raw()));
            if (navigation::tryTerrainPathfindLayerFromNavigationLayer(
                    placement.navigationLayer, unitLayer)) {
                unitZ = math::q32_32::from_raw(
                    m_content.m_terrain.pathfindLayerHeightRawAt(
                        unitLayer, unitX.raw(), unitY.raw())
                        .value_or(placement.position.zRaw));
                if (unitLayer != game::terrain::kGroundPathfindLayer) {
                    unitZ += math::q32_32{int32_t{1}};
                }
                initialPathfindLayer = unitLayer;
            }

            // onUnitCreated(factory=null, unit) scores the unit as built and
            // notifies the AI; it never charges money or academy production.
            const GameSessionObjectSpawnResult spawnedUnit =
                m_lifecycle.spawnObject({
                    .templateName = unitName,
                    .owner = playerId,
                    .transform = ObjectFixedTransformComponent{
                        .position = {unitX, unitY, unitZ},
                        .yawRadians = unit->templateData
                            .placementViewAngleRadiansFixed,
                        .authoritative = true,
                    },
                    .initialPathfindLayer = initialPathfindLayer,
                    .origin = ObjectCreationOrigin::Scenario,
                    .confirmedTick = m_presentation.m_confirmedTick,
                    .scoreAsBuilt = true,
                });
            if (!spawnedUnit) {
                ++rejectedCount;
                TD_LOG_WARN(
                    "[GameSession] Player {} StartingUnit{} '{}' could not be created",
                    playerId.value, unitIndex, unitName);
                continue;
            }
            ++unitCount;
        }
    }

    if (requested != 0 || missingWaypointCount != 0) {
        TD_LOG_INFO(
            "[GameSession] Match starting bases: players={} buildings={} units={} missingStartWaypoint={} rallyCenterFallback={} rejected={}",
            requested, buildingCount, unitCount, missingWaypointCount,
            centerFallbackCount, rejectedCount);
    }
}

void GameSessionScenarioBootstrapService::materializeScenarioInitialBuildings() {
    if (!m_content.m_active || !m_presentation.m_scenarioDefinition) return;

    auto& durableBuildList = m_ai.m_priorityBuildEntries;
    const auto appendBuildListNode = [&] (
        const scenario::ScenarioBuildIntent& intent,
        GameSessionPriorityBuildState state,
        ObjectId object = INVALID_OBJECT_ID) {
        const auto duplicate = std::find_if(
            durableBuildList.begin(), durableBuildList.end(),
            [&intent](const GameSessionPriorityBuildEntry& entry) noexcept {
                return entry.authoredBuildList &&
                    entry.sourceSideOrdinal == intent.sourceSideOrdinal &&
                    entry.sourceBuildListOrdinal ==
                        intent.sourceBuildListOrdinal;
            });
        if (duplicate != durableBuildList.end() ||
            durableBuildList.size() >= 4096u) return;
        durableBuildList.push_back({
            .player = intent.resolvedOwner,
            .objectType = intent.templateName,
            .anchorX = intent.x,
            .anchorY = intent.y,
            .yawRadians = intent.angle,
            .scriptName = intent.structureName,
            .sourceSideOrdinal = intent.sourceSideOrdinal,
            .sourceBuildListOrdinal = intent.sourceBuildListOrdinal,
            .createdTick = m_presentation.m_confirmedTick,
            .nextAttemptTick = m_presentation.m_confirmedTick,
            .state = state,
            .constructedObject = object,
            .remainingRebuilds = scenarioBuildRebuildCount(intent),
            .authoredBuildList = true,
        });
    };

    for (const scenario::ScenarioBuildIntent& intent :
         m_presentation.m_scenarioDefinition->buildIntents()) {
        if (intent.initiallyBuilt || intent.templateName.empty() ||
            !intent.fixedPoseValid) continue;
        const PlayerState* owner =
            m_content.m_players.get(intent.resolvedOwner);
        if (!owner || owner->controller != PlayerControllerKind::Ai ||
            !m_content.m_contentSnapshot.findObjectArchetype(
                intent.templateName)) continue;
        appendBuildListNode(
            intent, GameSessionPriorityBuildState::Unbuilt);
    }

    size_t requested = 0;
    size_t spawnedCount = 0;
    size_t existingCount = 0;
    size_t rejectedCount = 0;
    for (const scenario::ScenarioBuildIntent& intent :
         m_presentation.m_scenarioDefinition->buildIntents()) {
        if (!intent.initiallyBuilt || intent.templateName.empty()) continue;
        const PlayerState* owner = m_content.m_players.get(intent.resolvedOwner);
        // RefCode reaches this loop through AIPlayer::newMap. A BuildList on
        // a human/neutral Side remains inert rather than silently creating a
        // free authoritative structure for a controller that has no AIPlayer.
        if (!owner || owner->controller != PlayerControllerKind::Ai) continue;
        ++requested;
        const container::SharedPtr<const game::ObjectArchetype> archetype =
            m_content.m_contentSnapshot.findObjectArchetype(intent.templateName);
        if (!archetype) {
            ++rejectedCount;
            TD_LOG_WARN(
                "[GameSession] InitiallyBuilt entry {}/{} references unknown template '{}'",
                intent.sourceSideOrdinal, intent.sourceBuildListOrdinal,
                intent.templateName);
            continue;
        }

        // ObjectList import runs first.  A live authored objectName is the
        // stable compatibility identity for an already-materialized entry;
        // never create a second anonymous copy merely because the old linked
        // BuildList pointer/runtime ObjectId no longer exists.
        if (!intent.structureName.empty()) {
            const std::optional<ObjectId> existing =
                m_presentation.m_scriptObjects.liveNamedObject(intent.structureName);
            if (existing) {
                const std::optional<ecs::entity> entity =
                    m_world.m_objects.entityFromId(*existing);
                const OwnerComponent* existingOwner = entity
                    ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
                    : nullptr;
                const ThingTemplateComponent* existingType = entity
                    ? ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity)
                    : nullptr;
                const bool matchesIntent = existingOwner && existingType &&
                    existingType->archetype &&
                    existingOwner->player == intent.resolvedOwner &&
                    existingType->archetype->templateData.name ==
                        archetype->templateData.name;
                if (matchesIntent &&
                    !intent.objectScriptAttachment.empty()) {
                    m_objectEvents.m_objectHookEvents.push_back({
                        .sourceSideOrdinal = intent.sourceSideOrdinal,
                        .sourceBuildListOrdinal =
                            intent.sourceBuildListOrdinal,
                        .object = *existing,
                    });
                } else if (!matchesIntent) {
                    TD_LOG_WARN(
                        "[GameSession] InitiallyBuilt entry {}/{} conflicts with live objectName '{}'",
                        intent.sourceSideOrdinal,
                        intent.sourceBuildListOrdinal,
                        intent.structureName);
                }
                if (matchesIntent) {
                    appendBuildListNode(
                        intent, GameSessionPriorityBuildState::Completed,
                        *existing);
                }
                ++existingCount;
                continue;
            }
        }
        if (!intent.fixedPoseValid) {
            ++rejectedCount;
            TD_LOG_WARN(
                "[GameSession] InitiallyBuilt entry {}/{} has an invalid pose",
                intent.sourceSideOrdinal, intent.sourceBuildListOrdinal);
            continue;
        }
        const math::q32_32 x = intent.x;
        const math::q32_32 y = intent.y;
        const math::q32_32 ground = math::q32_32::from_raw(
            m_content.m_terrain.groundHeightRaw(
                x.raw(), y.raw()));

        ObjectSpawnRequest request{
            .templateName = intent.templateName,
            .owner = intent.resolvedOwner,
            .transform = ObjectFixedTransformComponent{
                .position = {
                    x,
                    y,
                // Legacy SidesList explicitly discards the authored Z and
                // BuildAssistant::buildObjectNow snaps the result to ground.
                    ground,
                },
                .yawRadians = intent.angle,
                .authoritative = true,
            },
            .origin = ObjectCreationOrigin::Map,
            .confirmedTick = m_presentation.m_confirmedTick,
            .scriptName = intent.structureName,
            .academyAsProduction = true,
        };
        const GameSessionObjectSpawnResult spawned =
            m_lifecycle.spawnObject(std::move(request));
        if (!spawned) {
            ++rejectedCount;
            continue;
        }
        ++spawnedCount;
        appendBuildListNode(
            intent, GameSessionPriorityBuildState::Completed,
            spawned.object);

        // buildStructureNow immediately invokes objectScriptAttachment after
        // construction completion. Queue the immutable source key for the
        // first ScriptRuntime microtask window; the runtime resolves the
        // compiled attachment and establishes callingObject/currentPlayer.
        if (!intent.objectScriptAttachment.empty()) {
            m_objectEvents.m_objectHookEvents.push_back({
                .sourceSideOrdinal = intent.sourceSideOrdinal,
                .sourceBuildListOrdinal = intent.sourceBuildListOrdinal,
                .object = spawned.object,
            });
        }
    }

    if (requested != 0) {
        TD_LOG_INFO(
            "[GameSession] InitiallyBuilt BuildList entries: requested={} spawned={} existing={} rejected={}",
            requested, spawnedCount, existingCount, rejectedCount);
    }
}

} // namespace engine
