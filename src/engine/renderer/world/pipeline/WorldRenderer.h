#pragma once

#include "core/container/container_types.h"

#include "engine/renderer/runtime/RendererStats.h"
#include "engine/renderer/world/effects/DynamicPointLightRuntime.h"
#include "presentation/render/RenderSceneSnapshot.h"
#include "engine/renderer/world/model/StaticTextureMapper.h"
#include "engine/renderer/world/pipeline/WorldCamera.h"
#include "engine/renderer/world/effects/WorldPostProcessRenderer.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
namespace engine::d3d12 {
class D3D12Device;
}

namespace engine::render {

namespace world_renderer_detail {
class WorldRendererUploadOwner;
}

struct StaticMeshVertex;

enum class StaticMeshBlendMode : uint8_t {
    Opaque,
    Additive,
    Alpha,
    Multiply,
    Screen,
    Count,
};

enum class StaticMeshDepthCompare : uint8_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
    Count,
};

enum class StaticMeshAlphaTestMode : uint8_t {
    Disabled,
    GreaterEqual,
    LessEqual,
};

// RefCode's W3DCustomEdging is not a regular terrain alpha blend.  It clips
// one shared edge-mask texture twice: the low band reveals the blend-source
// texture and the high band reveals the edge RGB.  Keep this phase explicit
// instead of overloading W3D material alpha-test semantics.
enum class StaticMeshTerrainEdgePhase : uint8_t {
    Disabled,
    BlendSource,
    EdgeRgb,
};

// Renderer-owned lighting values derived from a sealed world-frame snapshot.
// This deliberately contains no map/game object pointers: DX12 command
// recording receives a value copy that is stable for the whole frame.
struct WorldDirectionalLight {
    // Surface-to-light direction.  W3D GlobalLighting serializes the legacy
    // lightPos/ray direction, so snapshot extraction negates it before
    // placing it here.
    math::vec3 directionToLight{0.0f, 0.0f, 1.0f};
    math::vec3 diffuse{};
};

struct WorldLightEnvironment {
    // Preserve the existing deterministic diagnostic/world fallback when a
    // map has no GlobalLighting chunk.  The first directional light mirrors
    // the former fixed temporary direction/intensity; the remaining slots
    // are intentionally disabled.
    math::vec3 ambient{0.25f, 0.25f, 0.25f};
    container::Array<WorldDirectionalLight, kTerrainRenderGlobalLightCount> directionalLights{{
        {{0.35f, 0.85f, 0.40f}, {0.75f, 0.75f, 0.75f}},
        {},
        {},
    }};

    // RefCode's W3DDisplay uses ObjectLight[tod][0].ambient as scene
    // ambient, then installs all three object lights as directional diffuse
    // lights with zero per-light ambient.  This conversion makes that
    // contract explicit and protects command recording from malformed map
    // values without reaching back into live map state.
    [[nodiscard]] static WorldLightEnvironment fromTerrainGlobalLighting(
        const TerrainGlobalLightingRenderData& globalLighting) noexcept;
};

inline constexpr uint32_t kMaximumWorldDynamicPointLights =
    dynamic_lights::performance_limits::kTerrainReceiverMaximumLights;

// CPU-visible form of the shader's inner/outer linear falloff.  Malformed
// radii are clamped to a finite hard edge, making focused probes deterministic.
[[nodiscard]] float worldDynamicPointLightAttenuation(
    float distanceToLight, float innerRadius, float outerRadius) noexcept;

struct ObjectDynamicLightEnvironment final {
    math::vec3 ambient{};
    container::Array<DynamicPointLightRenderData,
                     dynamic_lights::performance_limits::kObjectDiffuseMaximumLights>
        diffuseLights{};
    uint32_t diffuseLightCount = 0;
};

// LightEnvironmentClass accumulates every accepted point light into ambient,
// while retaining only the four strongest attenuated diffuse contributors at
// the object's centre. This pure projection is shared by command recording
// and focused probes.
[[nodiscard]] ObjectDynamicLightEnvironment selectObjectDynamicLights(
    math::vec3 objectCenter,
    container::Span<const DynamicPointLightRenderData> lights) noexcept;

