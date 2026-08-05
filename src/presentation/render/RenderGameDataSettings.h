#pragma once

#include "core/container/container_types.h"
#include "GroundDecalPerformanceSettings.h"
#include "PresentationDefaults.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace config {
class GraphPreferences;
}

namespace engine {

// Stable names used by GameLOD.ini and Options.ini.
enum class RenderStaticLod : uint8_t {
    Low,
    Medium,
    High,
    VeryHigh,
    Custom,
    Count,
};

[[nodiscard]] constexpr bool allowsDrawModuleAtStaticLod(
    RenderStaticLod selectedLod, uint8_t minimumRequiredLod,
    bool useDrawModuleLod) noexcept {
    // GeneralsTD does not remove authored Draw modules by a quality/LOD
    // setting.  Keep the legacy arguments in the content contract, but make
    // the runtime admission rule unconditional.
    static_cast<void>(selectedLod);
    static_cast<void>(minimumRequiredLod);
    static_cast<void>(useDrawModuleLod);
    return true;
}

enum class RenderTimeOfDay : uint8_t {
    Morning,
    Afternoon,
    Evening,
    Night,
};

enum class RenderWeather : uint8_t {
    Normal,
    Snowy,
};

enum class RenderDynamicLod : uint8_t {
    Low,
    Medium,
    High,
    VeryHigh,
    Count,
};

enum class RenderParticlePriority : uint8_t {
    None,
    WeaponExplosion,
    ScorchMark,
    DustTrail,
    BuildUp,
    DebrisTrail,
    UnitDamageFx,
    DeathExplosion,
    SemiConstant,
    Constant,
    WeaponTrail,
    AreaEffect,
    Critical,
    AlwaysRender,
    Count,
};

enum class RenderTerrainLod : uint8_t {
    Automatic,
    Low,
    Medium,
    High,
    VeryHigh,
};

// Legacy GameLOD.ini/Options.ini values are still parsed so existing content
// remains readable, but they may not discard authored presentation.  W3D HLOD
// containers are likewise still decoded because they are part of the file
// format; the model loader always materializes their highest-detail member.
// Capacity ceilings remain independent memory-safety limits, not LOD.
namespace render_lod_policy {
inline constexpr RenderStaticLod kStaticProfile = RenderStaticLod::VeryHigh;
inline constexpr RenderDynamicLod kDynamicProfile = RenderDynamicLod::VeryHigh;
inline constexpr RenderTerrainLod kTerrainProfile = RenderTerrainLod::VeryHigh;
inline constexpr bool kDynamicLodEnabled = false;
inline constexpr bool kDrawModuleLodEnabled = false;
inline constexpr RenderParticlePriority kMinimumParticlePriority =
    RenderParticlePriority::None;
inline constexpr RenderParticlePriority kMinimumParticleSkipPriority =
    RenderParticlePriority::None;
inline constexpr uint32_t kParticleSkipMask = 0;
} // namespace render_lod_policy

// Final-image AA is independent from the legacy Options.ini MSAA value.
enum class RenderAntiAliasingMode : uint8_t { Off, Fxaa };
enum class RenderDisplayMode : uint8_t {
    Windowed,
    BorderlessFullscreen,
    ExclusiveFullscreen,
};
enum class RenderParticleSimulationBackend : uint8_t { Cpu, GpuCompute };

struct RenderRgbColor final {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

struct RenderVector3 final {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct RenderLightDescriptor final {
    RenderRgbColor ambient;
    RenderRgbColor diffuse;
    RenderVector3 position;
};

struct RenderCameraGameData final {
    float pitchDegrees = 37.5f;
    float yawDegrees = 0.0f;
    float initialHeight = 232.0f;
    float minimumHeight = 120.0f;
    float maximumHeight = 310.0f;
    float adjustSpeed = 0.3f;
    float scrollAmountCutoff = 50.0f;
    float horizontalScrollSpeedFactor = 1.6f;
    float verticalScrollSpeedFactor = 2.0f;
    float keyboardScrollSpeedFactor = 2.0f;
    float keyboardDefaultScrollSpeedFactor = 0.5f;
    float keyboardRotateSpeed = 0.1f;
    float horizontalFieldOfViewDegrees = 50.0f;
    float nearClipDistance = 10.0f;
    float viewportHeightScale = 0.8f;
    bool enforceMaximumHeight = false;
};

struct RenderInputGameData final {
    bool useAlternateMouse = false;
    bool useRightMouseScrollWithAlternateMouse = false;
    bool rightMouseAlwaysScrolls = true;
    bool doubleClickAttackMove = false;
    bool drawScrollAnchor = false;
    bool moveScrollAnchor = true;
    bool screenEdgeScrollWindowed = false;
    bool screenEdgeScrollFullscreen = true;
    bool cursorCaptureWindowedGame = true;
    bool cursorCaptureFullscreenGame = true;
    uint32_t screenEdgeWidthPixels = 3;
    uint32_t dragTolerancePixels = 25;
    uint32_t dragToleranceWorldUnits = 25;
    uint32_t dragToleranceMilliseconds = 250;
};

struct RenderShadowGameData final {
    bool useVolumes = true;
    bool useDecals = true;
};

struct RenderTerrainGameData final {
    bool adjustCliffTextures = true;
    bool bilinearTextures = true;
    bool trilinearTextures = true;
    bool useCloudMap = true;
    bool useLightMap = true;
    bool showTrees = true;
    bool useTreeSway = true;
    bool useBuildupScaffolds = true;
    bool extraAnimations = true;
    bool useDrawModuleLod = false;
    bool useHeatEffects = true;
    bool multiPass = true;
    bool threeWayBlend = false;
    bool stretch = false;
    bool useHalfHeightMap = false;
    bool drawEntireTerrain = false;
    RenderTerrainLod lod = render_lod_policy::kTerrainProfile;
};

struct RenderVertexWaterGameData final {
    container::String availableMaps;
    float heightClampLow = 0.0f;
    float heightClampHigh = 0.0f;
    float angleRadians = 0.0f;
    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 0.0f;
    uint32_t gridCellsX = 0;
    uint32_t gridCellsY = 0;
    float gridSize = 0.0f;
    float attenuationA = 0.0f;
    float attenuationB = 0.0f;
    float attenuationC = 0.0f;
    float attenuationRange = 0.0f;
};

struct RenderWaterGameData final {
    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 7.0f;
    float extentX = 2000.0f;
    float extentY = 2000.0f;
    int32_t waterType = 0;
    bool useWaterPlane = true;
    bool useCloudPlane = true;
    bool showSoftEdge = true;
    bool drawSkyBox = true;
    float skyBoxPositionZ = -100.0f;
    float skyBoxScale = 8.4f;
    container::Array<RenderVertexWaterGameData, 4> vertexWater;
};

struct RenderVisibilityGameData final {
    uint32_t unlookPersistMilliseconds = 5000;
    uint32_t defaultOcclusionDelayMilliseconds = 3000;
    uint8_t clearAlpha = 255;
    uint8_t fogAlpha = 127;
    uint8_t shroudAlpha = 0;
    RenderRgbColor shroudColor;
    float occludedLuminanceScale = 0.5f;
    bool behindBuildingMarkers = true;
};

struct RenderObjectFeedbackGameData final {
    bool showObjectHealth = true;
    float selectionFlashSaturationFactor = 0.5f;
    bool selectionFlashHouseColor = false;
    // InGameUI owns the default style; Language.DrawableCaptionFont is
    // applied afterwards and may replace this complete font descriptor.
    container::String drawableCaptionFont = "Arial";
    int32_t drawableCaptionPointSize = 10;
    bool drawableCaptionBold = false;
    uint32_t drawableCaptionColor = 0xffffffffu;
    // RefCode GlobalData values used by InGameUI::placeBuildAvailable().
    // They are authored presentation behaviour, not renderer work budgets.
    float objectPlacementOpacity = 0.45f;
    bool objectPlacementShadows = true;
    // GlobalData/InGameUI/MiscAudio jointly own object-addressed feedback in
    // RefCode. Freeze those authored values into the session presentation
    // descriptor so a confirmed consumer never reaches back into VFS or a
    // process-global configuration object.
    container::String levelGainAnimationName;
    float levelGainAnimationDisplaySeconds = 0.0f;
    float levelGainAnimationZRisePerSecond = 0.0f;
    uint32_t floatingTextTimeoutMilliseconds = 333u;
    float floatingTextMoveUpPerSecond = 30.0f;
    float floatingTextVanishPerSecond = 3.0f;
    container::String unitPromotedAudioEvent = "UnitPromoted";
    container::String stealthDiscoveredAudioEvent =
        "StealthDiscoveredSound";
    container::String stealthNeutralizedAudioEvent =
        "StealthNeutralizedSound";
    container::String radarInfiltrationAudioEvent = "NoSound";
    // Radar "under attack" family. RefCode Radar::tryUnderAttackEvent picks
    // one of these by the victim's KindOf after the 250-unit / 10-second
    // event dedup admits a new event; the generic key covers everything that
    // is neither infantry/vehicle nor a victory-counting structure.
    container::String radarUnitUnderAttackAudioEvent =
        "RadarNotifyUnitUnderAttack";
    container::String radarHarvesterUnderAttackAudioEvent =
        "RadarNotifyHarvesterUnderAttack";
    container::String radarStructureUnderAttackAudioEvent =
        "RadarNotifyStructureUnderAttack";
    container::String radarUnderAttackAudioEvent = "RadarNotifyUnderAttack";
    container::String moneyWithdrawAudioEvent = "MoneyWithdraw";
    container::String sabotageShutdownAudioEvent = "SabotageBuildingPower";
    container::String sabotageResetTimerAudioEvent = "SabotageBuilding";
    container::String defectorTimerTickAudioEvent = "DefectorTimerTick";
};

struct RenderLightingGameData final {
    static constexpr size_t kTimeOfDayCount = 4;
    static constexpr size_t kGlobalLightCount = 3;
    container::Array<container::Array<RenderLightDescriptor, kGlobalLightCount>,
                     kTimeOfDayCount>
        terrain;
    container::Array<container::Array<RenderLightDescriptor, kGlobalLightCount>,
                     kTimeOfDayCount>
        objects;
    container::Array<float, kTimeOfDayCount> infantryScale{};
    uint32_t globalLightCount = 3;
};

struct RenderDeviceGameData final {
    uint32_t width = presentation_defaults::DEFAULT_OUTPUT_WIDTH;
    uint32_t height = presentation_defaults::DEFAULT_OUTPUT_HEIGHT;
    uint32_t antiAliasing = 0;
    uint32_t textureFilter = 2;
    uint32_t anisotropyLevel = 2;
    uint32_t gamma = 50;
    float displayGamma = 1.0f;
    uint32_t textureReductionFactor = 0;
    bool dynamicLodEnabled = render_lod_policy::kDynamicLodEnabled;
    bool fpsLimitEnabled = true;
};

// GlobalData's ActiveBody auto-particle contract.  These are authored visual
// names and per-object counts, not simulation state or renderer capacity.
// A Draw channel contributes its contiguous Prefix01, Prefix02... bones; the
// renderer enforces the maximum across all channels of the same Object.
struct RenderBodyParticleChannel final {
    container::String prefix;
    container::String particleSystem;
    uint32_t maximumSystems = 0;
};

struct RenderBodyParticleGameData final {
    RenderBodyParticleChannel fireSmall;
    RenderBodyParticleChannel fireMedium;
    RenderBodyParticleChannel fireLarge;
    RenderBodyParticleChannel smokeSmall;
    RenderBodyParticleChannel smokeMedium;
    RenderBodyParticleChannel smokeLarge;
    RenderBodyParticleChannel aflame;
};

struct RenderControlBarPowerGameData final {
    int32_t logarithmicBase = 7;
    float intervals = 3.0f;
    int32_t yellowRange = 5;
};

// Authored and user-visible behaviour only. No allocation limit belongs in
// this value, even when changing one field has an incidental performance cost.
struct RenderVisualDescriptor final {
    RenderCameraGameData camera;
    RenderInputGameData input;
    RenderShadowGameData shadows;
    RenderTerrainGameData terrain;
    RenderWaterGameData water;
    RenderVisibilityGameData visibility;
    RenderObjectFeedbackGameData objectFeedback;
    RenderLightingGameData lighting;
    RenderDeviceGameData device;
    RenderBodyParticleGameData bodyParticles;
    RenderControlBarPowerGameData controlBarPower;
    RenderTimeOfDay defaultTimeOfDay = RenderTimeOfDay::Afternoon;
    RenderWeather defaultWeather = RenderWeather::Normal;
    // RefCode GlobalData defaults both switches to true. They control the
    // confirmed ModelCondition NIGHT/SNOW producer, not renderer lighting or
    // Weather.ini particle visibility.
    bool forceModelsToFollowTimeOfDay = true;
    bool forceModelsToFollowWeather = true;
    float particleScale = 1.0f;
};

// Session-selected quality/work limits. These may reject optional work, but
// they never reinterpret an authored size, colour, lifetime or geometry value.
struct RenderOperationalBudget final {
    uint32_t maximumParticles = 2500;
    uint32_t maximumFieldParticles = 30;
    uint32_t maximumTerrainTracks = 100;
    uint32_t terrainLodTargetMilliseconds = 45;
    uint32_t modelUploadsPerFrame = 1;
    uint32_t modelUploadsPerLoadingFrame = 16;
    uint64_t modelUploadBytesPerFrame = 8ull * 1024ull * 1024ull;
    uint64_t modelUploadBytesPerLoadingFrame = 64ull * 1024ull * 1024ull;
    uint32_t modelUploadMicrosecondsPerFrame = 2000;
    uint32_t modelUploadMicrosecondsPerLoadingFrame = 12000;
    uint32_t initialParticleEmitterCapacity = 256;
    uint32_t maximumParticleEmitters = 4096;
    uint32_t maximumAttachedFxEmitters = 16384;
    uint32_t maximumFxPresentationCommands = 16384;
    uint32_t maximumGroundProjectorsPerFrame =
        ground_decals::performance_limits::kDefaultMaximumInstancesPerFrame;
    uint32_t maximumGroundProjectorTextures =
        ground_decals::performance_limits::kDefaultMaximumResidentTextures;
    uint32_t particleDrawExpansionFactor = 6;
};

// Original fixed-capacity compatibility values. They describe the legacy
// renderer contract and are intentionally separate from optional-work budgets.
struct RenderCompatibilityCapacities final {
    uint32_t maximumRoadSegments = 4000;
    uint32_t maximumRoadVertices = 3000;
    uint32_t maximumRoadIndices = 5000;
    uint32_t maximumRoadTypes = 35;
    uint32_t maximumVisibleTranslucentObjects = 512;
    uint32_t maximumVisibleOccluderObjects = 512;
    uint32_t maximumVisibleOccludeeObjects = 512;
    uint32_t maximumVisibleOtherObjects = 512;
};

// One parsed StaticGameLOD profile. Track history has its own established
// descriptor; the fields here are the remaining original LOD policy.
struct RenderGameLodProfile final {
    uint32_t maximumParticles = 2500;
    bool useShadowVolumes = true;
    bool useShadowDecals = true;
    bool useCloudMap = true;
    bool useLightMap = true;
    bool showSoftWaterEdge = true;
    bool useBuildupScaffolds = true;
    bool useTreeSway = true;
    bool useHeatEffects = true;
    bool dynamicLodEnabled = render_lod_policy::kDynamicLodEnabled;
    bool fpsLimitEnabled = true;
    bool showTrees = true;
    uint32_t textureReductionFactor = 0;
};

struct RenderDynamicLodProfile final {
    uint32_t minimumFramesPerSecond = 0;
    uint32_t particleSkipMask = 0;
    uint32_t debrisSkipMask = 0;
    float slowDeathScale = 1.0f;
    RenderParticlePriority minimumParticlePriority =
        RenderParticlePriority::WeaponExplosion;
    RenderParticlePriority minimumParticleSkipPriority =
        RenderParticlePriority::WeaponExplosion;
};

// Complete requested scene-detail policy. Resolution, final-image AA, gamma,
// input preferences and internal engineering budgets do not belong here.
struct RenderFeatureQualitySettings final {
    RenderStaticLod staticLod = render_lod_policy::kStaticProfile;
    RenderDynamicLod dynamicLod = render_lod_policy::kDynamicProfile;
    RenderTerrainLod terrainLod = render_lod_policy::kTerrainProfile;
    uint32_t maximumParticles = 2500;
    uint32_t textureReductionFactor = 0;
    bool useShadowVolumes = true;
    bool useShadowDecals = true;
    bool useCloudMap = true;
    bool useLightMap = true;
    bool showSoftWaterEdge = true;
    bool showTrees = true;
    bool useTreeSway = true;
    bool useBuildupScaffolds = true;
    bool extraAnimations = true;
    bool useDrawModuleLod = false;
    bool useHeatEffects = true;
    bool behindBuildingMarkers = true;
    bool dynamicLodEnabled = render_lod_policy::kDynamicLodEnabled;
    RenderParticleSimulationBackend particleSimulationBackend =
        RenderParticleSimulationBackend::Cpu;
};

struct RenderFeaturePresetSet final {
    RenderFeatureQualitySettings engineSafe;
    container::Array<RenderFeatureQualitySettings,
                     static_cast<size_t>(RenderStaticLod::Count)> profiles;
};

// Every member is an optional external layer. Absent values preserve the
// selected complete preset/base.
struct RenderFeatureQualityOverrides final {
    std::optional<RenderStaticLod> staticLod;
    std::optional<RenderDynamicLod> dynamicLod;
    std::optional<RenderTerrainLod> terrainLod;
    std::optional<uint32_t> maximumParticles;
    std::optional<uint32_t> textureReductionFactor;
    std::optional<bool> useShadowVolumes;
    std::optional<bool> useShadowDecals;
    std::optional<bool> useCloudMap;
    std::optional<bool> useLightMap;
    std::optional<bool> showSoftWaterEdge;
    std::optional<bool> showTrees;
    std::optional<bool> useTreeSway;
    std::optional<bool> useBuildupScaffolds;
    std::optional<bool> extraAnimations;
    std::optional<bool> useDrawModuleLod;
    std::optional<bool> useHeatEffects;
    std::optional<bool> behindBuildingMarkers;
    std::optional<bool> dynamicLodEnabled;
    std::optional<RenderParticleSimulationBackend> particleSimulationBackend;
};

struct RenderDisplaySettings final {
    uint32_t width = presentation_defaults::DEFAULT_OUTPUT_WIDTH;
    uint32_t height = presentation_defaults::DEFAULT_OUTPUT_HEIGHT;
    uint32_t refreshRateHz = 0;
    uint32_t textureFilter = 2;
    uint32_t anisotropyLevel = 2;
    uint32_t gamma = 50;
    float displayGamma = 1.0f;
    uint32_t legacyAntiAliasing = 0; // Compatibility MSAA input only.
    RenderDisplayMode displayMode = RenderDisplayMode::Windowed;
    RenderAntiAliasingMode antiAliasingMode = RenderAntiAliasingMode::Off;
    float fxaaSubpixel = 0.75f;
    float fxaaEdgeThreshold = 0.166f;
    float fxaaEdgeThresholdMin = 0.0833f;
    bool verticalSync = false;
    bool fpsLimitEnabled = true;
};

struct RenderDisplayOverrides final {
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
    std::optional<uint32_t> refreshRateHz;
    std::optional<uint32_t> textureFilter;
    std::optional<uint32_t> anisotropyLevel;
    std::optional<uint32_t> gamma;
    std::optional<float> displayGamma;
    std::optional<uint32_t> legacyAntiAliasing;
    std::optional<RenderDisplayMode> displayMode;
    std::optional<RenderAntiAliasingMode> antiAliasingMode;
    std::optional<float> fxaaSubpixel;
    std::optional<float> fxaaEdgeThreshold;
    std::optional<float> fxaaEdgeThresholdMin;
    std::optional<bool> verticalSync;
    std::optional<bool> fpsLimitEnabled;
};

enum class RenderDisplayChangeMask : uint32_t {
    None = 0,
    Resolution = 1u << 0u,
    OutputMode = 1u << 1u,
    FramePacing = 1u << 2u,
    AntiAliasing = 1u << 3u,
    TextureSampling = 1u << 4u,
    Gamma = 1u << 5u,
};
[[nodiscard]] constexpr RenderDisplayChangeMask operator|(
    RenderDisplayChangeMask a, RenderDisplayChangeMask b) noexcept {
    return static_cast<RenderDisplayChangeMask>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr RenderDisplayChangeMask& operator|=(
    RenderDisplayChangeMask& a, RenderDisplayChangeMask b) noexcept {
    return a = a | b;
}
[[nodiscard]] constexpr bool hasRenderDisplayChange(
    RenderDisplayChangeMask mask, RenderDisplayChangeMask change) noexcept {
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(change)) != 0;
}

enum class RenderDisplayFallbackMask : uint32_t {
    None = 0,
    ResolutionClamped = 1u << 0u,
    AnisotropyClamped = 1u << 1u,
    FxaaUnavailable = 1u << 2u,
    OutputModeUnavailable = 1u << 3u,
    DesktopModeApplied = 1u << 4u,
};
[[nodiscard]] constexpr RenderDisplayFallbackMask operator|(
    RenderDisplayFallbackMask a, RenderDisplayFallbackMask b) noexcept {
    return static_cast<RenderDisplayFallbackMask>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr RenderDisplayFallbackMask& operator|=(
    RenderDisplayFallbackMask& a, RenderDisplayFallbackMask b) noexcept {
    return a = a | b;
}
[[nodiscard]] constexpr bool hasRenderDisplayFallback(
    RenderDisplayFallbackMask mask,
    RenderDisplayFallbackMask fallback) noexcept {
    return (static_cast<uint32_t>(mask) &
            static_cast<uint32_t>(fallback)) != 0;
}

struct RenderDisplayCapabilities final {
    uint32_t maximumWidth = 16384;
    uint32_t maximumHeight = 16384;
    uint32_t maximumAnisotropy = 16;
    bool supportsFxaa = true;
    uint32_t desktopWidth = 0;
    uint32_t desktopHeight = 0;
    uint32_t desktopRefreshRateHz = 0;
    bool supportsBorderlessFullscreen = true;
    bool supportsExclusiveFullscreen = true;
};
struct ResolvedRenderFeatureSnapshot final {
    RenderFeatureQualitySettings requested;
    uint64_t revision = 0;
};
struct ResolvedRenderDisplaySnapshot final {
    RenderDisplaySettings requested;
    RenderDisplaySettings effective;
    uint64_t revision = 0;
    RenderDisplayChangeMask changeMask = RenderDisplayChangeMask::None;
    RenderDisplayFallbackMask fallbackMask = RenderDisplayFallbackMask::None;
};

// Main-thread-owned observation of the SDL window after an output request or
// a platform window event.  The render thread consumes this value only; it
// never queries or mutates SDL window/display state.  revision is monotonic
// for the lifetime of one RendererSubsystem and permits a newest-only mailbox
// to collapse a resize storm without allowing an older extent to rebuild the
// swapchain after a newer one.
struct RenderWindowOutputState final {
    uint32_t logicalWidth = 0;
    uint32_t logicalHeight = 0;
    uint32_t pixelWidth = 0;
    uint32_t pixelHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t refreshRateHz = 0;
    RenderDisplayMode displayMode = RenderDisplayMode::Windowed;
    uint64_t revision = 0;
    uint64_t requestRevision = 0;
    bool applySucceeded = true;

    [[nodiscard]] bool validPixelExtent() const noexcept {
        return pixelWidth != 0u && pixelHeight != 0u;
    }

    [[nodiscard]] bool validLogicalExtent() const noexcept {
        return logicalWidth != 0u && logicalHeight != 0u;
    }
};

// Presentation settings compiled by GameDataLoader and copied into one
// immutable shared snapshot when GameSession starts. WorldRenderSnapshot
// forwards that same handle; command recording must never reach back into
// GlobalData, Options.ini, GameDataLoader, or a live GameSession.
struct RenderGameDataSettings final {
    RenderVisualDescriptor visual;
    RenderOperationalBudget operational;
    RenderCompatibilityCapacities compatibility;
    RenderStaticLod selectedLod = render_lod_policy::kStaticProfile;
    RenderDynamicLod selectedDynamicLod =
        render_lod_policy::kDynamicProfile;
    container::Array<RenderGameLodProfile,
                     static_cast<size_t>(RenderStaticLod::Count)>
        lodProfiles;
    container::Array<RenderDynamicLodProfile,
                     static_cast<size_t>(RenderDynamicLod::Count)>
        dynamicLodProfiles;

    RenderGameDataSettings() noexcept;
};

// Non-negotiable memory/validation bounds. They are intentionally not fields
// in RenderGameDataSettings and cannot be raised by INI, LOD or Options.
namespace render_game_data_limits {
inline constexpr uint32_t kMaximumParticles = 8192;
inline constexpr uint32_t kMaximumFieldParticles = 1024;
inline constexpr uint32_t kMaximumTerrainTracks = 200;
inline constexpr uint32_t kMaximumRoadSegments = 65536;
inline constexpr uint32_t kMaximumRoadVertices = 1u << 20u;
inline constexpr uint32_t kMaximumRoadIndices = 1u << 21u;
inline constexpr uint32_t kMaximumRoadTypes = 256;
inline constexpr uint32_t kMaximumVisibleObjectsPerClass = 1u << 20u;
inline constexpr uint32_t kMaximumModelUploadsPerFrame = 64;
inline constexpr uint64_t kMaximumModelUploadBytesPerFrame =
    1024ull * 1024ull * 1024ull;
inline constexpr uint32_t kMaximumModelUploadMicrosecondsPerFrame = 1000000;
inline constexpr uint32_t kMaximumParticleEmitters = 16384;
inline constexpr uint32_t kMaximumAttachedFxEmitters = 65536;
inline constexpr uint32_t kMaximumFxPresentationCommands = 65536;
inline constexpr uint32_t kMaximumGroundProjectorsPerFrame = 65536;
inline constexpr uint32_t kMaximumGroundProjectorTextures = 4096;
inline constexpr uint32_t kMaximumParticleDrawExpansionFactor = 64;
inline constexpr uint32_t kMaximumTextureReductionFactor = 8;
inline constexpr uint32_t kMaximumRenderDimension = 16384;
} // namespace render_game_data_limits

[[nodiscard]] RenderFeatureQualitySettings renderFeatureQualityFromGameData(
    const RenderGameDataSettings& settings) noexcept;
[[nodiscard]] RenderFeaturePresetSet renderFeaturePresetSetFromGameData(
    const RenderGameDataSettings& settings) noexcept;
void projectRenderFeatureQualityToGameData(
    const RenderFeatureQualitySettings& quality,
    RenderGameDataSettings& settings) noexcept;
[[nodiscard]] RenderDisplaySettings renderDisplaySettingsFromGameData(
    const RenderGameDataSettings& settings) noexcept;
void projectRenderDisplaySettingsToGameData(
    const RenderDisplaySettings& display,
    RenderGameDataSettings& settings) noexcept;
void applyRenderFeatureQualityOverrides(
    RenderFeatureQualitySettings& settings,
    const RenderFeatureQualityOverrides& overrides) noexcept;
[[nodiscard]] ResolvedRenderFeatureSnapshot resolveRenderFeatureQuality(
    const RenderFeatureQualitySettings& base,
    const RenderFeatureQualityOverrides& overrides = {},
    uint64_t revision = 0) noexcept;
void applyRenderDisplayOverrides(
    RenderDisplaySettings& settings,
    const RenderDisplayOverrides& overrides) noexcept;
[[nodiscard]] RenderDisplayChangeMask renderDisplayChangeMask(
    const RenderDisplaySettings& before,
    const RenderDisplaySettings& after) noexcept;
[[nodiscard]] ResolvedRenderDisplaySnapshot resolveRenderDisplaySettings(
    const RenderDisplaySettings& base,
    const RenderDisplayOverrides& overrides = {},
    const RenderDisplayCapabilities& capabilities = {},
    const RenderDisplaySettings* previousEffective = nullptr,
    uint64_t revision = 0) noexcept;

[[nodiscard]] bool applyRenderGameDataIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr,
    container::String* error = nullptr);

[[nodiscard]] bool applyRenderMouseIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr,
    container::String* error = nullptr);
[[nodiscard]] bool applyRenderInGameUiIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr,
    container::String* error = nullptr);
[[nodiscard]] bool applyPresentationMiscAudioIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr,
    container::String* error = nullptr);
[[nodiscard]] bool applyRenderLanguageIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr,
    container::String* error = nullptr);
[[nodiscard]] bool applyRenderGameLodIni(
    container::StringView content, RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr,
    container::String* error = nullptr);
void applyRenderOptions(
    const config::GraphPreferences& preferences,
    RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr);
void normalizeRenderGameDataSettings(
    RenderGameDataSettings& settings,
    container::Vector<container::String>* diagnostics = nullptr) noexcept;

} // namespace engine
