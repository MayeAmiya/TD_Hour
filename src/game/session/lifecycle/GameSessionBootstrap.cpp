#include "game/session/core/GameSession.h"
#include "game/session/integration/GameRenderExtractionCache.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/session/lifecycle/GameSessionScenarioBootstrapService.h"
#include "game/session/ai/GameSessionStrategicAIService.h"
#include "game/session/lifecycle/GameSessionWorldMaintenanceService.h"

#include "VFS.h"
#include "core/container/string_utils.h"
#include "core/config/GlobalData.h"
#include "debug/debug.h"
#include "game/base/MapContentIdentity.h"
#include "game/base/MapSourceBlob.h"
#include "game/navigation/integration/NavigationTerrainAdapter.h"
#include "game/object/simulation/combat/ObjectHistoricWeaponLedger.h"
#include "presentation/render/RenderQualitySettingsManager.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

constexpr size_t kNavigationCellStoragePerSlotBudget = 1024;
constexpr int32_t kLegacyStartupFadeDecreaseFrames = 33;

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] std::optional<uint32_t> terrainWaypointId(
    ai::AIWaypointHandle handle) noexcept {
    if (!handle || handle.value - 1 >
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(handle.value - 1);
}

[[nodiscard]] ai::AIWaypointQuery queryTerrainWaypointNode(
    const void* context, ai::AIWaypointHandle handle,
    uint64_t revision) noexcept {
    const auto* terrain =
        static_cast<const game::terrain::TerrainLogic*>(context);
    if (!terrain) return {};
    if (revision != terrain->waypointGraphRevision()) {
        return {.status = ai::AIWaypointQueryStatus::StaleRevision};
    }
    const std::optional<uint32_t> id = terrainWaypointId(handle);
    const game::terrain::WaypointRecord* waypoint = id
        ? terrain->waypointById(*id) : nullptr;
    if (!waypoint) {
        return {.status = ai::AIWaypointQueryStatus::Missing};
    }
    return {
        .status = ai::AIWaypointQueryStatus::Node,
        .node = {
            .position = {
                .xRaw = waypoint->positionRaw[0],
                .yRaw = waypoint->positionRaw[1],
                .zRaw = waypoint->positionRaw[2],
            },
            .linkCount = static_cast<uint32_t>(waypoint->links.size()),
            // RefCode uses a five-node lookahead only as a pathfinder
            // optimization. Zero preserves route correctness without making
            // the resolver depend on Navigation's mutable cell size.
            .lookAheadDistanceRaw = 0,
            .wall = false,
        },
    };
}

[[nodiscard]] ai::AIWaypointLinkQuery queryTerrainWaypointLink(
    const void* context, ai::AIWaypointHandle handle,
    uint64_t revision, uint32_t index) noexcept {
    const auto* terrain =
        static_cast<const game::terrain::TerrainLogic*>(context);
    if (!terrain) return {};
    if (revision != terrain->waypointGraphRevision()) {
        return {.status = ai::AIWaypointQueryStatus::StaleRevision};
    }
    const std::optional<uint32_t> id = terrainWaypointId(handle);
    const game::terrain::WaypointRecord* waypoint = id
        ? terrain->waypointById(*id) : nullptr;
    if (!waypoint || index >= waypoint->links.size() ||
        !terrain->waypointById(waypoint->links[index])) {
        return {.status = ai::AIWaypointQueryStatus::Missing};
    }
    return {
        .status = ai::AIWaypointQueryStatus::Node,
        .target = ai::AIWaypointHandle{
            static_cast<uint64_t>(waypoint->links[index]) + 1},
    };
}

[[nodiscard]] ai::AIWaypointGraphResolver terrainWaypointResolver(
    const game::terrain::TerrainLogic& terrain) noexcept {
    return {
        .context = &terrain,
        .queryNode = &queryTerrainWaypointNode,
        .queryLink = &queryTerrainWaypointLink,
    };
}

uint64_t nextPresentationEpoch() noexcept {
    static std::atomic<uint64_t> next{1};
    uint64_t result = next.fetch_add(1, std::memory_order_relaxed);
    while (result == 0) {
        result = next.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

[[nodiscard]] container::String siblingModifierPath(
    container::StringView mapPath, container::StringView fileName) {
    const size_t separator = mapPath.find_last_of("/\\");
    if (separator == container::StringView::npos) {
        return container::String{fileName};
    }
    container::String result{mapPath.substr(0, separator + 1u)};
    result += fileName;
    return result;
}

[[nodiscard]] container::String siblingMapIniPath(
    container::StringView mapPath) {
    return siblingModifierPath(mapPath, "map.ini");
}

[[nodiscard]] uint64_t sessionContentFingerprintBytes(
    uint64_t base, container::StringView logicalPath,
    const uint8_t* content, size_t contentSize) noexcept {
    // FNV-1a over the already frozen aggregate identity plus the exact VFS
    // winner selected for Map.ini. Path spelling is canonicalized so peers do
    // not disagree solely because one descriptor used backslashes or case.
    uint64_t hash = 1469598103934665603ull;
    const auto feed = [&](uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ull;
    };
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        feed(static_cast<uint8_t>((base >> shift) & 0xffu));
    }
    for (const unsigned char value : logicalPath) {
        const unsigned char canonical = value == '\\'
            ? '/' : static_cast<unsigned char>(std::tolower(value));
        feed(canonical);
    }
    feed(0xffu);
    for (size_t index = 0; index < contentSize; ++index) {
        feed(content[index]);
    }
    return hash == 0 ? 1u : hash;
}

[[nodiscard]] uint64_t sessionContentFingerprint(
    uint64_t base, container::StringView logicalPath,
    container::StringView content) noexcept {
    return sessionContentFingerprintBytes(
        base, logicalPath,
        reinterpret_cast<const uint8_t*>(content.data()), content.size());
}

[[nodiscard]] bool applySessionMapPresentationOverrides(
    container::StringView content, container::StringView sourcePath,
    RenderGameDataSettings& render,
    TrackMarksPresentationSettings& trackMarks,
    script::ScriptWaterPresentationSettings& water,
    script::ScriptTerrainRoadPresentationSettings& roads,
    container::String* error) {
    RenderGameDataSettings stagedRender = render;
    TrackMarksPresentationSettings stagedTrackMarks = trackMarks;
    script::ScriptWaterPresentationSettings stagedWater = water;
    script::ScriptTerrainRoadPresentationSettings stagedRoads = roads;
    container::Vector<container::String> diagnostics;
    container::String parseError;
    if (!applyRenderGameDataIni(
            content, stagedRender, &diagnostics, &parseError) ||
        !applyTrackMarksGameDataIni(
            content, stagedTrackMarks, &diagnostics, &parseError) ||
        !script::applyScriptWaterPresentationIni(
            content, stagedWater, &parseError) ||
        !script::applyScriptTerrainRoadPresentationIni(
            content, stagedRoads, &parseError)) {
        if (error) {
            *error = "session presentation modifier '" +
                container::String{sourcePath} + "' failed: " + parseError;
        }
        return false;
    }
    render = std::move(stagedRender);
    trackMarks = std::move(stagedTrackMarks);
    water = std::move(stagedWater);
    roads = std::move(stagedRoads);
    return true;
}

void setFixedBridgeSurfaceGeometry(
    game::terrain::TerrainElevatedPathfindSurface& surface,
    const LogicFixedVec3& from, const LogicFixedVec3& to,
    const container::Array<LogicFixedVec3, 4>& boundary,
    math::q32_32 height,
    math::q32_32 transitionEffectsHeight = {}) {
    const auto rawPoint = [](const LogicFixedVec3& point) {
        return container::Array<int64_t, 3>{
            point.x.raw(), point.y.raw(), point.z.raw()};
    };
    const auto projectedPoint = [](const LogicFixedVec3& point) {
        return math::vec3{point.x.to_float(), point.y.to_float(),
                          point.z.to_float()};
    };
    surface.fromRaw = rawPoint(from);
    surface.toRaw = rawPoint(to);
    surface.heightRaw = height.raw();
    surface.transitionEffectsHeightRaw =
        math::q32_32::max({}, transitionEffectsHeight).raw();
    surface.boundaryRaw.clear();
    surface.boundaryRaw.reserve(boundary.size());
    surface.boundary.clear();
    surface.boundary.reserve(boundary.size());
    for (const LogicFixedVec3& point : boundary) {
        surface.boundaryRaw.push_back(rawPoint(point));
        surface.boundary.push_back(projectedPoint(point));
    }
    surface.from = projectedPoint(from);
    surface.to = projectedPoint(to);
    surface.height = height.to_float();
    surface.transitionEffectsHeight =
        math::q32_32::from_raw(
            surface.transitionEffectsHeightRaw).to_float();
    surface.rawAuthoritative = true;
}

void installTerrainBridgePathfindSurfaces(
    game::terrain::TerrainLogic& terrain,
    const script::ScriptTerrainRoadPresentationSettings& roads,
    const GameContentSnapshot& content) {
    if (!terrain.isLoaded()) return;
    constexpr int32_t kBridgePoint1 = 0x00000010;
    constexpr int32_t kBridgePoint2 = 0x00000020;
    container::Vector<game::terrain::TerrainElevatedPathfindSurface> surfaces;
    const auto& objects = terrain.map().heightfield().objects;
    game::terrain::TerrainPathfindLayerId nextLayer = 1u;
    for (size_t index = 0; index + 1u < objects.size(); ++index) {
        const game::terrain::MapObjectRecord& first = objects[index];
        if ((first.flags & kBridgePoint1) == 0) continue;
        const game::terrain::MapObjectRecord& second = objects[index + 1u];
        if ((second.flags & kBridgePoint2) == 0) continue;
        const script::ScriptTerrainBridgeStyle* style =
            roads.findBridge(first.name);
        if (!style) {
            ++index;
            continue;
        }
        LogicFixedVec3 from{
            math::q32_32::from_raw(first.positionRaw[0]),
            math::q32_32::from_raw(first.positionRaw[1]),
            {},
        };
        LogicFixedVec3 to{
            math::q32_32::from_raw(second.positionRaw[0]),
            math::q32_32::from_raw(second.positionRaw[1]),
            {},
        };
        from.z = math::q32_32::from_raw(terrain.groundHeightRaw(
            from.x.raw(), from.y.raw())) +
            math::q32_32::from_fraction(1, 4);
        to.z = math::q32_32::from_raw(terrain.groundHeightRaw(
            to.x.raw(), to.y.raw())) +
            math::q32_32::from_fraction(1, 4);
        const math::q32_32 directionX = to.x - from.x;
        const math::q32_32 directionY = to.y - from.y;
        const math::q32_32 length = math::q32_32::sqrt(
            directionX * directionX + directionY * directionY);
        if (length <= math::q32_32{}) {
            ++index;
            continue;
        }
        const math::q32_32 sideX = -directionY / length;
        const math::q32_32 sideY = directionX / length;
        const math::q32_32 halfWidth = math::q32_32::max(
            math::q32_32::from_fraction(1, 2),
            math::q32_32{int32_t{17}} * style->scaleFixed);
        game::terrain::TerrainElevatedPathfindSurface surface;
        surface.layer = nextLayer++;
        surface.sourceRecordIndex = static_cast<uint64_t>(index);
        setFixedBridgeSurfaceGeometry(
            surface, from, to,
            container::Array<LogicFixedVec3, 4>{
                LogicFixedVec3{from.x + sideX * halfWidth,
                               from.y + sideY * halfWidth, from.z},
                LogicFixedVec3{to.x + sideX * halfWidth,
                               to.y + sideY * halfWidth, to.z},
                LogicFixedVec3{to.x - sideX * halfWidth,
                               to.y - sideY * halfWidth, to.z},
                LogicFixedVec3{from.x - sideX * halfWidth,
                               from.y - sideY * halfWidth, from.z},
            },
            (from.z + to.z) / math::q32_32{int32_t{2}},
            style->transitionEffectsHeightFixed);
        surfaces.push_back(std::move(surface));
        ++index;
    }

    // RefCode also calls addLandmarkBridgeToLogic(Object*) for authored
    // IsBridge BOX objects.  Build the same rectangle from immutable template
    // geometry before map objects spawn, so their central spawn transaction
    // can select this elevated layer immediately.
    for (size_t index = 0; index < objects.size(); ++index) {
        const game::terrain::MapObjectRecord& record = objects[index];
        if ((record.flags & (kBridgePoint1 | kBridgePoint2)) != 0) continue;
        const container::SharedPtr<const game::ObjectArchetype> archetype =
            content.findObjectArchetype(record.name);
        if (!archetype) continue;
        const game::ThingTemplate& thing = archetype->templateData;
        if ((!thing.isBridge &&
             !game::objectHasKind(archetype->kindOfMask,
                                  game::ObjectKindOf::LandmarkBridge)) ||
            thing.geometry.type != game::ObjectGeometryType::Box) {
            continue;
        }
        const math::q32_32 major = math::q32_32::max(
            math::q32_32{}, thing.geometry.majorRadiusFixed);
        const math::q32_32 minor = math::q32_32::max(
            math::q32_32{}, thing.geometry.minorRadiusFixed);
        if (major <= math::q32_32{} || minor <= math::q32_32{}) continue;
        const math::q32_32 centerX =
            math::q32_32::from_raw(record.positionRaw[0]);
        const math::q32_32 centerY =
            math::q32_32::from_raw(record.positionRaw[1]);
        const math::q32_32 centerZ =
            math::q32_32::from_raw(record.positionRaw[2]) +
            math::q32_32::from_raw(terrain.groundHeightRaw(
                centerX.raw(), centerY.raw()));
        const math::q32_32_sincos heading = math::fixed_sincos(
            math::q32_32::from_raw(record.angleRaw));
        const LogicFixedVec3 from{
            centerX - heading.cosine * major,
            centerY - heading.sine * major,
            centerZ,
        };
        const LogicFixedVec3 to{
            centerX + heading.cosine * major,
            centerY + heading.sine * major,
            centerZ,
        };
        const math::q32_32 sideX = -heading.sine;
        const math::q32_32 sideY = heading.cosine;
        const script::ScriptTerrainBridgeStyle* style =
            roads.findBridge(archetype->name);
        game::terrain::TerrainElevatedPathfindSurface surface;
        surface.layer = nextLayer++;
        surface.sourceRecordIndex = static_cast<uint64_t>(index);
        setFixedBridgeSurfaceGeometry(
            surface, from, to,
            container::Array<LogicFixedVec3, 4>{
                LogicFixedVec3{from.x + sideX * minor,
                               from.y + sideY * minor, centerZ},
                LogicFixedVec3{to.x + sideX * minor,
                               to.y + sideY * minor, centerZ},
                LogicFixedVec3{to.x - sideX * minor,
                               to.y - sideY * minor, centerZ},
                LogicFixedVec3{from.x - sideX * minor,
                               from.y - sideY * minor, centerZ},
            },
            centerZ,
            style ? style->transitionEffectsHeightFixed
                  : math::q32_32{});
        surfaces.push_back(std::move(surface));
    }
    static_cast<void>(terrain.setElevatedPathfindSurfaces(
        std::move(surfaces)));
}

// A renderer may be temporarily paused while the confirmed simulation keeps
// advancing. Bound the non-destructive script-shake journal so malformed
// repeating maps cannot grow client memory without limit; a renderer uses
// source stamps to ignore entries it already applied, so retaining recent
// history is what lets a newest-only snapshot consumer recover after drops.

[[nodiscard]] navigation::NavigationTerrainPolicy defaultNavigationTerrainPolicy() noexcept
{
    return {{1},
            {1},
            16384,
            256,
            16384,
            static_cast<uint32_t>(kNavigationCellStoragePerSlotBudget),
            256,
            4096,
            1024,
            1024};
}

// Selection-camera requests are client-local and normally consumed in the
// same confirmed frame. Keep a defensive bounded tail nevertheless, so a UI
// hitch cannot let malformed repeating maps grow session memory forever.
// OBJECT_FORCE_SELECT uses the same local main-thread handoff as selection
// camera modifiers. Keep its bounded tail independent so a malformed map
// cannot make an unconsumed local UI journal grow without limit.

// A render queue can discard intermediate logic snapshots. BW is not a final
// desired-state slot: Begin -> End in one tick must still reach the client in
// source order, otherwise End sees no installed filter and is silently lost.
// Keep a bounded replay tail just like SCREEN_SHAKE; the renderer owns an
// epoch/sequence cursor and never mutates this confirmed session history.
// Motion blur shares W3D's mutually-exclusive tactical-view filter slot with
// BW, but retains a separate source-ordered replay tail so a newest-only
// renderer can recover a Follow/EndFollow or one-shot Jump command.
// Diagnostic mirrors must not become a second unbounded presentation queue.
// The lossless FX stream is drained independently; these tails exist only for
// focused probes and gameplay diagnostics which may inspect recent producers.

[[nodiscard]] container::StringView pathFileNameView(
    container::StringView path) noexcept {
    const size_t separator = path.find_last_of("/\\");
    return separator == container::StringView::npos
        ? path : path.substr(separator + 1u);
}

[[nodiscard]] bool configureSessionVertexWater(
    game::terrain::TerrainLogic& terrain,
    const RenderWaterGameData& water) {
    if (!terrain.isLoaded() || !terrain.waypointByName("WaveGuide1")) {
        return true;
    }
    const container::StringView resolvedPath =
        terrain.contentIdentity().resolvedPath;
    for (const RenderVertexWaterGameData& source : water.vertexWater) {
        if (source.availableMaps.empty() ||
            (!equalAsciiInsensitive(resolvedPath, source.availableMaps) &&
             !equalAsciiInsensitive(pathFileNameView(resolvedPath),
                                    pathFileNameView(source.availableMaps)))) {
            continue;
        }
        return terrain.configureVertexWater({
            .positionX = source.positionX,
            .positionY = source.positionY,
            .positionZ = source.positionZ,
            .angleRadians = source.angleRadians,
            .cellsX = source.gridCellsX,
            .cellsY = source.gridCellsY,
            .gridSize = source.gridSize,
            .influenceRange = source.attenuationRange,
        });
    }
    return true;
}

[[nodiscard]] bool matchesResolvedStartDescriptor(const ResolvedMatchSetup& setup,
                                                  const GameStartInfo& info,
                                                  bool allowReplayModeOverlay) noexcept {
    const bool modeMatches = setup.mode == info.mode ||
        (allowReplayModeOverlay && info.mode == GameMode::Replay);
    return modeMatches &&
           setup.mapName == info.mapName &&
           setup.mapCrc == info.mapCRC &&
           setup.mapSize == info.mapSize &&
           setup.difficulty == info.difficulty &&
           setup.rankPoints == info.rankPoints &&
           setup.gameSpeedFps == info.gameSpeedFPS &&
           setup.seed == static_cast<uint32_t>(info.seed) &&
           setup.superweaponRestricted == info.superweaponRestricted &&
           setup.oldFactionsOnly == info.oldFactionsOnly;
}

[[nodiscard]] LocalControlContext localControlFromStartInfo(const GameStartInfo& info) noexcept {
    LocalControlContext local;
    if (info.localPlayerSlot >= 0 && info.localPlayerSlot < MAX_SLOTS) {
        local.controlledSlot = MatchPlayerSlotId{static_cast<uint8_t>(info.localPlayerSlot)};
    }
    return local;
}

[[nodiscard]] bool verifyMapContentIdentity(
    const GameStartInfo& info, const game::MapContentIdentity& actual,
    container::String& error) {
    if (info.mapName.empty()) return true;
    if (!actual.isKnown()) {
        error = "loaded map has no content identity";
        return false;
    }
    // Zero remains a deliberate legacy/dev "not declared" value.  Every
    // declared half of the descriptor is nevertheless strict: accepting a
    // matching name with different bytes would invalidate replay/lockstep
    // determinism before gameplay has even started.
    if (info.mapCRC != 0 && info.mapCRC != actual.crc) {
        error = "map CRC differs from the start descriptor";
        return false;
    }
    if (info.mapSize != 0 && info.mapSize != actual.size) {
        error = "map size differs from the start descriptor";
        return false;
    }
    return true;
}

void hydrateLegacyMapIdentity(GameStartInfo& descriptor,
                              const game::MapContentIdentity& actual) noexcept {
    // Legacy zero means "not declared".  Once TerrainLogic selected the real
    // VFS bytes, new canonical setup/replay/network values must carry them.
    if (descriptor.mapCRC == 0) descriptor.mapCRC = actual.crc;
    if (descriptor.mapSize == 0) descriptor.mapSize = actual.size;
}

void hydrateLegacyResolvedMapIdentity(ResolvedMatchSetup& setup,
                                      const GameStartInfo& originalDescriptor,
                                      const game::MapContentIdentity& actual) noexcept {
    // Upgrade old v3 replay descriptors only when both parallel legacy fields
    // were absent; any declared disagreement remains a hard startup error.
    if (originalDescriptor.mapCRC == 0 && setup.mapCrc == 0) setup.mapCrc = actual.crc;
    if (originalDescriptor.mapSize == 0 && setup.mapSize == 0) setup.mapSize = actual.size;
}

void addTerrainStartLayout(MatchDraft& draft, const game::terrain::TerrainLogic& terrain) {
    const container::Span<const game::terrain::MultiplayerStartPosition> starts =
        terrain.multiplayerStartPositions();
    draft.availableStartPositions.clear();
    draft.availableStartPositions.reserve(starts.size());
    for (const game::terrain::MultiplayerStartPosition& start : starts) {
        draft.availableStartPositions.push_back({
            .index = start.index,
            .worldX = start.position.x(),
            .worldY = start.position.y(),
        });
    }
}

[[nodiscard]] bool resolvedStartsMatchTerrain(const ResolvedMatchSetup& setup,
                                               const game::terrain::TerrainLogic& terrain,
                                               container::String& error) {
    const container::Span<const game::terrain::MultiplayerStartPosition> starts =
        terrain.multiplayerStartPositions();
    for (const ResolvedPlayerSetup& player : setup.players) {
        if (player.participation != PlayerParticipationKind::Participant) {
            continue;
        }
        if (player.startPosition < 0) {
            if (!starts.empty()) {
                error = "canonical match setup leaves a participant start unresolved on a map with start waypoints";
                return false;
            }
            continue;
        }
        const bool exists = std::any_of(starts.begin(), starts.end(),
            [&player](const game::terrain::MultiplayerStartPosition& start) {
                return start.index == player.startPosition;
            });
        if (!exists) {
            error = "canonical match setup reserves a start position absent from the loaded map";
            return false;
        }
    }
    return true;
}

} // namespace


bool GameSession::start(const GameStartInfo& info,
                        GameSessionStartDependencies dependencies) {
    shutdown();

    GameStartInfo effectiveInfo = info;
    ObjectSimulationRules effectiveObjectSimulationRules =
        dependencies.objectSimulationRules;
    const math::q32_32 zero{};
    const math::q32_32 one{int32_t{1}};
    if (effectiveObjectSimulationRules.unitDamagedThresholdFixed < zero ||
        effectiveObjectSimulationRules.unitDamagedThresholdFixed > one ||
        effectiveObjectSimulationRules.unitReallyDamagedThresholdFixed < zero ||
        effectiveObjectSimulationRules.unitReallyDamagedThresholdFixed >
            effectiveObjectSimulationRules.unitDamagedThresholdFixed) {
        TD_LOG_ERROR(
            "[GameSession] Required authored damage thresholds are absent or invalid");
        return false;
    }
    container::Vector<container::String> gameplayModifierPaths;
    container::Vector<audio::AudioContentLayer> audioContentLayers;
    container::Vector<ui::MappedImageContentLayer> mappedImageContentLayers;
    std::optional<ui::MapStringContentLayer> mapStringContentLayer;
    domainState().contentState().m_startInfo = effectiveInfo;
    domainState().contentState().m_simulationRandom.seed(static_cast<uint32_t>(effectiveInfo.seed));
    domainState().worldState().m_objects.reset();
    dependencies.objectAIRuntimeConfig.skirmishGroupFudgeDistanceRaw =
        effectiveObjectSimulationRules.ai.skirmishGroupFudgeDistance.raw();
    const uint32_t logicFramesPerSecond = std::max<uint32_t>(
        1u, effectiveObjectSimulationRules.logicFramesPerSecond);
    const auto aiMillisecondsToTicks =
        [logicFramesPerSecond](uint32_t milliseconds) noexcept {
            return static_cast<uint32_t>(std::min<uint64_t>(
                std::numeric_limits<uint32_t>::max(),
                (static_cast<uint64_t>(milliseconds) *
                     logicFramesPerSecond +
                 999u) /
                    1000u));
        };
    dependencies.objectAIRuntimeConfig.forceIdleBeforeAcquireTicks =
        aiMillisecondsToTicks(
            effectiveObjectSimulationRules.ai.forceIdleMilliseconds);
    dependencies.objectAIRuntimeConfig.guardEnemyScanIntervalTicks =
        aiMillisecondsToTicks(
            effectiveObjectSimulationRules.ai.guardEnemyScanMilliseconds);
    dependencies.objectAIRuntimeConfig.guardReturnScanIntervalTicks =
        aiMillisecondsToTicks(
            effectiveObjectSimulationRules.ai.
                guardEnemyReturnScanMilliseconds);
    dependencies.objectAIRuntimeConfig.guardChaseDurationTicks =
        aiMillisecondsToTicks(
            effectiveObjectSimulationRules.ai.
                guardChaseDurationMilliseconds);
    if (domainState().aiState().m_objectAI.initialize(dependencies.objectAIRuntimeConfig) !=
        ai::AIStateSoASlotStatus::Success) {
        TD_LOG_ERROR("[GameSession] Failed to initialize object AI runtime");
        shutdown();
        return false;
    }
    domainState().aiState().m_objectAIPathSequences.clear();
    domainState().aiState().m_objectAI.setPathSequenceResolver(domainState().aiState().m_objectAIPathSequences.resolver());
    const ai::AIWaypointGraphResolver waypointResolver =
        terrainWaypointResolver(domainState().contentState().m_terrain);
    domainState().aiState().m_objectAI.setWaypointGraphResolver(
        waypointResolver);
    domainState().contentState().m_navigation.setWaypointGraphResolver(
        waypointResolver);
    domainState().aiState().m_objectAI.setPathHandleReleaser({
        .context = &domainState().contentState().m_navigation,
        .release = [](void* context, ai::PathHandle path) noexcept {
            auto* navigation =
                static_cast<navigation::NavigationSystem*>(context);
            if (navigation && navigation->isInitialized()) {
                static_cast<void>(navigation->releasePath(
                    path, navigation->pathRevision()));
            }
        },
    });
    domainState().aiState().m_objectAIShadowFacts.clear();
    domainState().aiState().m_objectAIShadowFacts.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_objectAIShadowNextFacts.clear();
    domainState().aiState().m_objectAIShadowNextFacts.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_objectAIOrderCapabilitySnapshot.clear();
    domainState().aiState().m_objectAIOrderCapabilitySnapshot.moveStopSubjects.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_objectAIOrderCapabilitySnapshot.attackSubjects.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_objectAIOrderCapabilitySnapshot
        .autonomousAttackSubjects.reserve(
            dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_playerOrderCapabilitySnapshot.clear();
    domainState().aiState().m_playerOrderCapabilitySnapshot.moveStopSubjects.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_playerOrderCapabilitySnapshot.attackSubjects.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_scriptOrderCapabilitySnapshot.clear();
    domainState().aiState().m_scriptOrderCapabilitySnapshot.moveStopSubjects.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_scriptOrderCapabilitySnapshot.attackSubjects.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_objectAIMovementCommands.clear();
    domainState().aiState().m_objectAIMovementCommands.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    domainState().aiState().m_objectAIMoveCompletions.clear();
    domainState().aiState().m_objectAIMoveCompletions.reserve(
        dependencies.objectAIRuntimeConfig.maximumActors);
    container::Vector<ai::MovementFeedback>{}.swap(
        domainState().aiState().m_movementFeedbackDrainScratch);
    container::Vector<ai::AIFacingFeedback>{}.swap(
        domainState().aiState().m_facingFeedbackDrainScratch);
    domainState().aiState().m_strategicAI.reset();
    domainState().aiState().m_priorityBuildEntries.clear();
    domainState().aiState().m_priorityBuildEntries.reserve(64);
    domainState().worldState().m_ownership.reset();
    domainState().worldState().m_objectTeams.reset();
    domainState().worldState().m_objectCombat.reset();
    resetHistoricWeaponLedger(domainState().worldState().m_registry);
    domainState().worldState().m_objectProjectiles.reset();
    domainState().worldState().m_pendingProductionSpawns.clear();
    domainState().worldState().m_pendingProductionUpgrades.clear();
    domainState().worldState().m_objectSimulation.reset();
    domainState().worldState().m_objectSimulation.setSessionSeed(static_cast<uint64_t>(effectiveInfo.seed));
    domainState().worldState().m_objectSimulation.setRules(
        effectiveObjectSimulationRules);
    domainState().contentState().m_objectSimulationRules = domainState().worldState().m_objectSimulation.rules();
    domainState().worldState().m_spatialIndex.clear();
    container::Vector<ObjectId>{}.swap(
        domainState().presentationState().m_scriptSightQueryScratch);
    domainState().worldState().m_modelConditionAuthority = {};
    renderExtractionCache().resetWorld();
    domainState().worldState().m_mapVisibility.clear();
    domainState().contentState().m_contentSnapshot.clear();
    domainState().worldState().m_mapObjectSpawnReport = {};
    domainState().worldState().m_clientTerrainObjects.clear();
    domainState().presentationState().m_cameraDirector.reset();
    domainState().presentationState().m_pendingCameraInput.clear();
    domainState().presentationState().m_scriptCamera.reset();
    domainState().presentationState().m_scriptTimeFrozen = false;
    // A presentation epoch identifies the whole GameSession boundary, not an
    // individual output channel.  World, FX and audio snapshots may be
    // submitted in different orders, but consumers must be able to compare
    // them against one session generation without treating a sibling channel
    // as a newer match.
    const uint64_t sessionPresentationEpoch = nextPresentationEpoch();
    domainState().presentationState().m_audioJournal.reset(
        sessionPresentationEpoch);
    domainState().presentationState().m_scriptPresentationEpoch =
        sessionPresentationEpoch;
    domainState().presentationState().m_localPlacementPresentation.reset(domainState().presentationState().m_scriptPresentationEpoch);
    domainState().presentationState().m_queuedConstructionPlacements.clear();
    domainState().presentationState().m_rejectedConstructionPlacements.clear();
    domainState().presentationState().m_fxInvocations.reset(
        sessionPresentationEpoch, static_cast<uint32_t>(effectiveInfo.seed));
    domainState().presentationState().m_confirmedTick = 0;
    domainState().presentationState().m_hasConfirmedFrame = false;
    domainState().presentationState().m_objectFeedbackOrdinal = 0;
    domainState().presentationState().m_objectWorldAnimations.clear();
    domainState().presentationState().m_objectFloatingTexts.clear();
    domainState().presentationState().m_objectSelectionFlashes.clear();
    domainState().presentationState().m_commandBackendOutcomes.clear();
    domainState().presentationState().m_objectAmbientAudio.clear();
    domainState().presentationState().m_hackInternetAudio.clear();
    domainState().presentationState().m_objectLossRadarEvents.clear();
    domainState().presentationState().m_stealthRadarFeedbackHistory.clear();
    domainState().presentationState().m_underAttackRadarFeedbackHistory.clear();
    domainState().presentationState().m_evaPolledRetryTicks = {};
    domainState().presentationState().m_evaObservedRankLevel = 0;
    domainState().presentationState().m_pendingEvaAnnouncements.clear();
    domainState().objectEventState().m_projectileRadiusDecalEvents.clear();
    domainState().presentationState().m_scriptGameplayEvents.reset();
    domainState().presentationState().m_scriptSessionEvents.clear();
    domainState().presentationState().m_scriptPresentationSequence = 0;
    domainState().presentationState().m_scriptLetterboxPresentation = {
        .enabled = false,
        .stamp = {.presentationEpoch = domainState().presentationState().m_scriptPresentationEpoch},
    };
    domainState().presentationState().m_scriptScreenShakeJournal.clear();
    domainState().presentationState().m_scriptScreenShakeJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_scriptLocalizedCameraShakeJournal.clear();
    domainState().presentationState().m_scriptLocalizedCameraShakeJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_scriptMoveCameraToSelectionRequests.clear();
    domainState().presentationState().m_scriptCameraPresentationJournal.clear();
    domainState().presentationState().m_scriptCameraPresentationJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_scriptCameraMovementRevision = 0;
    domainState().presentationState().m_scriptCameraCompletedRevision = 0;
    domainState().presentationState().m_scriptCameraSlavePresentation = {
        .enabled = false,
        .stamp = {.presentationEpoch = domainState().presentationState().m_scriptPresentationEpoch},
    };
    domainState().presentationState().m_scriptForceObjectSelectionRequests.clear();
    domainState().presentationState().m_scriptScreenFadePresentation = {
        .active = true,
        .blendMode = script::ScriptScreenFadeBlendMode::Multiply,
        .minimumIntensity = 1.0f,
        .maximumIntensity = 0.0f,
        .currentIntensity = 0.0f,
        .increaseFrames = 0,
        .holdFrames = 0,
        .decreaseFrames = kLegacyStartupFadeDecreaseFrames,
        .currentFrame = 0,
        .stamp = {.presentationEpoch = domainState().presentationState().m_scriptPresentationEpoch},
    };
    domainState().presentationState().m_scriptBlackAndWhitePresentation = {
        .enabled = false,
        .transitionFrames = 0,
        .stamp = {.presentationEpoch = domainState().presentationState().m_scriptPresentationEpoch},
    };
    domainState().presentationState().m_scriptBlackAndWhiteJournal.clear();
    domainState().presentationState().m_scriptBlackAndWhiteJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_scriptMotionBlurPresentation = {
        .mode = script::ScriptMotionBlurMode::ZoomIn,
        .stamp = {.presentationEpoch = domainState().presentationState().m_scriptPresentationEpoch},
    };
    domainState().presentationState().m_scriptMotionBlurJournal.clear();
    domainState().presentationState().m_scriptMotionBlurJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_renderGameDataSettings = domainState().contentState().m_data.renderGameDataSettings();
    domainState().presentationState().m_renderGameDataSettingsSnapshot =
        std::make_shared<const RenderGameDataSettings>(
            domainState().presentationState().m_renderGameDataSettings);
    // GameContentSnapshot deliberately excludes local presentation options.
    // Freeze the manager's resolved Feature value directly: the legacy
    // RenderGameDataSettings copy remains an authored/content compatibility
    // projection and must not become a second quality authority.
    if (const auto quality = domainState().contentState().m_data.renderQualitySettingsSnapshot()) {
        domainState().presentationState().m_renderFeatureQualitySnapshot =
            std::make_shared<const ResolvedRenderFeatureSnapshot>(
                quality->feature);
        domainState().presentationState().m_initialRenderDisplaySnapshot = quality->display;
    } else {
        // The manager publishes from construction, so this is only a bounded
        // fail-safe for malformed host setup.
        domainState().presentationState().m_renderFeatureQualitySnapshot =
            std::make_shared<const ResolvedRenderFeatureSnapshot>(
                resolveRenderFeatureQuality(
                    RenderFeatureQualitySettings{}, {}, 1u));
        domainState().presentationState().m_initialRenderDisplaySnapshot = resolveRenderDisplaySettings(
            RenderDisplaySettings{},
            {}, {}, nullptr, 1u);
    }
    const uint64_t visibilityTicksPerSecond = static_cast<uint64_t>(
        std::max(1, effectiveInfo.gameSpeedFPS));
    const auto visibilityMillisecondsToTicks =
        [visibilityTicksPerSecond](uint32_t milliseconds) noexcept {
            const uint64_t scaled = visibilityTicksPerSecond * milliseconds;
            const uint64_t rounded = (scaled + 999u) / 1000u;
            return static_cast<uint32_t>(std::min<uint64_t>(
                rounded, std::numeric_limits<uint32_t>::max()));
        };
    domainState().worldState().m_visibilityUnlookPersistenceTicks = visibilityMillisecondsToTicks(
        domainState().presentationState().m_renderGameDataSettings.visual.visibility.unlookPersistMilliseconds);
    domainState().worldState().m_visibilityFogTransitionTicks = visibilityMillisecondsToTicks(
        game::terrain::kDefaultFogTransitionMilliseconds);
    domainState().presentationState().m_scriptSkyboxPresentation = {
        .enabled = domainState().presentationState().m_renderGameDataSettings.visual.water.drawSkyBox,
        .stamp = {.presentationEpoch = domainState().presentationState().m_scriptPresentationEpoch},
    };
    // Freeze WaterTransparency's five named faces alongside the session.
    // Rendering must not query mutable VFS/GameDataLoader state after a map
    // starts, and DRAW_SKYBOX_BEGIN/END itself changes only the draw flag.
    domainState().presentationState().m_scriptSkyboxPresentationTextures = domainState().contentState().m_data.skyboxPresentationTextures();
    domainState().presentationState().m_scriptWaterPresentationSettings = domainState().contentState().m_data.waterPresentationSettings();
    domainState().presentationState().m_scriptTerrainRoadPresentationSettings =
        domainState().contentState().m_data.terrainRoadPresentationSettings();
    domainState().presentationState().m_trackMarksPresentationSettings = domainState().contentState().m_data.trackMarksPresentationSettings();
    domainState().presentationState().m_scriptTreeSwayPresentation = {
        // Script state records the authored/cinematic gate. Feature quality
        // is composed at extraction so a later script enable cannot bypass a
        // session-frozen quality disable.
        .enabled = true,
        .stamp = {.presentationEpoch = domainState().presentationState().m_scriptPresentationEpoch},
    };
    domainState().presentationState().m_scriptWeatherPresentation = {
        .visible = true,
        .snow = domainState().contentState().m_data.weatherPresentationSettings(),
        .stamp = {.presentationEpoch = domainState().presentationState().m_scriptPresentationEpoch},
    };
    domainState().presentationState().m_scriptInfantryLightingPresentation = {
        .overrideScale = std::nullopt,
        .stamp = {.presentationEpoch = domainState().presentationState().m_scriptPresentationEpoch},
    };
    domainState().presentationState().m_scriptUiPresentation.reset(domainState().presentationState().m_scriptPresentationEpoch);
    domainState().presentationState().m_scriptCommandBarOverrides.reset(domainState().presentationState().m_scriptPresentationEpoch);
    domainState().presentationState().m_scriptObjectBuildabilityOverrides.clear();
    domainState().presentationState().m_scriptAttackPrioritySets.clear();
    domainState().presentationState().m_scriptAttackPriorityById.clear();
    domainState().presentationState().m_scriptAttackPriorityById.push_back(nullptr);
    domainState().presentationState().m_scriptAttackPrioritySequence = 0;
    domainState().presentationState().m_objectsReceiveDifficultyBonuses = true;
    domainState().presentationState().m_chooseVictimAlwaysNormal = false;
    domainState().presentationState().m_scriptToppleDirections.clear();
    domainState().presentationState().m_scriptRankLevelLimit = kDefaultScriptRankLevelLimit;
    domainState().presentationState().m_scoreAccumulationEnabled = true;
    domainState().contentState().m_players.setScoreAccumulationEnabled(true);
    domainState().presentationState().m_scriptClientOptions.reset(domainState().presentationState().m_scriptPresentationEpoch);
    domainState().presentationState().m_scriptMapPresentation.reset(domainState().presentationState().m_scriptPresentationEpoch);
    domainState().presentationState().m_scriptObjectPresentation.reset(domainState().presentationState().m_scriptPresentationEpoch);
    domainState().presentationState().m_scriptViewCompatibility.reset(domainState().presentationState().m_scriptPresentationEpoch);
    domainState().presentationState().m_pendingScriptPresentationCompletions.clear();
    domainState().presentationState().m_pendingVisualAnimationAdmissions.clear();
    domainState().presentationState().m_pendingVisualAnimationCompletions.clear();
    domainState().presentationState().m_pendingScriptMusicLoops.clear();
    domainState().presentationState().m_scriptPresentationCompletions.reset(domainState().presentationState().m_scriptPresentationEpoch);
    domainState().presentationState().m_scriptRuntime.setProgram({});
    domainState().presentationState().m_scriptRuntime.setContext({});
    domainState().presentationState().m_scriptOrderExecutionRecords.clear();
    domainState().presentationState().m_legacyMapScriptSource.reset();
    domainState().presentationState().m_legacyMapScriptLoadReport = {};
    domainState().presentationState().m_scenarioDefinition.reset();
    domainState().objectEventState().m_frameLifecycleEvents.clear();
    domainState().objectEventState().m_frameHealthEvents.clear();
    container::Vector<ObjectHealthEvent>{}.swap(
        domainState().objectEventState().m_healthDrainScratch);
    domainState().objectEventState().m_teamUnitDestroyedHookEvents.clear();
    domainState().objectEventState().m_objectHookEvents.clear();
    domainState().contentState().m_ruleset = std::move(dependencies.ruleset);
    if (!domainState().contentState().m_ruleset || !domainState().contentState().m_ruleset->isLoaded()) {
        TD_LOG_ERROR("[GameSession] Startup requires a loaded frozen multiplayer ruleset");
        shutdown();
        return false;
    }
    if (dependencies.simulationContentFingerprint == 0) {
        TD_LOG_ERROR("[GameSession] Startup requires a frozen aggregate simulation content fingerprint");
        shutdown();
        return false;
    }
    uint64_t effectiveSimulationContentFingerprint =
        dependencies.simulationContentFingerprint;
    const auto captureSessionContent = [&] (
        container::Span<const container::String> modifierPaths) {
        container::String contentSnapshotError;
        const bool captured = modifierPaths.empty()
            ? domainState().contentState().m_contentSnapshot.capture(
                  domainState().contentState().m_data,
                  dependencies.scienceCatalog, dependencies.upgradeCatalog,
                  &contentSnapshotError)
            : domainState().contentState().m_contentSnapshot.
                  captureWithGameplayModifiers(
                      domainState().contentState().m_data, modifierPaths,
                      dependencies.scienceCatalog,
                      dependencies.upgradeCatalog, &contentSnapshotError);
        if (!captured) {
            TD_LOG_ERROR(
                "[GameSession] Failed to freeze session gameplay content: {}",
                contentSnapshotError);
        }
        return captured;
    };
    // Terrain is part of the authoritative setup contract, not a post-roster
    // rendering convenience.  Load it before resolving random spawn slots so
    // every participant consumes the same immutable map-start layout.
    game::MapSourceHandle startupMapSource;
    if (!effectiveInfo.mapName.empty()) {
        container::String terrainError;
        if (!game::loadMapSourceBlob(
                effectiveInfo.mapName, startupMapSource, &terrainError)) {
            TD_LOG_ERROR("[GameSession] Failed to load map '{}': {}", effectiveInfo.mapName, terrainError);
            shutdown();
            return false;
        }
        const game::MapContentIdentity& sourceIdentity =
            startupMapSource->identity();
        if (!verifyMapContentIdentity(info, sourceIdentity, terrainError)) {
            TD_LOG_ERROR("[GameSession] Refusing map '{}': {} (actual path='{}' crc={} size={})",
                         effectiveInfo.mapName, terrainError,
                         sourceIdentity.resolvedPath, sourceIdentity.crc,
                         sourceIdentity.size);
            shutdown();
            return false;
        }
        hydrateLegacyMapIdentity(effectiveInfo, sourceIdentity);
        // The map blob is authoritative simulation input: terrain geometry,
        // object placement, SidesList and legacy script bytes all originate
        // from this exact VFS winner. Map CRC/size validate the launcher
        // descriptor, but the lockstep identity must also include the bytes
        // themselves so a different map cannot share the same aggregate
        // GameData fingerprint.
        effectiveSimulationContentFingerprint =
            sessionContentFingerprintBytes(
                effectiveSimulationContentFingerprint,
                sourceIdentity.resolvedPath,
                startupMapSource->bytes().data(),
                startupMapSource->bytes().size());
        const container::StringView resolvedMapPath =
            sourceIdentity.resolvedPath;
        const container::String mapIniPath =
            siblingModifierPath(resolvedMapPath, "map.ini");
        const container::String soloIniPath =
            siblingModifierPath(resolvedMapPath, "solo.ini");
        const container::String mapStringPath =
            siblingModifierPath(resolvedMapPath, "map.str");
        if (io::VFS::instance().exists(mapIniPath)) {
            gameplayModifierPaths.push_back(mapIniPath);
        }
        if (io::VFS::instance().exists(soloIniPath)) {
            gameplayModifierPaths.push_back(soloIniPath);
        }
        if (io::VFS::instance().exists(mapStringPath)) {
            container::Vector<uint8_t> mapStringBytes;
            if (io::VFS::instance().readToBuffer(
                    mapStringPath, mapStringBytes)) {
                ui::MapStringContentLayer layer;
                layer.sourcePath = mapStringPath;
                if (!mapStringBytes.empty()) {
                    layer.content.assign(
                        reinterpret_cast<const char*>(mapStringBytes.data()),
                        mapStringBytes.size());
                }
                mapStringContentLayer = std::move(layer);
            } else {
                TD_LOG_WARN(
                    "[GameSession] Could not read optional map string file '{}'",
                    mapStringPath);
            }
        }
        auto effectiveRuleset = std::make_shared<MultiplayerRuleset>(
            *domainState().contentState().m_ruleset);
        for (const container::String& modifierPath : gameplayModifierPaths) {
            const container::String modifierContent =
                io::VFS::instance().readAll(modifierPath);
            audioContentLayers.push_back({
                .sourcePath = modifierPath,
                .content = modifierContent,
            });
            mappedImageContentLayers.push_back({
                .sourcePath = modifierPath,
                .content = modifierContent,
            });
            effectiveSimulationContentFingerprint = sessionContentFingerprint(
                effectiveSimulationContentFingerprint, modifierPath,
                modifierContent);

            ObjectSimulationRules stagedRules =
                effectiveObjectSimulationRules;
            container::String modifierError;
            if (!stagedRules.applyLegacyGameDataOverrides(
                    modifierContent, modifierPath, &modifierError) ||
                !stagedRules.applyLegacyAIDataOverrides(
                    modifierContent, modifierPath, &modifierError) ||
                !effectiveRuleset->applyOverridesFromVfs(
                    modifierPath, &modifierError) ||
                !applySessionMapPresentationOverrides(
                    modifierContent, modifierPath,
                    domainState().presentationState().m_renderGameDataSettings,
                    domainState().presentationState().m_trackMarksPresentationSettings,
                    domainState().presentationState().m_scriptWaterPresentationSettings,
                    domainState().presentationState().m_scriptTerrainRoadPresentationSettings,
                    &modifierError)) {
                TD_LOG_ERROR(
                    "[GameSession] Gameplay modifier '{}' rejected: {}",
                    modifierPath, modifierError);
                shutdown();
                return false;
            }
            effectiveObjectSimulationRules = std::move(stagedRules);
        }
        if (!gameplayModifierPaths.empty()) {
            domainState().contentState().m_ruleset =
                std::move(effectiveRuleset);
            domainState().worldState().m_objectSimulation.setRules(
                effectiveObjectSimulationRules);
            domainState().contentState().m_objectSimulationRules =
                domainState().worldState().m_objectSimulation.rules();
        }
        // ZH loads Map.ini then solo.ini before TerrainLogic::loadMap.  The
        // modern session keeps those values private, so refresh every derived
        // presentation value and pass the legacy water extents explicitly
        // before parsing map bytes.  This also prevents map-local GameData
        // from leaking into a later match through process globals.
        domainState().presentationState().m_renderGameDataSettingsSnapshot =
            std::make_shared<const RenderGameDataSettings>(
                domainState().presentationState().m_renderGameDataSettings);
        domainState().worldState().m_visibilityUnlookPersistenceTicks =
            visibilityMillisecondsToTicks(
                domainState().presentationState().m_renderGameDataSettings.
                    visual.visibility.unlookPersistMilliseconds);
        domainState().presentationState().m_scriptSkyboxPresentation.enabled =
            domainState().presentationState().m_renderGameDataSettings.
                visual.water.drawSkyBox;
        if (!domainState().contentState().m_terrain.loadMap(
                startupMapSource,
                domainState().presentationState().m_renderGameDataSettings.
                    visual.water.extentX,
                domainState().presentationState().m_renderGameDataSettings.
                    visual.water.extentY,
                &terrainError)) {
            TD_LOG_ERROR("[GameSession] Failed to parse map '{}': {}",
                         effectiveInfo.mapName, terrainError);
            shutdown();
            return false;
        }
        if (!captureSessionContent(gameplayModifierPaths)) {
            shutdown();
            return false;
        }
        if (!configureSessionVertexWater(
                domainState().contentState().m_terrain, domainState().presentationState().m_renderGameDataSettings.visual.water)) {
            TD_LOG_ERROR(
                "[GameSession] Refusing invalid VertexWater grid for map '{}'",
                domainState().contentState().m_terrain.contentIdentity().resolvedPath);
            shutdown();
            return false;
        }
        installTerrainBridgePathfindSurfaces(
            domainState().contentState().m_terrain, domainState().presentationState().m_scriptTerrainRoadPresentationSettings,
            domainState().contentState().m_contentSnapshot);
        domainState().contentState().m_startInfo = effectiveInfo;
    }
    if (!domainState().contentState().m_contentSnapshot.isCaptured() &&
        !captureSessionContent({})) {
        shutdown();
        return false;
    }
    constexpr container::StringView kEvaIniPath = "data/ini/Eva.ini";
    const container::String evaIniContent =
        io::VFS::instance().exists(container::String{kEvaIniPath})
            ? io::VFS::instance().readAll(container::String{kEvaIniPath})
            : container::String{};
    container::String evaCatalogError;
    if (!domainState().contentState().m_contentSnapshot.freezeEvaEventCatalog(
            evaIniContent, kEvaIniPath, &evaCatalogError)) {
        // EVA is local presentation policy. Missing or malformed mod content
        // disables advisor feedback without invalidating authoritative play.
        TD_LOG_WARN("[GameSession] EVA content degraded: {}", evaCatalogError);
    }
    container::String audioCatalogError;
    if (!domainState().contentState().m_contentSnapshot.
            freezeAudioContentLayers(
                audioContentLayers, &audioCatalogError)) {
        // AudioEvent/AudioSettings are presentation content. A malformed
        // map-local audio block must not reject an otherwise valid game
        // session; retain every successfully applied earlier layer and let
        // missing events resolve as ordinary safe no-ops at playback time.
        TD_LOG_WARN(
            "[GameSession] Map-local audio overrides degraded: {}",
            audioCatalogError);
    }
    container::String mappedImageCatalogError;
    if (!domainState().contentState().m_contentSnapshot.
            freezeMappedImageContentLayers(
                mappedImageContentLayers, &mappedImageCatalogError)) {
        TD_LOG_WARN(
            "[GameSession] Map-local mapped-image overrides degraded: {}",
            mappedImageCatalogError);
    }
    if (mapStringContentLayer) {
        container::String mapStringFreezeError;
        if (!domainState().contentState().m_contentSnapshot.
                freezeMapStringContentLayer(
                    *mapStringContentLayer, &mapStringFreezeError)) {
            TD_LOG_WARN(
                "[GameSession] Map string content freeze degraded: {}",
                mapStringFreezeError);
        }
    }
    if (domainState().contentState().m_terrain.isLoaded()) {
        const navigation::NavigationTerrainAdapterResult navigationResult =
            navigation::NavigationTerrainAdapter::initialize(
                domainState().contentState().m_navigation,
                domainState().contentState().m_terrain.map().heightfield(),
                defaultNavigationTerrainPolicy(),
                &domainState().contentState().m_terrain);
        if (navigationResult.status != navigation::NavigationTerrainAdapterStatus::Success) {
            TD_LOG_ERROR("[GameSession] Failed to initialize navigation: adapter={} system={}",
                         static_cast<uint32_t>(navigationResult.status),
                         static_cast<uint32_t>(navigationResult.systemStatus));
            shutdown();
            return false;
        }
        domainState().contentState().m_navigationFootprintScratch.resize(
            domainState().contentState().m_navigation.grid().cellCount());
    }
    LocalControlContext localControl;
    ResolvedMatchSetup resolvedSetup;
    if (dependencies.resolvedMatchSetup) {
        localControl = localControlFromStartInfo(effectiveInfo);
        resolvedSetup = std::move(*dependencies.resolvedMatchSetup);
        if (domainState().contentState().m_terrain.isLoaded()) {
            hydrateLegacyResolvedMapIdentity(resolvedSetup, info, domainState().contentState().m_terrain.contentIdentity());
        }
        if (!matchesResolvedStartDescriptor(resolvedSetup, effectiveInfo,
                                            dependencies.allowReplayModeOverlay)) {
            TD_LOG_ERROR("[GameSession] Canonical match setup conflicts with its legacy start descriptor");
            shutdown();
            return false;
        }
        container::String terrainError;
        if (domainState().contentState().m_terrain.isLoaded() && !resolvedStartsMatchTerrain(resolvedSetup, domainState().contentState().m_terrain, terrainError)) {
            TD_LOG_ERROR("[GameSession] Canonical match setup is incompatible with '{}': {}",
                         effectiveInfo.mapName, terrainError);
            shutdown();
            return false;
        }
    } else {
        MatchDraft matchDraft = LegacyMatchSetupAdapter::draftFromGameStartInfo(effectiveInfo, localControl);
        if (domainState().contentState().m_terrain.isLoaded()) addTerrainStartLayout(matchDraft, domainState().contentState().m_terrain);
        MatchSetupResolution resolution = MatchSetupResolver::resolve(
            matchDraft, *domainState().contentState().m_ruleset,
            effectiveSimulationContentFingerprint);
        if (!resolution.setup) {
            const container::String playerError = resolution.issues.empty()
                ? "match draft did not resolve"
                : resolution.issues.front().message;
            TD_LOG_ERROR("[GameSession] Failed to resolve match players: {}", playerError);
            shutdown();
            return false;
        }
        resolvedSetup = std::move(*resolution.setup);
    }
    const ResolvedMatchSetupValidation setupValidation = validateResolvedMatchSetup(
        resolvedSetup, domainState().contentState().m_ruleset.get(),
        effectiveSimulationContentFingerprint);
    if (!setupValidation.ok()) {
        TD_LOG_ERROR("[GameSession] Canonical match setup is incompatible with loaded content: {}",
                     setupValidation.issues.front().message);
        shutdown();
        return false;
    }
    const bool hasParticipant = std::any_of(
        resolvedSetup.players.begin(), resolvedSetup.players.end(),
        [](const ResolvedPlayerSetup& player) {
            return player.participation == PlayerParticipationKind::Participant;
        });
    if (!hasParticipant && effectiveInfo.mode != GameMode::Replay) {
        TD_LOG_ERROR("[GameSession] A non-replay session requires at least one simulation participant");
        shutdown();
        return false;
    }
    container::String playerError;
    if (!domainState().contentState().m_players.initialize(resolvedSetup, *domainState().contentState().m_ruleset,
                              effectiveSimulationContentFingerprint,
                              localControl, &playerError)) {
        TD_LOG_ERROR("[GameSession] Failed to resolve match players: {}", playerError);
        shutdown();
        return false;
    }
    domainState().contentState().m_resolvedMatchSetup = std::move(resolvedSetup);
    // RefCode TerrainLogic loads SidesList as part of the map, then
    // PlayerList::newGame materializes its players/teams/diplomacy before
    // ObjectList import and ScriptEngine::newMap. Parse and apply that same
    // immutable source ordering here; map objects must never be created with
    // a temporary lobby-only owner resolver.
    GameSessionScenarioBootstrapService scenarioBootstrap =
        scenarioBootstrapService();
    if (domainState().contentState().m_terrain.isLoaded()) {
        scenarioBootstrap.loadLegacyMapScriptProgram();
        if (!scenarioBootstrap.applyLegacyScenarioDefinition()) {
            TD_LOG_ERROR("[GameSession] Failed to apply legacy SidesList scenario for '{}'", effectiveInfo.mapName);
            shutdown();
            return false;
        }
        // Terrain and legacy script/Scenario import are the only startup
        // consumers of the raw map bytes. Keep typed terrain/script state but
        // do not retain the immutable source blob for the rest of the match.
        domainState().contentState().m_terrain.releaseStartupMapSource();
        startupMapSource.reset();
        if (!domainState().worldState().m_mapVisibility.initialize(domainState().contentState().m_terrain.map(), domainState().contentState().m_players.activePlayerIds())) {
            TD_LOG_ERROR("[GameSession] Failed to initialize terrain map-visibility authority");
            shutdown();
            return false;
        }
        const GameMode canonicalMode = domainState().contentState().m_resolvedMatchSetup
            ? domainState().contentState().m_resolvedMatchSetup->mode : effectiveInfo.mode;
        const bool multiplayerShroudPolicyApplies =
            canonicalMode == GameMode::Skirmish || effectiveInfo.network.enabled;
        domainState().worldState().m_sessionShroudEnabled = !multiplayerShroudPolicyApplies ||
            domainState().contentState().m_ruleset->multiplayer().useShroud;
        static_cast<void>(
            domainState().worldState().m_mapVisibility.setShroudEnabled(domainState().worldState().m_sessionShroudEnabled));
    }
    if (const RankInfoCatalog* ranks =
            domainState().contentState().m_contentSnapshot.rankInfoCatalog()) {
        domainState().contentState().m_players.initializeRankProgression(
            *ranks,
            domainState().presentationState().m_scriptRankLevelLimit);
        const ResolvedMatchSetup* match =
            domainState().contentState().m_resolvedMatchSetup
            ? &*domainState().contentState().m_resolvedMatchSetup : nullptr;
        // RefCode applies the launcher-provided starting rank points only to
        // human players in a fresh single-player game. Replay/network peers
        // obtain progression solely from their canonical command/history.
        if (match && !effectiveInfo.network.enabled &&
            match->mode != GameMode::Replay && match->rankPoints != 0) {
            for (const PlayerId playerId :
                 domainState().contentState().m_players.activePlayerIds()) {
                const PlayerState* player =
                    domainState().contentState().m_players.get(playerId);
                if (!player || player->controller !=
                        PlayerControllerKind::Human) {
                    continue;
                }
                static_cast<void>(
                    domainState().contentState().m_players.addSkillPoints(
                        playerId, match->rankPoints, *ranks,
                        domainState().presentationState().m_scriptRankLevelLimit));
            }
        }
    }

    // Original Object ownership flows through a Team. Create one default live
    // team for every materialized player, register Scenario Team prototypes,
    // and precreate only singleton instances. Map ObjectList import then uses
    // the same lazy TeamFactory::findTeam-equivalent boundary for ordinary
    // prototypes before it creates any entity.
    if (!scenarioBootstrap.initializeObjectTeams()) {
        TD_LOG_ERROR("[GameSession] Failed to initialize live object teams");
        shutdown();
        return false;
    }
    if (!strategicAIService().initialize()) {
        TD_LOG_ERROR("[GameSession] Failed to initialize strategic AI runtime");
        shutdown();
        return false;
    }
    // The importer uses the same object-creation path as normal gameplay, so
    // activate the session after terrain loading but before creating map-owned
    // entities.  It receives only the TerrainMap's detached source values.
    domainState().contentState().m_active = true;
    if (domainState().contentState().m_terrain.isLoaded()) {
        presentationPort().frameCameraOnTerrain();
        domainState().worldState().m_mapObjectSpawnReport = MapObjectImport::import(
            *this, domainState().contentState().m_terrain.map().heightfield());
        const MapObjectSpawnReport& mapReport =
            domainState().worldState().m_mapObjectSpawnReport;
        framePort().noteDegradation(
            FrameDegradation::MissingObjectRecipe,
            mapReport.skippedUnknownTemplateCount);
        framePort().noteDegradation(
            FrameDegradation::MalformedMapObject,
            mapReport.skippedEmptyNameCount);
        framePort().noteDegradation(
            FrameDegradation::MalformedMapObject,
            mapReport.skippedInvalidTransformCount);
        framePort().noteDegradation(
            FrameDegradation::OptionalVisualUnavailable,
            mapReport.clientTerrainVisualFailureCount);
        framePort().noteDegradation(
            FrameDegradation::OptionalVisualUnavailable,
            mapReport.disabledClientDecorationCount);
        framePort().noteDegradation(
            FrameDegradation::ScenarioBindingSkipped,
            mapReport.scenarioTeamResolutionFailureCount);
        framePort().noteDegradation(
            FrameDegradation::ScenarioBindingSkipped,
            mapReport.scriptNameConflictCount);
        framePort().noteDegradation(
            FrameDegradation::ObjectCreationSkipped,
            mapReport.creationFailureCount);
        framePort().noteDegradation(
            FrameDegradation::ObjectCreationSkipped,
            mapReport.terrainBridgeAuthorityFailureCount);
        TD_LOG_INFO("[GameSession] Map objects: source={} spawned={} clientTerrain={} disabledDecor={} "
                    "clientVisualFailures={} bridgeAuthority={}/{} skipped={} waypoint={} roadBridge={} clientMetadata={} "
                    "unknown={} ownerFallback={} scenarioTeams={} scenarioTeamFailures={} "
                    "scriptNames={} scriptNameConflicts={}",
            domainState().worldState().m_mapObjectSpawnReport.sourceRecordCount,
            domainState().worldState().m_mapObjectSpawnReport.spawnedCount,
            domainState().worldState().m_mapObjectSpawnReport.clientTerrainObjectCount,
            domainState().worldState().m_mapObjectSpawnReport.disabledClientDecorationCount,
            domainState().worldState().m_mapObjectSpawnReport.clientTerrainVisualFailureCount,
            domainState().worldState().m_mapObjectSpawnReport.terrainBridgeAuthorityCount,
            domainState().worldState().m_mapObjectSpawnReport.terrainBridgeAuthorityFailureCount,
            domainState().worldState().m_mapObjectSpawnReport.skippedCount(),
            domainState().worldState().m_mapObjectSpawnReport.skippedWaypointCount,
            domainState().worldState().m_mapObjectSpawnReport.skippedRoadOrBridgeCount,
            domainState().worldState().m_mapObjectSpawnReport.skippedClientMetadataCount,
            domainState().worldState().m_mapObjectSpawnReport.skippedUnknownTemplateCount,
            domainState().worldState().m_mapObjectSpawnReport.neutralOwnerFallbackCount,
            domainState().worldState().m_mapObjectSpawnReport.scenarioTeamMemberCount,
            domainState().worldState().m_mapObjectSpawnReport.scenarioTeamResolutionFailureCount,
            domainState().worldState().m_mapObjectSpawnReport.scriptNameBoundCount,
            domainState().worldState().m_mapObjectSpawnReport.scriptNameConflictCount);
        // RefCode places the lobby roster's StartingBuilding/StartingUnit set
        // immediately after the map objects (GameLogic.cpp:2054) and only then
        // runs ThePlayerList->newMap() (:2155), which is what materializes the
        // authored AI BuildLists. Keep that order: a generated con-yard must
        // exist before an authored priority build list starts planning around
        // it.
        scenarioBootstrap.materializeMatchStartingBases();
        scenarioBootstrap.materializeScenarioInitialBuildings();

        // RefCode finishes Pathfinder's initial object registration before
        // GameLogic enters its first update. Leaving this map-import batch in
        // the bounded runtime publisher made every player/script Move wait
        // hundreds of ticks behind a whole-map zone rebuild. Bootstrap is a
        // loading boundary, so publish the closed initial batch now; later
        // construction and destruction remain bounded confirmed-tick work.
        navigation::NavigationSystemStatus startupNavigation =
            domainState().contentState().m_navigation.synchronizeTerrainHeight(
                domainState().contentState().m_terrain);
        if (startupNavigation == navigation::NavigationSystemStatus::Success) {
            startupNavigation = domainState().contentState().m_navigation.
                synchronizeWaterRaster(
                    domainState().contentState().m_terrain);
        }
        if (startupNavigation == navigation::NavigationSystemStatus::Success) {
            startupNavigation =
            domainState().contentState().m_navigation.
                publishStagedStartupDynamicTopology();
        }
        if (startupNavigation != navigation::NavigationSystemStatus::Success) {
            TD_LOG_ERROR(
                "[GameSession] Failed to publish startup object navigation: status={}",
                static_cast<uint32_t>(startupNavigation));
            shutdown();
            return false;
        }
    }

    // Map import creates entities before the first confirmed tick. Build the
    // deterministic broad phase once here so initial UI/script consumers do
    // not need to wait for a no-op simulation frame before world picking.
    domainState().worldState().m_spatialIndex.rebuild(domainState().worldState().m_registry, domainState().worldState().m_objects);
    worldMaintenanceService().updateMapVisibilityLookers(
        domainState().presentationState().m_confirmedTick);

    // Bootstrap uses the normal lifecycle/event machinery before tick one.
    // A structural failure there must reject the candidate immediately. If a
    // faulted session is published as Started, the presentation coordinator
    // correctly refuses to expose its uncommitted endpoint and Loading can
    // only expire at the renderer deadline with no startup ticket.
    if (framePort().result().faulted()) {
        TD_LOG_ERROR(
            "[GameSession] Bootstrap produced an uncommitted simulation fault; session start rejected");
        shutdown();
        return false;
    }

    TD_LOG_INFO("[GameSession] Started: mode={} map={} seed={}",
                static_cast<int>(effectiveInfo.mode), effectiveInfo.mapName, effectiveInfo.seed);
    return true;
}

uint64_t GameSession::rebindResultPresentationEpoch() noexcept {
    if (!domainState().contentState().m_active) return 0;

    const uint64_t epoch = nextPresentationEpoch();
    auto& presentation = domainState().presentationState();
    presentation.m_audioJournal.rebindEpoch(epoch);
    presentation.m_scriptPresentationEpoch = epoch;

    // Candidate Loading may already have retired every renderer/audio
    // consumer belonging to the old epoch. Reissue durable Result values
    // under a fresh process-monotonic identity, while dropping one-shot
    // commands that were already observed before the transition attempt.
    presentation.m_pendingCameraInput.clear();
    presentation.m_localPlacementPresentation.reset(epoch);
    presentation.m_queuedConstructionPlacements.clear();
    presentation.m_rejectedConstructionPlacements.clear();
    presentation.m_fxInvocations.reset(
        epoch,
        static_cast<uint32_t>(domainState().contentState().m_startInfo.seed));
    presentation.m_objectFeedbackOrdinal = 0;
    presentation.m_objectWorldAnimations.clear();
    presentation.m_objectFloatingTexts.clear();
    presentation.m_objectSelectionFlashes.clear();
    presentation.m_commandBackendOutcomes.clear();
    presentation.m_scriptSessionEvents.clear();

    presentation.m_scriptLetterboxPresentation.stamp.presentationEpoch = epoch;
    presentation.m_scriptScreenShakeJournal.clear();
    presentation.m_scriptScreenShakeJournalTrimmedThroughSequence = 0;
    presentation.m_scriptLocalizedCameraShakeJournal.clear();
    presentation.m_scriptLocalizedCameraShakeJournalTrimmedThroughSequence = 0;
    presentation.m_scriptMoveCameraToSelectionRequests.clear();
    presentation.m_scriptCameraPresentationJournal.clear();
    presentation.m_scriptCameraPresentationJournalTrimmedThroughSequence = 0;
    presentation.m_scriptCameraMovementRevision = 0;
    presentation.m_scriptCameraCompletedRevision = 0;
    presentation.m_scriptCameraSlavePresentation.stamp.presentationEpoch = epoch;
    presentation.m_scriptForceObjectSelectionRequests.clear();
    presentation.m_scriptScreenFadePresentation.stamp.presentationEpoch = epoch;
    presentation.m_scriptBlackAndWhitePresentation.stamp.presentationEpoch = epoch;
    presentation.m_scriptBlackAndWhiteJournal.clear();
    presentation.m_scriptBlackAndWhiteJournalTrimmedThroughSequence = 0;
    presentation.m_scriptMotionBlurPresentation.stamp.presentationEpoch = epoch;
    presentation.m_scriptMotionBlurJournal.clear();
    presentation.m_scriptMotionBlurJournalTrimmedThroughSequence = 0;
    presentation.m_scriptSkyboxPresentation.stamp.presentationEpoch = epoch;
    presentation.m_scriptTreeSwayPresentation.stamp.presentationEpoch = epoch;
    presentation.m_scriptWeatherPresentation.stamp.presentationEpoch = epoch;
    presentation.m_scriptInfantryLightingPresentation.stamp.presentationEpoch = epoch;

    presentation.m_scriptUiPresentation.rebindPresentationEpoch(epoch);
    presentation.m_scriptCommandBarOverrides.rebindPresentationEpoch(epoch);
    presentation.m_scriptClientOptions.rebindPresentationEpoch(epoch);
    presentation.m_scriptMapPresentation.rebindPresentationEpoch(epoch);
    presentation.m_scriptObjectPresentation.rebindPresentationEpoch(epoch);
    presentation.m_scriptViewCompatibility.rebindPresentationEpoch(epoch);
    presentation.m_pendingScriptPresentationCompletions.clear();
    presentation.m_pendingVisualAnimationAdmissions.clear();
    presentation.m_pendingVisualAnimationCompletions.clear();
    presentation.m_pendingScriptMusicLoops.clear();
    presentation.m_scriptPresentationCompletions.reset(epoch);

    // Every extraction cache carries or derives the old epoch. Preserve the
    // authoritative sources but force a complete immutable Result world to
    // be rebuilt before the rollback Loading screen is dismissed.
    renderExtractionCache().resetAll();
    domainState().presentationState().m_renderBeaconRadarHistory.clear();
    domainState().presentationState().m_renderBeaconRadarEpoch = 0;
    domainState().objectEventState().m_upgradeRadarHistory.clear();
    domainState().objectEventState().m_upgradeRadarEpoch = 0;
    return epoch;
}

void GameSession::shutdown() {
    const bool hadSessionState = domainState().contentState().m_active || domainState().contentState().m_ruleset || domainState().contentState().m_resolvedMatchSetup || domainState().presentationState().m_legacyMapScriptSource || domainState().contentState().m_contentSnapshot.isCaptured() || domainState().worldState().m_objects.objectCount() != 0 ||
        domainState().aiState().m_objectAI.initialized() ||
        domainState().contentState().m_terrain.isLoaded() || domainState().contentState().m_navigation.isInitialized() ||
        domainState().worldState().m_mapVisibility.isInitialized() || domainState().contentState().m_players.playerCount() != 0 ||
        !domainState().presentationState().m_audioJournal.empty() || domainState().presentationState().m_scriptPresentationEpoch != 0 ||
        domainState().presentationState().m_fxInvocations.presentationEpoch() != 0 || domainState().presentationState().m_fxInvocations.pendingCount() != 0 ||
        domainState().presentationState().m_objectFeedbackOrdinal != 0 ||
        !domainState().presentationState().m_objectWorldAnimations.empty() ||
        !domainState().presentationState().m_objectFloatingTexts.empty() ||
        !domainState().presentationState().m_objectSelectionFlashes.empty() ||
        !domainState().presentationState().m_stealthRadarFeedbackHistory.empty() ||
        !domainState().objectEventState().m_projectileRadiusDecalEvents.empty() ||
        domainState().presentationState().m_scriptGameplayEvents.pendingSpecialPowerCount() != 0 ||
        domainState().presentationState().m_scriptGameplayEvents.pendingUpgradeCount() != 0 ||
        domainState().presentationState().m_scriptGameplayEvents.bridgeTransitionCount() != 0 ||
        !domainState().presentationState().m_scriptSessionEvents.empty() ||
        !domainState().presentationState().m_scriptOrderExecutionRecords.empty() ||
        domainState().presentationState().m_scriptPresentationSequence != 0 || domainState().presentationState().m_scriptLetterboxPresentation.enabled ||
        domainState().presentationState().m_scriptRankLevelLimit != kDefaultScriptRankLevelLimit ||
        !domainState().presentationState().m_scriptScreenShakeJournal.empty() || domainState().presentationState().m_scriptScreenFadePresentation.active ||
        !domainState().presentationState().m_scriptLocalizedCameraShakeJournal.empty() ||
        !domainState().presentationState().m_scriptMoveCameraToSelectionRequests.empty() ||
        !domainState().presentationState().m_scriptCameraPresentationJournal.empty() ||
        domainState().presentationState().m_scriptCameraMovementRevision != 0 ||
        domainState().presentationState().m_scriptCameraCompletedRevision != 0 ||
        domainState().presentationState().m_scriptCameraSlavePresentation.enabled ||
        domainState().presentationState().m_scriptCameraSlavePresentation.stamp.sequence != 0 ||
        !domainState().presentationState().m_scriptForceObjectSelectionRequests.empty() ||
        domainState().presentationState().m_scriptBlackAndWhitePresentation.stamp.sequence != 0 ||
        !domainState().presentationState().m_scriptBlackAndWhiteJournal.empty() ||
        domainState().presentationState().m_scriptMotionBlurPresentation.stamp.sequence != 0 ||
        !domainState().presentationState().m_scriptMotionBlurJournal.empty() ||
        domainState().presentationState().m_scriptSkyboxPresentation.enabled ||
        domainState().presentationState().m_scriptInfantryLightingPresentation.overrideScale.has_value() ||
        !domainState().presentationState().m_pendingScriptPresentationCompletions.empty() ||
        !domainState().presentationState().m_pendingScriptMusicLoops.empty() ||
        domainState().presentationState().m_scriptCommandBarOverrides.lastMutation().sequence != 0 ||
        domainState().frameState().m_open ||
        domainState().frameState().m_result.faulted();
    // Shutdown is deliberately fully idempotent.  A startup may fail before
    // The active flag becomes true (missing content, bad roster or bad map), yet it
    // has already touched transient presentation/session state.  Do not keep
    // an old "already empty" fast path that can leak an epoch or descriptor
    // into a later attempt.
    domainState().worldState().m_objects.reset();
    domainState().aiState().m_objectAI.reset();
    domainState().aiState().m_objectAIPathSequences.clear();
    domainState().aiState().m_objectAIShadowFacts.clear();
    domainState().aiState().m_objectAIShadowNextFacts.clear();
    domainState().aiState().m_objectAIMovementCommands.clear();
    domainState().aiState().m_objectAIMoveCompletions.clear();
    container::Vector<ai::MovementFeedback>{}.swap(
        domainState().aiState().m_movementFeedbackDrainScratch);
    container::Vector<ai::AIFacingFeedback>{}.swap(
        domainState().aiState().m_facingFeedbackDrainScratch);
    domainState().aiState().m_strategicAI.reset();
    domainState().aiState().m_priorityBuildEntries.clear();
    domainState().worldState().m_ownership.reset();
    domainState().worldState().m_objectTeams.reset();
    domainState().worldState().m_objectCombat.reset();
    resetHistoricWeaponLedger(domainState().worldState().m_registry);
    domainState().worldState().m_objectProjectiles.reset();
    domainState().worldState().m_pendingProductionSpawns.clear();
    domainState().worldState().m_pendingProductionUpgrades.clear();
    domainState().worldState().m_objectSimulation.reset();
    domainState().objectEventState().m_upgradeRadarHistory.clear();
    domainState().objectEventState().m_upgradeRadarEpoch = 0;
    domainState().worldState().m_objectSimulation.setSessionSeed(0);
    domainState().contentState().m_objectSimulationRules = {};
    domainState().worldState().m_objectSimulation.setRules(domainState().contentState().m_objectSimulationRules);
    domainState().worldState().m_spatialIndex.clear();
    container::Vector<ObjectId>{}.swap(
        domainState().presentationState().m_scriptSightQueryScratch);
    domainState().worldState().m_modelConditionAuthority = {};
    domainState().contentState().m_contentSnapshot.clear();
    domainState().contentState().m_ruleset.reset();
    domainState().contentState().m_resolvedMatchSetup.reset();
    domainState().contentState().m_simulationRandom.seed(0);
    domainState().contentState().m_players.reset();
    domainState().worldState().m_mapVisibility.clear();
    domainState().worldState().m_sessionShroudEnabled = true;
    domainState().worldState().m_visibilityUnlookPersistenceTicks = 0;
    domainState().worldState().m_visibilityFogTransitionTicks = 0;
    domainState().contentState().m_navigation.reset();
    domainState().contentState().m_navigationFootprintScratch.clear();
    domainState().contentState().m_terrain.clear();
    renderExtractionCache().terrain = {};
    renderExtractionCache().resetWorld();
    domainState().presentationState().m_scriptObjects.clear();
    domainState().presentationState().m_missionState.reset();
    domainState().presentationState().m_scriptMultiplayerVictory = {};
    domainState().presentationState().m_scriptRuntime.setProgram({});
    domainState().presentationState().m_scriptRuntime.setContext({});
    domainState().presentationState().m_legacyMapScriptSource.reset();
    domainState().presentationState().m_legacyMapScriptLoadReport = {};
    domainState().presentationState().m_scenarioDefinition.reset();
    domainState().presentationState().m_cameraDirector.reset();
    domainState().presentationState().m_pendingCameraInput.clear();
    domainState().presentationState().m_localPlacementPresentation.reset();
    domainState().presentationState().m_queuedConstructionPlacements.clear();
    domainState().presentationState().m_rejectedConstructionPlacements.clear();
    domainState().presentationState().m_scriptCamera.reset();
    domainState().presentationState().m_scriptTimeFrozen = false;
    domainState().presentationState().m_audioJournal.reset();
    domainState().presentationState().m_scriptPresentationEpoch = 0;
    domainState().presentationState().m_fxInvocations.reset();
    domainState().presentationState().m_confirmedTick = 0;
    domainState().presentationState().m_hasConfirmedFrame = false;
    domainState().presentationState().m_objectFeedbackOrdinal = 0;
    domainState().presentationState().m_objectWorldAnimations.clear();
    domainState().presentationState().m_objectFloatingTexts.clear();
    domainState().presentationState().m_objectSelectionFlashes.clear();
    domainState().presentationState().m_commandBackendOutcomes.clear();
    domainState().presentationState().m_objectAmbientAudio.clear();
    domainState().presentationState().m_hackInternetAudio.clear();
    domainState().presentationState().m_objectLossRadarEvents.clear();
    domainState().presentationState().m_stealthRadarFeedbackHistory.clear();
    domainState().presentationState().m_underAttackRadarFeedbackHistory.clear();
    domainState().presentationState().m_evaPolledRetryTicks = {};
    domainState().presentationState().m_evaObservedRankLevel = 0;
    domainState().presentationState().m_pendingEvaAnnouncements.clear();
    domainState().objectEventState().m_projectileRadiusDecalEvents.clear();
    domainState().presentationState().m_scriptGameplayEvents.reset();
    domainState().presentationState().m_scriptSessionEvents.clear();
    domainState().presentationState().m_scriptOrderExecutionRecords.clear();
    domainState().presentationState().m_scriptPresentationSequence = 0;
    domainState().presentationState().m_scriptLetterboxPresentation = {};
    domainState().presentationState().m_scriptScreenShakeJournal.clear();
    domainState().presentationState().m_scriptScreenShakeJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_scriptLocalizedCameraShakeJournal.clear();
    domainState().presentationState().m_scriptLocalizedCameraShakeJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_scriptMoveCameraToSelectionRequests.clear();
    domainState().presentationState().m_scriptCameraPresentationJournal.clear();
    domainState().presentationState().m_scriptCameraPresentationJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_scriptCameraMovementRevision = 0;
    domainState().presentationState().m_scriptCameraCompletedRevision = 0;
    domainState().presentationState().m_scriptCameraSlavePresentation = {};
    domainState().presentationState().m_scriptForceObjectSelectionRequests.clear();
    domainState().presentationState().m_scriptScreenFadePresentation = {};
    domainState().presentationState().m_scriptBlackAndWhitePresentation = {};
    domainState().presentationState().m_scriptBlackAndWhiteJournal.clear();
    domainState().presentationState().m_scriptBlackAndWhiteJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_scriptMotionBlurPresentation = {};
    domainState().presentationState().m_scriptMotionBlurJournal.clear();
    domainState().presentationState().m_scriptMotionBlurJournalTrimmedThroughSequence = 0;
    domainState().presentationState().m_scriptSkyboxPresentation = {};
    domainState().presentationState().m_scriptSkyboxPresentationTextures = {};
    domainState().presentationState().m_scriptWaterPresentationSettings = {};
    domainState().presentationState().m_scriptTerrainRoadPresentationSettings = {};
    domainState().presentationState().m_trackMarksPresentationSettings = {};
    domainState().presentationState().m_renderGameDataSettings = {};
    domainState().presentationState().m_renderGameDataSettingsSnapshot.reset();
    domainState().presentationState().m_renderFeatureQualitySnapshot.reset();
    domainState().presentationState().m_initialRenderDisplaySnapshot = {};
    domainState().presentationState().m_scriptTreeSwayPresentation = {};
    domainState().presentationState().m_scriptWeatherPresentation = {};
    domainState().presentationState().m_scriptInfantryLightingPresentation = {};
    domainState().presentationState().m_scriptUiPresentation.reset();
    domainState().presentationState().m_scriptCommandBarOverrides.reset();
    domainState().presentationState().m_scriptObjectBuildabilityOverrides.clear();
    domainState().presentationState().m_scriptAttackPrioritySets.clear();
    domainState().presentationState().m_scriptAttackPriorityById.clear();
    domainState().presentationState().m_scriptAttackPrioritySequence = 0;
    domainState().presentationState().m_objectsReceiveDifficultyBonuses = true;
    domainState().presentationState().m_chooseVictimAlwaysNormal = false;
    domainState().presentationState().m_scriptToppleDirections.clear();
    domainState().presentationState().m_scriptRankLevelLimit = kDefaultScriptRankLevelLimit;
    domainState().presentationState().m_scoreAccumulationEnabled = true;
    domainState().contentState().m_players.setScoreAccumulationEnabled(true);
    domainState().presentationState().m_scriptClientOptions.reset();
    domainState().presentationState().m_scriptMapPresentation.reset();
    domainState().presentationState().m_scriptObjectPresentation.reset();
    domainState().presentationState().m_scriptViewCompatibility.reset();
    domainState().presentationState().m_pendingScriptPresentationCompletions.clear();
    domainState().presentationState().m_pendingVisualAnimationAdmissions.clear();
    domainState().presentationState().m_pendingVisualAnimationCompletions.clear();
    domainState().presentationState().m_pendingScriptMusicLoops.clear();
    domainState().presentationState().m_scriptPresentationCompletions.reset();
    domainState().worldState().m_mapObjectSpawnReport = {};
    domainState().worldState().m_clientTerrainObjects.clear();
    domainState().objectEventState().m_frameLifecycleEvents.clear();
    domainState().objectEventState().m_frameHealthEvents.clear();
    container::Vector<ObjectHealthEvent>{}.swap(
        domainState().objectEventState().m_healthDrainScratch);
    domainState().objectEventState().m_teamUnitDestroyedHookEvents.clear();
    domainState().objectEventState().m_objectHookEvents.clear();
    // The frame transaction aggregate is retired here like every other one.
    // raiseSimulationFault() with no open frame latches Faulted directly into
    // m_result/m_pendingFault, and nothing else clears it: a reused instance
    // would either fail its own bootstrap fault gate in start() or fault the
    // first begin() of an otherwise clean session.
    domainState().frameState().m_result = {};
    domainState().frameState().m_pendingDegradationMask = 0;
    domainState().frameState().m_pendingDegradationCount = 0;
    domainState().frameState().m_pendingFault = {};
    domainState().frameState().m_pendingAdditionalFaultCount = 0;
    domainState().frameState().m_open = false;
    domainState().contentState().m_startInfo.reset();
    domainState().contentState().m_active = false;

    if (hadSessionState) TD_LOG_INFO("[GameSession] Shutdown");
}
} // namespace engine