// RefCode installs authored map point lights in the scene light environment,
// independently of Drawable::ReceivesDynamicLights. Dynamic LightPulse input
// can therefore be disabled while authored ambient/diffuse still reaches an
// object. Both sources share the original all-ambient/top-four-diffuse rule.
[[nodiscard]] ObjectDynamicLightEnvironment selectObjectPointLights(
    math::vec3 objectCenter,
    container::Span<const DynamicPointLightRenderData> dynamicLights,
    container::Span<const TerrainPointLightRenderData> sceneLights,
    bool includeDynamicLights = true,
    bool includeSceneLights = true) noexcept;

enum class StaticMeshDynamicLightReceiver : uint8_t {
    None,
    Object,
    Terrain,
};

// Stable semantic layers for the tactical world. The numeric order is the
// compositing contract: camera distance may reorder translucent primitives
// only inside one layer and must never move roads/terrain overlays across
// unrelated object or track passes. Water sits between opaque and transparent
// objects, matching W3DWater's fixed level after non-translucent geometry.
enum class StaticMeshWorldLayer : uint8_t {
    Background,
    TerrainBase,
    TerrainBlend,
    TerrainExtra,
    Roads,
    // Ordering sentinel owned by GroundProjectorRenderer. Static mesh packets
    // must not use this value; split FullWorld excludes the sentinel itself.
    ProjectorsAndScorches,
    Bridges,
    Tracks,
    // W3DWaypointBuffer draws after terrain/roads/bridges/fog/tracks but
    // before Bibs and ordinary objects, using PASS_ALWAYS additive lines.
    Waypoints,
    // RefCode emits faction Bibs at the end of the terrain object's render,
    // before the scene traverses ordinary objects. Their PASS_ALWAYS shader
    // is safe only at this boundary: later object depth/color replaces them.
    Bibs,
    ObjectsOpaque,
    Water,
    ObjectsTransparent,
    ShroudComposite,
    WorldUi,
    Count,
};

enum class StaticMeshPassExecution : uint8_t {
    FullWorld,
    // Split main-world colour submission around the dedicated ground
    // projector renderer. The pre phase owns all once-per-frame preparation
    // (visibility upload and complete directional shadow); the post phase
    // consumes the preserved reflection/shadow state and finishes statistics.
    FullWorldBeforeProjectors,
    FullWorldAfterProjectors,
    Overlay,
    Reflection,
};

// Backend-facing packet for an immutable static mesh primitive. GPU resources
// remain owned by the asset cache; the views and descriptor handle are copied
// into the packet for the duration of command recording.
struct StaticMeshDrawPacket {
    D3D12_VERTEX_BUFFER_VIEW vertexBuffer{};
    D3D12_INDEX_BUFFER_VIEW indexBuffer{};
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrv{};
    D3D12_GPU_DESCRIPTOR_HANDLE detailTextureSrv{};
    // Classic terrain/road/bridge macro layers. Bit 0 enables the animated
    // TSCloudMed stage and bit 1 enables the repeating macro/light map. The
    // handles remain packet-lifetime values owned by WorldTextureCache.
    D3D12_GPU_DESCRIPTOR_HANDLE terrainCloudTextureSrv{};
    D3D12_GPU_DESCRIPTOR_HANDLE terrainMacroTextureSrv{};
    uint8_t terrainMacroFlags = 0;
    // Optional immutable CPU geometry retained by W3D assets for the legacy
    // blended-polygon sorting path. These pointers live exactly as long as
    // the model that emitted this frame's packet.
    const StaticMeshVertex* sortingVertices = nullptr;
    uint32_t sortingVertexCount = 0;
    const uint32_t* sortingIndices = nullptr;
    uint32_t sortingIndexCount = 0;
    bool requiresTriangleSorting = false;
    // W3D's authored static sort bin. Zero is SORT_LEVEL_NONE and keeps
    // camera/triangle sorting; positive levels use stable high-to-low bins.
    int32_t sortLevel = 0;
    StaticTextureMapperStages textureMappers{};
    // Deterministic presentation clock sealed from PreparedWorldFrame's
    // simulation frame. Vertex mappers must never consult a renderer wall
    // clock while recording this packet.
    float visualTimeSeconds = 0.0f;

