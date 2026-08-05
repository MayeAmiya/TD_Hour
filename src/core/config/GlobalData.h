#pragma once

#include "container/container_types.h"

#include <cstdint>
namespace config {

// Time of day enum
enum class TimeOfDay : int {
    Morning = 0,
    Afternoon,
    Evening,
    Night,
    Count
};

// Weather enum
enum class Weather : int {
    Clear = 0,
    Snowy,
    Rainy,
    Count
};

// Terrain LOD enum
enum class TerrainLOD : int {
    Automatic = 0,
    Low,
    Medium,
    High,
    VeryHigh,
    Count
};

// Static game LOD levels
enum class StaticGameLODLevel : int {
    Unknown = -1,
    Low = 0,
    Medium,
    High,
    VeryHigh,
    Custom,
    Count = 5
};

// Terrain lighting data for a single light
struct TerrainLighting {
    float ambient[3] = {0.0f, 0.0f, 0.0f};
    float diffuse[3] = {0.0f, 0.0f, 0.0f};
    float lightPos[3] = {0.0f, 0.0f, 0.0f};
};

// Forward declarations
class CommandLine;

// Global data container class
class GlobalData {
public:
    struct MapSearchPath {
        container::String vfsPath;
        bool multiplayer = true;
    };

    struct LocalMapPath {
        container::String sourcePath;
        container::String vfsRoot;
        bool multiplayer = true;
    };

    GlobalData();
    ~GlobalData() = default;

    // Load from INI file (must be called first, before other systems)
    void loadFromIni(const container::String& filename);

    // Set time of day
    bool setTimeOfDay(TimeOfDay tod);

    // Accessors
    TimeOfDay getTimeOfDay() const { return m_timeOfDay; }
    Weather getWeather() const { return m_weather; }
    StaticGameLODLevel getStaticLOD() const { return m_staticLOD; }
    float getAllowedHeightVariationForBuilding() const noexcept {
        return m_allowedHeightVariationForBuilding;
    }

    // Data paths (loaded from GameOptions.ini)
    const container::String& getGeneralsDataPath() const {
        return m_generalsDataPath;
    }
    const container::String& getZeroHourDataPath() const {
        return m_zeroHourDataPath;
    }
    const container::String& getModDataPath() const { return m_modDataPath; }
    const container::String& getLocaleDataPath() const { return m_localeDataPath; }
    const container::String& getUserDataPath() const { return m_userDataPath; }
    const container::String& getSaveDataPath() const { return m_saveDataPath; }
    const container::String& getReplayDataPath() const { return m_replayDataPath; }
    // Absolute path of the GameOptions.ini actually adopted for this process.
    // Direct launch from Bin/<config> must keep all later consumers (fonts,
    // preferences) on the same configuration source as VFS setup.
    const container::String& getLoadedConfigPath() const {
        return m_loadedConfigPath;
    }
    const container::Vector<MapSearchPath>& getMapSearchPaths() const { return m_mapSearchPaths; }
    const container::Vector<LocalMapPath>& getLocalMapPaths() const { return m_localMapPaths; }

    // Display settings
    bool isWindowed() const { return m_windowed; }
    int getXResolution() const { return m_xResolution; }
    int getYResolution() const { return m_yResolution; }
    // Client frame pacing defaults. Map scripts resolve SET_FPS_LIMIT=0 to
    // this immutable-at-session-start policy instead of writing GlobalData.
    bool useFpsLimit() const { return m_useFpsLimit; }
    int framesPerSecondLimit() const { return m_framesPerSecondLimit; }

    // Audio settings
    bool isAudioOn() const { return m_audioOn; }
    bool isMusicOn() const { return m_musicOn; }
    bool isSoundsOn() const { return m_soundsOn; }

    // Game balance
    int getBaseValuePerSupplyBox() const { return m_baseValuePerSupplyBox; }
    float getBuildSpeed() const { return m_buildSpeed; }
    float getRefundPercent() const { return m_refundPercent; }
    // Kept as plain configuration values; terrain loading receives them by
    // explicit context rather than reaching through the global singleton.
    float getWaterExtentX() const { return m_waterExtentX; }
    float getWaterExtentY() const { return m_waterExtentY; }
    // This is captured into a terrain render snapshot.  Rendering must never
    // read GlobalData directly after the logic frame has been sealed.
    bool isAdjustCliffTextures() const { return m_adjustCliffTextures; }
    // Captured when an object visual is instantiated.  This is the modern
    // counterpart to RefCode skipping SwayClientUpdate construction when the
    // client option is disabled; render command recording never reads the
    // mutable GlobalData singleton.
    bool useTreeSway() const { return m_useTreeSway; }
    // W3DWater creates `new_skybox` with these values, then centers it on the
    // tactical camera in XY. They are copied into the world snapshot; a
    // renderer must not consult mutable GlobalData while recording commands.
    bool isDrawSkyBoxEnabled() const { return m_drawSkyBox != 0.0f; }
    float skyBoxPositionZ() const { return m_skyBoxPositionZ; }
    float skyBoxScale() const { return m_skyBoxScale; }
    // Partition sizing is simulation/map configuration.  Consumers capture
    // this scalar at their own stable boundary instead of reaching through
    // GlobalData's implementation fields.
    float partitionCellSize() const { return m_partitionCellSize; }