    // wwmath/HLSL use row vectors. Hierarchical W3D callers provide
    // part.localTransform * entityWorld here, in that order.
    math::transform worldTransform{};
    // Previous authoritative endpoint consumed only by the GPU vertex path.
    // Alpha one makes this packet bit-for-bit equivalent to the current-only
    // path; no interpolated value is ever written back to PreparedWorldFrame.
    math::transform previousWorldTransform{};
    float interpolationAlpha = 1.0f;
    // Backend-neutral world-space point used for transparent ordering. Skin
    // packets intentionally keep worldTransform at identity because their
    // palette already contains world-space joints, so their instance/model
    // origin must be carried independently instead of sorting at (0,0,0).
    math::vec3 sortCenter{};
    bool hasExplicitSortCenter = false;
    math::vec4 diffuse{1.0f, 1.0f, 1.0f, 1.0f};
    math::vec3 ambient{0.25f, 0.25f, 0.25f};
    math::vec3 specular{};
    math::vec3 emissive{};
    float shininess = 0.0f;
    // Resolved from the mesh-level W3D prelit flag. When false, the pixel
    // shader keeps texture/material/vertex colour and all alpha/blend/fog
    // handling, but skips real-time scene lighting.
    bool lightingEnabled = false;
    bool primaryGradientDisabled = false;
    // Preserved raw W3D texturing contract. Stock texturing-disabled passes
    // have no texture binding and are already visually equivalent today;
    // retaining the bit prevents Mod/future fog-texturing work from reparsing.
    bool texturingEnabled = true;
    // The final base-texture pass selected from a PRELIT_LIGHTMAP_MULTI_PASS
    // wrapper is an authored baked-light overlay, not an independently
    // translucent surface. It must not be triangle-sorted, shadow-cast, or
    // receive the live light environment a second time.
    bool lightmapPass = false;
    // Per-instance scalar for the script infantry-light override.  Normal
    // terrain, skybox and non-infantry packets retain one, so their existing
    // object-light environment is unchanged.
    float directionalLightScale = 1.0f;
    // Legacy Drawable::colorFlash adds its TintEnvelope colour to the
    // drawable's light environment.  Keep it as detached per-instance
    // presentation data instead of modifying an immutable W3D material or
    // any gameplay component.
    math::vec3 scriptFlashTint{};
    float heatVisionIntensity = 0.0f;
    bool heatVisionOnly = false;
    float objectOpacity = 1.0f;
    // W3DTreeBuffer's per-tree upright bend remains per-instance, so trees
    // with different push phases may still share immutable mesh resources.
    math::vec2 treePushAsideDirection{};
    float treePushAsideAmount = 0.0f;
    float treePushAsideDistanceFactor = 0.0f;
    float treePushAsideDarkeningFactor = 0.0f;

    // NAMED_CUSTOM_COLOR's resolved indicator colour. The two eligibility
    // bits are derived from immutable W3D names at packet creation (legacy
    // HOUSECOLOR mesh and ZHC texture conventions); neither one changes the
    // shared D3D12W3dModel or WorldTextureCache.
    math::vec3 scriptIndicatorColor{};
    bool hasScriptIndicatorColor = false;
    bool houseColorVertexMaterial = false;
    bool houseColorTexture = false;
    bool houseColorInverseAlphaMask = false;
    // Dynamic LightPulse illumination is explicit rather than inferred from
    // directional-shadow policy. Terrain, roads and ordinary W3D opt in;
    // water, skybox, track marks and projectors remain unlit by default.
    bool receivesDynamicLights = false;
    // Authored map point lights are static scene lights, not LightPulse
    // effects. Ordinary W3D objects/props/trees opt in regardless of the
    // per-Drawable dynamic-light switch; terrain is already vertex-lit.
    bool receivesScenePointLights = false;
    StaticMeshDynamicLightReceiver dynamicLightReceiver =
        StaticMeshDynamicLightReceiver::None;

    // Classic W3D skin geometry stores one hierarchy joint per vertex. The
    // palette is owned by PreparedWorldFrame for this command-recording call.
    // A null/zero palette denotes an ordinary rigid/static primitive.
    const math::transform* skinPalette = nullptr;
    const math::transform* previousSkinPalette = nullptr;
    uint32_t skinBoneCount = 0;

    [[nodiscard]] math::vec3 resolvedSortCenter() const noexcept {
        return hasExplicitSortCenter ? sortCenter : worldTransform.translation();
    }

    [[nodiscard]] bool sharesSkinPaletteWith(
        const StaticMeshDrawPacket& other) const noexcept {
        return skinBoneCount != 0 && skinPalette != nullptr &&
            skinPalette == other.skinPalette &&
            previousSkinPalette == other.previousSkinPalette &&
            skinBoneCount == other.skinBoneCount;
    }

    // W3D's mesh-level TWO_SIDED flag disables back-face culling for every
    // primitive in the mesh. Alpha-tested materials use the original WW3D
    // reference value in the pixel shader rather than fixed-function state.
    bool twoSided = false;
    StaticMeshAlphaTestMode alphaTestMode = StaticMeshAlphaTestMode::Disabled;
    StaticMeshTerrainEdgePhase terrainEdgePhase = StaticMeshTerrainEdgePhase::Disabled;
    uint8_t samplerMode = 0;
    uint8_t detailSamplerMode = 0;
    uint8_t detailColorFunc = 0;
    uint8_t detailAlphaFunc = 0;
    uint8_t fogFunc = 0;
    bool hasDetailTexture = false;
    bool depthWrite = true;
    StaticMeshDepthCompare depthCompare = StaticMeshDepthCompare::LessEqual;
    StaticMeshBlendMode blendMode = StaticMeshBlendMode::Opaque;
    // TerrainLogic owns the polygon and water level. This merely selects the
    // renderer-owned water surface response for its sealed draw packet.
    bool waterSurface = false;
    bool castsShadow = true;
    bool receivesShadow = true;
    bool receivesVisibility = false;
    // Active map-boundary masking is independent from fog diplomacy. Friendly
    // objects bypass local shroud luminance but still set this bit, so staging
    // content outside the current WorldBuilder rectangle cannot leak through.
    bool receivesMapBorder = false;
    // Terrain-like surfaces keep their physical geometry outside the active
    // rectangle and fade it into the border shroud. Objects leave this false
    // and retain the strict map-boundary discard above.
    bool fadesMapBorder = false;
    StaticMeshWorldLayer worldLayer = StaticMeshWorldLayer::ObjectsOpaque;
    uint32_t materialPass = 0;

    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t baseVertex = 0;
};

// Pure ordering predicate used by command recording and focused probes.
// Layer order always wins; opaque packets are state-grouped and translucent
// packets remain strict back-to-front only within their semantic layer.
[[nodiscard]] bool staticMeshDrawOrderLess(
    const StaticMeshDrawPacket& lhs, const StaticMeshDrawPacket& rhs,
    math::vec3 cameraPosition) noexcept;

struct WorldLocalVisibilityGpuBinding final {
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrv{};
    math::vec2 origin{};
    math::vec2 textureSize{};
    math::vec2 playableMinimum{};
    math::vec2 playableMaximum{};
    float inverseCellSize = 0.0f;
    bool enabled = false;
    bool playableBoundsEnabled = false;
};

// Result of the renderer-local CAMERA_*_SLAVE_MODE consumer. `camera` is
// always a standalone value; `applied` distinguishes an actual posed-bone
// override from an enabled request that is still awaiting a W3D asset/bone.
// The latter must retain the ordinary tactical camera rather than pretending
// that an entity root transform is a camera bone.
struct CameraSlavePresentationCamera {
    RenderCameraSnapshot camera;
    bool applied = false;
};

// Dedicated D3D12 3D world pass. It consumes renderer-owned draw packets and
// has no dependency on game state or packed W3D file structures.
class WorldRenderer final {
public:
    WorldRenderer() = default;
    explicit WorldRenderer(d3d12::D3D12Device& device) noexcept;
    ~WorldRenderer();

    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;
    WorldRenderer(WorldRenderer&&) = delete;
    WorldRenderer& operator=(WorldRenderer&&) = delete;

    // Either construct with a device and call init(), or use this overload.
    bool init();
    bool init(d3d12::D3D12Device& device);
    void shutdown();

    // Clears epoch-scoped presentation cursors and validity without
    // rebuilding device resources. The next frame republishes visibility,
    // reflection, shadow and script-filter state for the new epoch.
    void resetPresentationEpoch(uint64_t presentationEpoch) noexcept;