    // Command line data
    struct CommandLineData {
        bool hasParsedForStartup = false;
        bool hasParsedForEngineInit = false;
    };

    CommandLineData m_commandLineData;

private:
    // Display/Window
    bool m_windowed = false;
    // Zero means no legacy GlobalData override. Output dimensions belong to
    // the renderer/display contract and are observed dynamically.
    int m_xResolution = 0;
    int m_yResolution = 0;
    int m_chipSetType = 0;
    float m_viewportHeightScale = 0.8f;
    bool m_headless = false;

    // Terrain rendering
    bool m_useTrees = true;
    bool m_useTreeSway = true;
    bool m_useHeatEffects = true;
    bool m_useFpsLimit = true;
    // Presentation pacing is independent from GameStartInfo::gameSpeedFPS.
    // The simulation keeps its authored 30 Hz while a default 120 Hz client
    // can submit three GPU-interpolated frames between confirmed endpoints.
    int m_framesPerSecondLimit = 120;
    bool m_useCloudMap = true;
    bool m_useLightMap = true;
    bool m_bilinearTerrainTex = false;
    bool m_trilinearTerrainTex = true;
    bool m_multiPassTerrain = true;
    bool m_adjustCliffTextures = true;
    bool m_stretchTerrain = false;
    bool m_useHalfHeightMap = false;
    bool m_drawEntireTerrain = false;
    TerrainLOD m_terrainLOD = TerrainLOD::Automatic;
    bool m_enableDynamicLOD = true;
    bool m_enableStaticLOD = true;
    int m_terrainLODTargetTimeMS = 5000;

    // Mouse/Controls
    bool m_useAlternateMouse = false;
    bool m_useRightMouseScrollWithAlternateMouse = false;
    bool m_clientRetaliationModeEnabled = true;
    bool m_doubleClickAttackMove = false;
    bool m_rightMouseAlwaysScrolls = false;

    // Water
    bool m_useWaterPlane = true;
    bool m_useCloudPlane = true;
    bool m_useShadowVolumes = false;
    bool m_useShadowDecals = false;
    int m_textureReductionFactor = -1;
    bool m_enableBehindBuildingMarkers = true;
    float m_waterPositionX = 0.0f;
    float m_waterPositionY = 0.0f;
    float m_waterPositionZ = 0.0f;
    float m_waterExtentX = 1000.0f;
    float m_waterExtentY = 1000.0f;
    int m_waterType = 0;
    bool m_showSoftWaterEdge = true;
    bool m_usingWaterTrackEditor = false;
    bool m_isWorldBuilder = false;
    int m_featherWater = 0;

    // Vertex water settings (4 grid settings)
    static constexpr int MAX_WATER_GRID_SETTINGS = 4;
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterHeightClampLow = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterHeightClampHi = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterAngle = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterXPosition = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterYPosition = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterZPosition = {};
    container::Array<int, MAX_WATER_GRID_SETTINGS> m_vertexWaterXGridCells = {};
    container::Array<int, MAX_WATER_GRID_SETTINGS> m_vertexWaterYGridCells = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterGridSize = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterAttenuationA = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterAttenuationB = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterAttenuationC = {};
    container::Array<float, MAX_WATER_GRID_SETTINGS> m_vertexWaterAttenuationRange = {};

    // Sky
    float m_downwindAngle = 0.0f;
    float m_skyBoxPositionZ = 0.0f;
    // RefCode defaults to an inactive skybox until a map/script enables it,
    // and builds the `new_skybox` render object at scale 4.5.
    float m_drawSkyBox = 0.0f;
    float m_skyBoxScale = 4.5f;

    // Camera
    float m_cameraPitch = 0.0f;
    float m_cameraYaw = 0.0f;
    float m_maxCameraHeight = 300.0f;
    float m_minCameraHeight = 100.0f;
    float m_terrainHeightAtEdgeOfMap = 0.0f;
    float m_unitDamagedThresh = 0.5f;
    float m_unitReallyDamagedThresh = 0.1f;
    float m_groundStiffness = 0.5f;
    float m_structureStiffness = 1.0f;
    float m_gravity = -1.0f;
    float m_stealthFriendlyOpacity = 0.5f;
    uint32_t m_defaultOcclusionDelay = 1000;