    // Rebuilds only the ordinary world root signature/PSOs when a new
    // session changes the frozen Options.ini sampling policy. Post-process,
    // shadow-map and UI samplers keep their explicitly authored contracts.
    [[nodiscard]] bool configureTextureSampling(
        uint32_t textureFilter, uint32_t anisotropyLevel,
        uint32_t sampleCount);

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] const StaticMeshRenderStats& lastStaticMeshStats() const noexcept {
        return m_lastStaticMeshStats;
    }
    [[nodiscard]] const SceneColorRenderStats& sceneColorStats() const noexcept {
        return m_postProcessRenderer.sceneColorStats();
    }

    // The renderer-side camera is strictly a diagnostic fallback.  Real game
    // frames supply their complete camera through RenderCameraSnapshot.
    WorldCamera& debugCamera() noexcept { return m_debugCamera; }
    const WorldCamera& debugCamera() const noexcept { return m_debugCamera; }
    [[nodiscard]] RenderCameraSnapshot debugCameraSnapshot() const noexcept {
        return m_debugCamera.toSnapshot();
    }

    // A renderer-only diagnostic mode shared with the application's F1
    // skeleton view.  It deliberately ignores material textures, alpha tests,
    // fog, and scene lighting, so a visible grid proves that the camera and
    // terrain/model geometry reached the GPU even when asset resolution is
    // still under investigation.
    void setSkeletonMode(bool enabled) noexcept { m_skeletonMode = enabled; }
    [[nodiscard]] bool skeletonMode() const noexcept { return m_skeletonMode; }

    // Material-isolation diagnostic. In this mode the normal textured PSO
    // samples only t0 and returns it directly: material/vertex colour,
    // detail stage, lighting and fog are deliberately bypassed. Skeleton mode
    // retains priority, so F1 remains a geometry-only diagnostic.
    void setTextureOnlyMode(bool enabled) noexcept { m_textureOnlyMode = enabled; }
    [[nodiscard]] bool textureOnlyMode() const noexcept { return m_textureOnlyMode; }

    // Must be called after D3D12Device::beginFrame() and before UI submission.
    // elapsedSeconds is used solely to rotate the diagnostic cube.
    void renderDebugScene(float elapsedSeconds);

    // Records one fixed-layer static world submission. Vertex buffers must
    // use StaticMeshVertex's position/normal/uv/color layout and index
    // buffers must use DXGI_FORMAT_R32_UINT. A split FullWorld call passes the
    // same complete packet span to BeforeProjectors and AfterProjectors; the
    // renderer filters it at the semantic ProjectorsAndScorches boundary.
    // Overlay mode preserves full-world shadow/visibility state for unrelated
    // late sublayers.
    void renderStaticMeshes(container::Span<const StaticMeshDrawPacket> drawPackets,
                            const RenderCameraSnapshot& cameraSnapshot,
                            const WorldLightEnvironment& lightEnvironment = {},
                            const LocalVisibilityRenderSnapshot& localVisibility = {},
                            float worldTimeSeconds = 0.0f,
                            container::Span<const DynamicPointLightRenderData>
                                dynamicPointLights = {},
                            container::Span<const TerrainPointLightRenderData>
                                scenePointLights = {},
                            StaticMeshPassExecution execution =
                                StaticMeshPassExecution::FullWorld);

    // Reconstructs W3DWater's WATER_TYPE_2 mirror pass into a fixed 256x256
    // renderer-owned target. Water packets are rejected by this boundary so
    // the sampled main pass cannot recurse into itself.
    [[nodiscard]] bool renderWaterReflection(
        container::Span<const StaticMeshDrawPacket> drawPackets,
        const RenderCameraSnapshot& cameraSnapshot, float planeZ,
        const WorldLightEnvironment& lightEnvironment = {},
        const LocalVisibilityRenderSnapshot& localVisibility = {},
        float worldTimeSeconds = 0.0f,
        container::Span<const DynamicPointLightRenderData>
            dynamicPointLights = {},
        container::Span<const TerrainPointLightRenderData>
            scenePointLights = {});

    // Valid after the full static-world pass has uploaded/bound the sealed
    // local-visibility texture. Raw projector/scorch passes use this exact
    // binding so they cannot repaint fully shrouded terrain.
    [[nodiscard]] WorldLocalVisibilityGpuBinding localVisibilityGpuBinding(
        const LocalVisibilityRenderSnapshot& localVisibility) const noexcept;

    // Applies a sealed, animated W3D camera-bone transform to a local world
    // pass camera. It never mutates GameCameraDirector, GameSession, ECS or
    // any replay/lockstep input. A missing target self-disables this renderer
    // consumer until a newer Enable request arrives, mirroring W3DView.
    [[nodiscard]] CameraSlavePresentationCamera applyScriptCameraSlave(
        const RenderCameraSnapshot& cameraSnapshot,
        const CameraSlaveRenderState& request, bool targetPresent,
        const std::optional<RenderMatrix>& boneWorldTransform) noexcept;

    // Consumes the detached SCREEN_SHAKE journal and returns a per-render-pass
    // camera copy.  It never writes GameCameraDirector or an audio listener:
    // RefCode offsets only the tactical view's source/target in XY.  Keeping
    // the `(epoch, sequence)` cursor in the renderer also makes newest-only
    // snapshot delivery safe when an intermediate logic frame is dropped.
    [[nodiscard]] RenderCameraSnapshot applyScriptScreenShake(
        const RenderCameraSnapshot& cameraSnapshot, uint64_t simulationFrame,
        const ScreenShakeRenderState& screenShake) noexcept;

    // Legacy CAMERA_FADE_* is a dedicated post-world blend, not a UI alpha
    // quad. Call this after world/object-icon work and before InGameUI so the
    // ControlBar remains outside the effect just as it did in W3DView.
    void renderScreenFade(const ScreenFadeRenderState& fade, uint64_t simulationFrame = 0);

    // W3D has one tactical-view filter slot: BW and CAMERA_MOTION_BLUR*
    // replace one another in stamped source order. Consume both before world
    // packets are recorded so a motion-blur Jump can alter only this local
    // presentation camera; neither GameCameraDirector nor ECS is mutated.
    [[nodiscard]] RenderCameraSnapshot prepareScriptViewFilters(
        const RenderCameraSnapshot& cameraSnapshot, uint64_t simulationFrame,
        const BlackAndWhiteRenderState& blackAndWhite,
        const MotionBlurRenderState& motionBlur) noexcept;

    // Captures the completed world pass and applies whichever renderer-owned
    // view filter prepareScriptViewFilters selected. Call before screen fade
    // and GUI so UI remains outside the legacy tactical-view effect.
    void renderScriptViewFilters();

    // Final single-sample world-space AA. It runs after script view filters
    // and before object/UI overlays; Off performs no capture or draw.
    [[nodiscard]] bool configureFxaa(
        bool enabled, float subpixel, float edgeThreshold,
        float edgeThresholdMin) noexcept;
    [[nodiscard]] bool fxaaAvailable() const noexcept {
        return m_postProcessRenderer.fxaaAvailable();
    }
    [[nodiscard]] bool renderFxaa(float tacticalViewportHeightScale);