    // Preload
    bool m_preloadAssets = true;
    bool m_preloadEverything = false;
    bool m_preloadReport = false;

    // Partition
    float m_partitionCellSize = 100.0f;

    // Ammo/Container pips
    float m_ammoPipWorldOffset[3] = {0.0f, 0.0f, 0.0f};
    float m_containerPipWorldOffset[3] = {0.0f, 0.0f, 0.0f};
    float m_ammoPipScreenOffset[2] = {0.0f, 0.0f};
    float m_containerPipScreenOffset[2] = {0.0f, 0.0f};
    float m_ammoPipScaleFactor = 1.0f;
    float m_containerPipScaleFactor = 1.0f;

    // Tracks
    uint32_t m_historicDamageLimit = 1000;
    int m_maxTerrainTracks = 200;
    int m_maxTankTrackEdges = 100;
    int m_maxTankTrackOpaqueEdges = 50;
    int m_maxTankTrackFadeDelay = 10;

    // Animations
    container::String m_levelGainAnimationName;
    float m_levelGainAnimationDisplayTimeInSeconds = 2.0f;
    float m_levelGainAnimationZRisePerSecond = 10.0f;
    container::String m_getHealedAnimationName;
    float m_getHealedAnimationDisplayTimeInSeconds = 2.0f;
    float m_getHealedAnimationZRisePerSecond = 10.0f;

    // Time/Weather
    TimeOfDay m_timeOfDay = TimeOfDay::Morning;
    Weather m_weather = Weather::Clear;
    bool m_makeTrackMarks = true;
    bool m_hideGarrisonFlags = false;
    bool m_forceModelsToFollowTimeOfDay = false;
    bool m_forceModelsToFollowWeather = false;

    // Terrain lighting (4 times of day × 3 lights)
    static constexpr int MAX_GLOBAL_LIGHTS = 3;
    container::Array<container::Array<TerrainLighting, MAX_GLOBAL_LIGHTS>, static_cast<size_t>(TimeOfDay::Count)> m_terrainLighting = {};
    container::Array<container::Array<TerrainLighting, MAX_GLOBAL_LIGHTS>, static_cast<size_t>(TimeOfDay::Count)> m_terrainObjectsLighting = {};

    // Global light settings
    float m_terrainAmbient[MAX_GLOBAL_LIGHTS][3] = {};
    float m_terrainDiffuse[MAX_GLOBAL_LIGHTS][3] = {};
    float m_terrainLightPos[MAX_GLOBAL_LIGHTS][3] = {};
    float m_infantryLightScale[static_cast<size_t>(TimeOfDay::Count)] = {};
    float m_scriptOverrideInfantryLightScale = 0.0f;

    // Solo player health bonus
    static constexpr int PLAYERTYPE_COUNT = 2;
    static constexpr int DIFFICULTY_COUNT = 3;
    float m_soloPlayerHealthBonusForDifficulty[PLAYERTYPE_COUNT][DIFFICULTY_COUNT] = {};

    // Occlusion
    int m_maxVisibleTranslucentObjects = 100;
    int m_maxVisibleOccluderObjects = 100;
    int m_maxVisibleOccludeeObjects = 100;
    int m_maxVisibleNonOccluderOrOccludeeObjects = 100;
    float m_occludedLuminanceScale = 0.5f;

    // Roads
    int m_numGlobalLights = 3;
    int m_maxRoadSegments = 1000;
    int m_maxRoadVertex = 10000;
    int m_maxRoadIndex = 10000;
    int m_maxRoadTypes = 10;

    // Audio
    bool m_audioOn = true;
    bool m_musicOn = true;
    bool m_soundsOn = true;
    bool m_sounds3DOn = true;
    bool m_speechOn = true;
    bool m_videoOn = true;
    bool m_disableCameraMovement = false;

    // Debug/Rendering
    bool m_useFX = true;
    bool m_showClientPhysics = false;
    bool m_showTerrainNormals = false;
    uint32_t m_noDraw = 0;
    int m_debugAI = 0;
    bool m_debugSupplyCenterPlacement = false;
    bool m_debugAIObstacles = false;
    bool m_showObjectHealth = false;
    bool m_scriptDebug = false;
    bool m_particleEdit = false;
    bool m_displayDebug = false;
    bool m_winCursors = false;
    bool m_constantDebugUpdate = false;
    bool m_showTeamDot = true;
    bool m_forceBenchmark = false;

    // Game balance
    int m_fixedSeed = -1;
    float m_particleScale = 1.0f;
    int m_baseValuePerSupplyBox = 100;
    float m_buildSpeed = 1.0f;
    float m_minDistFromEdgeOfMapForBuild = 100.0f;
    float m_supplyBuildBorder = 50.0f;
    float m_allowedHeightVariationForBuilding = 10.0f;
    float m_minLowEnergyProductionSpeed = 0.1f;
    float m_maxLowEnergyProductionSpeed = 1.0f;
    float m_lowEnergyPenaltyModifier = 0.5f;
    float m_multipleFactory = 1.0f;
    float m_refundPercent = 1.0f;

    // Command center
    float m_commandCenterHealRange = 100.0f;
    float m_commandCenterHealAmount = 1.0f;
    // Original shipped GameData default. Authoritative sessions freeze the
    // typed BuildPlacementSimulationRules copy instead of reading this
    // compatibility singleton during a confirmed tick.
    int m_maxLineBuildObjects = 50;
    int m_maxTunnelCapacity = 10;

    // Scrolling
    float m_horizontalScrollSpeedFactor = 1.0f;
    float m_verticalScrollSpeedFactor = 1.0f;
    float m_scrollAmountCutoff = 10.0f;
    float m_cameraAdjustSpeed = 0.1f;
    bool m_enforceMaxCameraHeight = true;
    bool m_buildMapCache = false;

    // Map
    container::String m_initialFile;
    container::String m_pendingFile;

    // Data paths (loaded from GameOptions.ini)
    container::String m_generalsDataPath;
    container::String m_zeroHourDataPath;
    container::String m_modDataPath;
    container::String m_localeDataPath;
    container::String m_userDataPath;
    container::String m_saveDataPath;
    container::String m_replayDataPath;
    container::String m_loadedConfigPath;
    container::Vector<MapSearchPath> m_mapSearchPaths;
    container::Vector<LocalMapPath> m_localMapPaths;
    bool m_playIntro = true;
    bool m_playSizzle = true;
    bool m_afterIntro = false;
    bool m_allowExitOutOfMovies = true;
    bool m_loadScreenRender = false;

    // Keyboard scrolling
    float m_keyboardScrollFactor = 0.5f;
    float m_keyboardDefaultScrollFactor = 0.5f;
    bool m_drawScrollAnchor = false;
    bool m_moveScrollAnchor = true;

    // Windows
    bool m_animateWindows = true;
    bool m_incrementalAGPBuf = false;

    // CRC
    uint32_t m_iniCRC = 0;
    uint32_t m_exeCRC = 0;

    // Movement penalty
    int m_movementPenaltyDamageState = 2;

    // Group select
    int m_groupSelectMinSelectSize = 1;
    float m_groupSelectVolumeBase = 0.5f;
    float m_groupSelectVolumeIncrement = 0.1f;
    int m_maxUnitSelectSounds = 3;

    // Selection flash
    float m_selectionFlashSaturationFactor = 1.0f;
    bool m_selectionFlashHouseColor = false;

    // Camera audio
    float m_cameraAudibleRadius = 500.0f;
    float m_groupMoveClickToGatherFactor = 1.0f;

    // Network
    int m_netMinPlayers = 2;

    // Auto fire/smoke particles (simplified)
    container::String m_autoFireParticleSmallPrefix;
    container::String m_autoFireParticleSmallSystem;
    int m_autoFireParticleSmallMax = 10;
    container::String m_autoFireParticleMediumPrefix;
    container::String m_autoFireParticleMediumSystem;
    int m_autoFireParticleMediumMax = 20;
    container::String m_autoFireParticleLargePrefix;
    container::String m_autoFireParticleLargeSystem;
    int m_autoFireParticleLargeMax = 30;
    container::String m_autoSmokeParticleSmallPrefix;
    container::String m_autoSmokeParticleSmallSystem;
    int m_autoSmokeParticleSmallMax = 10;
    container::String m_autoSmokeParticleMediumPrefix;
    container::String m_autoSmokeParticleMediumSystem;
    int m_autoSmokeParticleMediumMax = 20;
    container::String m_autoSmokeParticleLargePrefix;
    container::String m_autoSmokeParticleLargeSystem;
    int m_autoSmokeParticleLargeMax = 30;
    container::String m_autoAflameParticlePrefix;
    container::String m_autoAflameParticleSystem;
    int m_autoAflameParticleMax = 10;

    // Particles
    int m_maxParticleCount = 500;
    int m_maxFieldParticleCount = 30;

    // Health bonus
    static constexpr int LEVEL_COUNT = 4;
    float m_healthBonus[LEVEL_COUNT] = {};
    float m_defaultStructureRubbleHeight = 10.0f;

    // Replay
    int m_simulateReplayJobs = -1;

    // LOD
    StaticGameLODLevel m_staticLOD = StaticGameLODLevel::High;
};

// Global singleton
extern GlobalData* TheWritableGlobalData;
extern const GlobalData& TheGlobalData;

} // namespace config