private:
    enum class ShaderBytecode : uint8_t {
        WorldVertex,
        WorldPixel,
        DirectionalShadowVertex,
        DirectionalShadowPixel,
        Count,
    };

    struct PipelineStatePair {
        Microsoft::WRL::ComPtr<ID3D12PipelineState> backFaceCulled;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> frontFaceCulled;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> twoSided;
    };

    bool loadShaderPackages();
    bool createRootSignature();
    bool createPipelineStates();
    bool createDirectionalShadowResources();
    bool createDirectionalShadowPipelineStates();
    bool createDirectionalShadowFallbackSrv();
    void releaseDirectionalShadowResources() noexcept;
    bool ensureWaterReflectionResources();
    void releaseWaterReflectionResources() noexcept;
    bool renderDirectionalShadowMap(
        container::Span<const StaticMeshDrawPacket> drawPackets,
        const RenderCameraSnapshot& cameraSnapshot,
        const WorldLightEnvironment& lightEnvironment);
    bool createLocalVisibilityFallbackSrv();
    bool updateLocalVisibilityTexture(
        const LocalVisibilityRenderSnapshot& visibility);
    void releaseLocalVisibilityResources() noexcept;
    bool createDebugCube();

    struct ScriptScreenShakeConsumer {
        uint64_t presentationEpoch = 0;
        uint64_t lastSequence = 0;
        uint64_t localizedTrimmedThroughSequence = 0;
        uint64_t lastSimulationFrame = 0;
        bool hasSimulationFrame = false;
        math::vec2 direction{};
        math::vec2 offset{};
        float intensity = 0.0f;
    };

    struct ScriptCameraSlaveConsumer {
        uint64_t presentationEpoch = 0;
        uint64_t presentationSequence = 0;
        bool enabled = false;
    };

    d3d12::D3D12Device* m_device = nullptr;
    WorldPostProcessRenderer m_postProcessRenderer;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    container::Array<container::Vector<uint8_t>,
        static_cast<size_t>(ShaderBytecode::Count)> m_shaderBytecode;
    using DepthComparePipelines = container::Array<PipelineStatePair,
        static_cast<size_t>(StaticMeshDepthCompare::Count)>;
    using DepthWritePipelines = container::Array<DepthComparePipelines, 2>;
    container::Array<DepthWritePipelines, static_cast<size_t>(StaticMeshBlendMode::Count)> m_pipelineStates;
    PipelineStatePair m_shadowOpaquePipelineState;
    PipelineStatePair m_shadowAlphaTestPipelineState;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;
    uint32_t m_shadowMapSrv = UINT32_MAX;
    uint32_t m_shadowFallbackSrv = UINT32_MAX;
    D3D12_RESOURCE_STATES m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    math::float4x4 m_shadowViewProjection{};
    bool m_directionalShadowAvailable = false;
    bool m_directionalShadowValid = false;
    static constexpr uint32_t kWaterReflectionSize = 256;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_waterReflectionTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_waterReflectionTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_waterReflectionDepth;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_waterReflectionRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_waterReflectionDsvHeap;
    uint32_t m_waterReflectionSrv = UINT32_MAX;
    D3D12_RESOURCE_STATES m_waterReflectionTargetState =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES m_waterReflectionTextureState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    math::float4x4 m_waterReflectionViewProjection{};
    bool m_waterReflectionValid = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_localVisibilityTexture;
    uint32_t m_localVisibilitySrv = UINT32_MAX;
    uint32_t m_localVisibilityFallbackSrv = UINT32_MAX;
    D3D12_RESOURCE_STATES m_localVisibilityState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uint32_t m_localVisibilityWidth = 0;
    uint32_t m_localVisibilityHeight = 0;
    uint64_t m_localVisibilityPresentationEpoch = 0;
    uint64_t m_localVisibilityRevision = 0;
    uint64_t m_localVisibilityPolicyRevision = 0;
    uint64_t m_localVisibilityLayoutRevision = 0;
    uint8_t m_localVisibilityObserverPlayer = UINT8_MAX;
    bool m_localVisibilityEnabled = false;
    // One deliberately material-independent wireframe PSO is enough for the
    // diagnostic path: it normalizes every packet to an opaque, two-sided
    // grid instead of inheriting W3D blend/alpha state.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_skeletonPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cubeVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cubeIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_cubeVertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW m_cubeIndexBufferView{};
    WorldCamera m_debugCamera;
    ScriptScreenShakeConsumer m_scriptScreenShake;
    ScriptCameraSlaveConsumer m_scriptCameraSlave;
    bool m_skeletonMode = false;
    bool m_textureOnlyMode = false;
    // Capacity is retained, but packet values/pointers are cleared before
    // return so frame-arena palette addresses never cross submissions.
    container::Vector<const StaticMeshDrawPacket*> m_orderedDrawScratch;
    container::Vector<const StaticMeshDrawPacket*> m_shadowCasterScratch;
    uint32_t m_shadowCasterScratchHighWater = 0;
    container::Vector<StaticMeshDrawPacket> m_reflectionDrawScratch;
    struct TransparentTriangleSortEntry final {
        uint32_t sourceIndex = 0;
        float depth = 0.0f;
    };
    container::Vector<TransparentTriangleSortEntry>
        m_transparentTriangleSortScratch;
    container::Vector<uint32_t> m_transparentIndexScratch;
    container::UniquePtr<world_renderer_detail::WorldRendererUploadOwner>
        m_uploadOwner;
    StaticMeshRenderStats m_lastStaticMeshStats;
    uint32_t m_textureFilter = 2;
    uint32_t m_anisotropyLevel = 2;
    uint32_t m_sampleCount = 1;
    bool m_initialized = false;
};

} // namespace engine::render
