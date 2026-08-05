#pragma once

#include "core/container/hash_containers.h"

#include "Renderer.h"
#include "engine/fx/runtime/FxPresentationCommands.h"
#include "RendererStats.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/terrain/GroundProjectorRenderer.h"
#include "engine/renderer/world/terrain/GroundDecalPresentation.h"
#include "presentation/render/RenderSceneSnapshot.h"
#include <SDL3/SDL.h>
#include <cstddef>
#include <optional>
#include "presentation/render/PresentationDefaults.h"
#include "core/constants/Colors.h"
#include "core/constants/Strings.h"
#include "debug/debug.h"

namespace engine {

class Font;
struct GameCameraInput;
struct RenderOperationalBudget;
struct RenderGameDataSettings;
struct ResolvedRenderFeatureSnapshot;
struct ResolvedRenderDisplaySnapshot;
struct RenderWindowOutputState;
class TextureManager;
namespace render {
class WorldRenderer;
struct PreparedWorldFrame;
struct ClientOptionsRenderState;
struct ParticleRenderDrawList;
struct GpuParticlePresentationQualification;
}
namespace fx {
class ParticleSystemCatalog;
class FxListCatalog;
struct FxPresentationSnapshot;
}

// DX12Renderer: native DX12 rendering backend
class DX12Renderer : public Renderer {
public:
    enum class PreparedWorldRenderState {
        Unavailable,
        PreparationPending,
        PublishedEndpoint,
        ReusedEndpoint,
    };

    struct PreparedWorldRenderResult final {
        size_t renderedInstances = 0;
        PreparedWorldRenderState state =
            PreparedWorldRenderState::Unavailable;

        [[nodiscard]] bool preparationPending() const noexcept {
            return state == PreparedWorldRenderState::PreparationPending;
        }

        [[nodiscard]] bool publishedEndpoint() const noexcept {
            return state == PreparedWorldRenderState::PublishedEndpoint;
        }
    };

    DX12Renderer();
    ~DX12Renderer() override;

    bool init(uint32_t width, uint32_t height, bool fullscreen) override;
    bool initWindow(uint32_t width, uint32_t height, bool fullscreen);
    bool initializeRenderResources(
        void* nativeWindowHandle,
        const RenderWindowOutputState& initialOutput);
    void shutdown() override;
    // Render-thread owner releases D3D12/world/GPU state. SDL window teardown
    // remains on the main thread and is performed by shutdown().
    void shutdownRenderResources();

    void beginFrame() override;
    void endFrame() override;
    void resize(uint32_t width, uint32_t height) override;

    void drawQuad(float x, float y, float w, float h, uint32_t color) override;
    void drawLine(float startX, float startY, float endX, float endY,
                  float width, uint32_t startColor, uint32_t endColor) override;
    void drawTexture(const RawTexture* tex, float x, float y, float w, float h, uint32_t tint = COLOR_WHITE) override;
    void drawText(const container::String& text, float x, float y, uint32_t color) override;
    void drawText(Font* font, const container::String& text, float x, float y, uint32_t color) override;
    void drawRect(float x, float y, float w, float h, uint32_t color) override;
    void drawBorder(float x, float y, float w, float h, uint32_t color, int thickness = 1) override;
    void drawTextureRegion(const RawTexture* tex, int srcLeft, int srcTop, int srcRight, int srcBottom,
                           int srcTexW, int srcTexH,
                           float dstX, float dstY, float dstW, float dstH,
                           uint32_t tint) override;

    void winRepaint() override;

    // Kept outside the legacy 2D Renderer interface. RendererSubsystem owns
    // the frame ordering and invokes this before UI submission.
    //
    // This is the formal game/render boundary: callers submit only immutable
    // frame data. ECS pointers, packed W3D objects, and D3D12 handles cannot
    // cross it.
    [[nodiscard]] bool prepareWorldSnapshot(
        render::WorldRenderSnapshot snapshot);
    // Renderer-local display policy. Disabling this collapses every logical
    // endpoint pair to the newest prepared endpoint without changing the
    // simulation tick rate or snapshot production.
    void setWorldInterpolationEnabled(bool enabled) noexcept;
    // Render-owner CPU publication boundary. Called every present frame even
    // when no new logic snapshot is accepted, so completed model/animation
    // work cannot stall behind snapshot preparation or simulation pause.
    void pumpWorldCpuResourceCompletions();
    void setRenderViewState(render::RenderViewState view);
    void setPresentationCameraOverride(
        render::PresentationCameraOverride cameraOverride);
    [[nodiscard]] bool hasPreparedWorld() const noexcept;
    // Render-thread sample used by RendererSubsystem's value-only feedback
    // mailbox. Callers outside the render owner must use that mailbox rather
    // than reading this mutable renderer state directly.
    [[nodiscard]] std::optional<render::RenderViewState>
    currentRenderViewState() const noexcept;
    [[nodiscard]] PreparedWorldRenderResult renderPreparedWorldWithStatus(
        TextureManager* objectIconTextures = nullptr);
    void setTacticalRadarPanel(float left, float top, float width,
                               float height, bool visible) noexcept;
    size_t renderPreparedWorld(TextureManager* objectIconTextures = nullptr);
    // Retires the current session-owned world at the app safe boundary.
    // CPU preparation and GPU use are joined before frame and asset owners
    // are released, so shell/result frames cannot retain the previous map.
    void retireWorldPresentation(
        render::WorldPreparationStamp retiredWorld);
    [[nodiscard]] uint64_t worldPresentationEpoch() const noexcept;

    // Quality authorities are applied explicitly at the safe frame boundary,
    // before FX/world submission. FX catalog submission must not reconfigure
    // display state or silently replace the session-frozen Feature revision.
    void applyRenderFeatureQuality(
        const ResolvedRenderFeatureSnapshot& feature);
    void applyRenderDisplaySettings(
        const ResolvedRenderDisplaySnapshot& display);
    // Render-owner side of the Main->Render output contract. It accepts only
    // newer observed revisions and never calls SDL.
    [[nodiscard]] bool applyWindowOutputState(
        const RenderWindowOutputState& output);
    [[nodiscard]] bool fxaaAvailable() const noexcept;
    // Non-UI validation/profile ingress for the experimental GPU particle
    // presentation gate. A benchmark owner publishes a monotonic revision;
    // gameplay, authored content and the quality menu do not own this value.
    void configureGpuParticlePresentationQualification(
        const render::GpuParticlePresentationQualification& qualification);

    // Lossless FX events have their own ordered stream. They are admitted at
    // this boundary but do not advance FxRuntime until the matching world
    // endpoint reaches the displayed side of the A/B timeline.
    void configureFxContent(container::SharedPtr<const fx::ParticleSystemCatalog> particles,
                             container::SharedPtr<const fx::FxListCatalog> fxLists);
    void configureFxContent(
        container::SharedPtr<const fx::ParticleSystemCatalog> particles,
        container::SharedPtr<const fx::FxListCatalog> fxLists,
        const RenderOperationalBudget& budget);
    void configureFxContent(
        container::SharedPtr<const fx::ParticleSystemCatalog> particles,
        container::SharedPtr<const fx::FxListCatalog> fxLists,
        const RenderGameDataSettings& settings);
    void submitFxSnapshot(const fx::FxPresentationSnapshot& snapshot);
    [[nodiscard]] container::Vector<fx::FxSoundCommand> takeFxSoundCommands();
    [[nodiscard]] container::Vector<render::RenderAnimationCompletionFeedback>
    takeAnimationCompletions();
    void clearFxPresentation();
    void submitGroundDecalPresentation(
        const render::GroundDecalPresentationBatch& batch);
    void clearGroundDecalPresentation(uint64_t presentationEpoch = 0);

    // Temporary diagnostic producer retained for asset validation. It routes
    // through the same snapshot APIs as a real game extraction.
    void prepareDebugWorld(float elapsedSeconds);
    void renderDebugWorld(float elapsedSeconds);

    // Consumes the backend-neutral result of WorldRenderPipeline and emits
    // static W3D draw packets for every GPU-ready visible instance.
    size_t renderWorldFrame(const render::PreparedWorldFrame& frame,
                            TextureManager* objectIconTextures = nullptr);

    // Latest renderer-local CAMERA_*_SLAVE_MODE pose for the application's
    // audio listener hand-off. It is an observer-only value: callers must
    // match the active GameSession presentation epoch, and it never exposes
    // a GameSession, ECS entity, Drawable, or mutable logic camera.
    [[nodiscard]] std::optional<render::RenderCameraSnapshot>
    scriptCameraSlaveListenerOverride(uint64_t expectedPresentationEpoch) const noexcept;
    [[nodiscard]] std::optional<render::WorldFrameRenderStats>
    lastWorldFrameStats() const noexcept;

    // Renderer-only general decal ingress. Callers provide already detached,
    // terrain-conformed projector values; epoch ordering prevents stale map
    // work from reviving decals after a session replacement.
    bool submitGroundDecal(uint64_t presentationEpoch,
                           render::GroundProjectorInstance decal);
    void clearGroundDecals(uint64_t presentationEpoch = 0);

    // Select a real VFS W3D for the diagnostic world pass. The asset is CPU
    // parsed immediately and its immutable GPU representation is uploaded in
    // the next beginFrame recording window.
    bool setDebugWorldAsset(const container::String& source);
    // Diagnostic terrain still enters as renderer-neutral snapshot data. Map
    // parsing remains a game/logic responsibility.
    bool setDebugWorldTerrain(container::SharedPtr<const render::TerrainRenderSnapshot> terrain);
    [[nodiscard]] container::SharedPtr<const render::TerrainRenderSnapshot>
    debugWorldTerrainSnapshot() const noexcept;
    void setDebugWorldVisibility(bool enabled) noexcept;
    void focusDebugWorldCamera(math::vec3 target, float distance) noexcept;
    // Applies detached presentation input only to the renderer-owned
    // diagnostic camera. The normal GameSession camera and ECS remain
    // untouched.
    [[nodiscard]] bool applyDebugWorldCameraInput(
        const GameCameraInput& input, float deltaSeconds) noexcept;
    [[nodiscard]] bool zoomDebugWorldCamera(float wheelUnits) noexcept;
    void setDebugWorldAnimation(container::String animationState);
    // Renderer-only material showcase used by the local Debug launch script.
    // It never changes GameSession/ECS state or ships in non-Debug builds.
    void setDebugMaterialEffects(bool enabled) noexcept;
    // F1's world-side counterpart to the UI skeleton view.  This remains a
    // renderer-local diagnostic state and never crosses the game snapshot
    // boundary.
    void setWorldSkeletonMode(bool enabled);
    // F3's world-side material-isolation diagnostic. The normal textured PSO
    // samples only the base SRV while F1 skeleton mode remains authoritative.
    void setWorldTextureOnlyMode(bool enabled);

    // Queues a screenshot of the next presented frame. Completion is
    // asynchronous and reported through the renderer log from endFrame.
    bool captureScreenshot(const container::String& filename);

    void showCursor(bool show);
    void setCursorVisible(bool visible);

    void setVirtualResolution(int vw, int vh) override;
    float getScaleX() const override {
        const float width = static_cast<float>(
            m_logicalWidth.load(std::memory_order_acquire));
        return width > 0.0f && m_uiCanvasW > 0.0f
            ? width / m_uiCanvasW : 1.0f;
    }
    float getScaleY() const override {
        const float height = static_cast<float>(
            m_logicalHeight.load(std::memory_order_acquire));
        return height > 0.0f && m_uiCanvasH > 0.0f
            ? height / m_uiCanvasH : 1.0f;
    }
    float getUiCanvasWidth() const override {
        return m_uiCanvasW;
    }
    float getUiCanvasHeight() const override {
        return m_uiCanvasH;
    }
    float getUiAuthoredWidth() const override {
        return static_cast<float>(std::max(m_virtualW, 1));
    }
    float getUiAuthoredHeight() const override {
        return static_cast<float>(std::max(m_virtualH, 1));
    }
    float getUiViewportWidth() const override { return m_uiCanvasW; }
    float getUiViewportHeight() const override { return m_uiCanvasH; }
    float getUiCanvasOffsetX() const override { return 0.0f; }
    float getUiCanvasOffsetY() const override { return 0.0f; }
    float getPresentationScaleX() const override {
        const float pixels = static_cast<float>(
            m_width.load(std::memory_order_acquire));
        return pixels > 0.0f && m_uiCanvasW > 0.0f
            ? pixels / m_uiCanvasW : 1.0f;
    }
    float getPresentationScaleY() const override {
        const float pixels = static_cast<float>(
            m_height.load(std::memory_order_acquire));
        return pixels > 0.0f && m_uiCanvasH > 0.0f
            ? pixels / m_uiCanvasH : 1.0f;
    }

    void getWindowSize(int& w, int& h) const override {
        w = static_cast<int>(m_logicalWidth.load(std::memory_order_acquire));
        h = static_cast<int>(m_logicalHeight.load(std::memory_order_acquire));
    }

    // Access to DX12 device for texture management
    engine::d3d12::D3D12Device& getD3D12Device() { return m_d3d12; }

    void* getSDLRenderer() const override { return nullptr; }  // No SDL renderer in native DX12
    SDL_Window* getSDLWindow() const { return m_window; }

private:
    void updateUiViewport() noexcept;
    struct WorldAssetRuntime;
    struct WorldEffectsPassResult final {
        size_t projectileTrailSegmentCount = 0;
        size_t typedFxTriangleCount = 0;
        size_t particleDrawCount = 0;
        size_t projectedShadowCount = 0;
        uint64_t projectorPrepareMicroseconds = 0;
        bool projectorPrepareSkipped = false;
    };
    struct WorldPostPassResult final {
        size_t smudgeDrawCount = 0;
        size_t snowflakeCount = 0;
        bool fxaaRendered = false;
    };
    struct WorldFrameFinalizeInput final {
        render::WorldPassTimingRenderStats passTimings;
        size_t preparedDrawCount = 0;
        WorldEffectsPassResult effects;
        WorldPostPassResult post;
    };

    [[nodiscard]] bool resizePresentationTargets(
        uint32_t width, uint32_t height);
    [[nodiscard]] bool synchronizeWorldPresentationEpoch(
        uint64_t presentationEpoch, bool fxIngress, bool forceReset = false);
    void consumeDisplayedFxSnapshot(
        const fx::FxPresentationSnapshot& snapshot);
    void releaseFxSnapshotsThrough(
        uint64_t presentationEpoch, uint64_t simulationFrame);

    void updateWorldParticlePass(
        const render::PreparedWorldFrame& frame,
        float renderDeltaSeconds,
        render::ParticleRenderDrawList& drawList);
    void prepareWorldTransientStreams(
        const render::PreparedWorldFrame& frame);
    void prepareWorldParticleDrawPass(
        const render::PreparedWorldFrame& frame,
        const render::RenderCameraSnapshot& presentationCamera,
        render::ParticleRenderDrawList& drawList);
    void publishWorldTerrainResources(
        const render::PreparedWorldFrame& frame,
        const render::RenderCameraSnapshot& presentationCamera);
    void prepareWorldTerrainPackets(
        const render::PreparedWorldFrame& frame,
        const render::RenderCameraSnapshot& presentationCamera,
        float visualTimeSeconds);
    void prepareWorldSkyboxPass(
        const render::SkyboxRenderState& skybox,
        const render::RenderCameraSnapshot& presentationCamera,
        float visualTimeSeconds);
    void prepareWorldTerrainBridgePackets(
        const render::PreparedWorldFrame& frame,
        float visualTimeSeconds);
    void prepareWorldViewVisibility(
        const render::PreparedWorldFrame& frame,
        const render::RenderCameraSnapshot& camera);
    void prepareWorldObjectPackets(
        const render::PreparedWorldFrame& frame,
        const render::TreeSwayRenderState& treeSway,
        float visualTimeSeconds,
        container::Span<const uint32_t> instanceSubset = {},
        bool currentEndpointOnly = false);
    [[nodiscard]] size_t prepareWorldOverlayPackets(
        const render::PreparedWorldFrame& frame,
        float visualTimeSeconds);
    void prepareWorldGroundProjectors(
        const render::PreparedWorldFrame& frame,
        const render::RenderCameraSnapshot& presentationCamera,
        const render::WorldLightEnvironment& lightEnvironment);
    [[nodiscard]] size_t renderWorldOpaqueGeometryPass(
        const render::PreparedWorldFrame& frame,
        const render::RenderCameraSnapshot& presentationCamera,
        const render::WorldLightEnvironment& lightEnvironment,
        float visualTimeSeconds,
        size_t drawCount);
    [[nodiscard]] WorldEffectsPassResult
    renderWorldEffectsAndOverlayPass(
        const render::PreparedWorldFrame& frame,
        const render::RenderCameraSnapshot& presentationCamera,
        render::ParticleRenderDrawList& particleDrawList,
        float renderDeltaSeconds,
        size_t modelRayPacketCount);
    [[nodiscard]] WorldPostPassResult
    renderWorldPostAndClientOverlayPass(
        const render::PreparedWorldFrame& frame,
        const render::RenderCameraSnapshot& presentationCamera,
        render::ParticleRenderDrawList& particleDrawList,
        const render::ClientOptionsRenderState& clientOptions,
        const render::WeatherRenderState& weather,
        TextureManager* objectIconTextures);
    [[nodiscard]] size_t finalizeWorldFrameStats(
        const render::PreparedWorldFrame& frame,
        const render::ParticleRenderDrawList& particleDrawList,
        const WorldFrameFinalizeInput& input);

    SDL_Window* m_window = nullptr;
    std::atomic<uint32_t> m_logicalWidth{
        engine::presentation_defaults::VIRTUAL_WIDTH};
    std::atomic<uint32_t> m_logicalHeight{
        engine::presentation_defaults::VIRTUAL_HEIGHT};
    uint64_t m_lastWindowOutputRevision = 0;
    bool m_renderResourcesInitialized = false;
    engine::d3d12::D3D12Device m_d3d12;
    container::UniquePtr<render::WorldRenderer> m_worldRenderer;
    container::UniquePtr<WorldAssetRuntime> m_worldAssets;
    bool m_worldInterpolationEnabled = true;
    struct TacticalRadarPanelState final {
        float left = 0.0f;
        float top = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        bool visible = false;
    } m_tacticalRadarPanel;
    container::Vector<render::RenderAnimationCompletionFeedback>
        m_pendingAnimationCompletions;
    // Lossless-by-key endpoint acknowledgement. One high-water generation
    // per ObjectId/channel avoids both the completion lane's 4096 oldest-drop
    // policy and an unbounded frame history while the main thread is busy.
    container::HashMap<uint64_t, render::RenderAnimationCompletionFeedback>
        m_pendingAnimationEndpointAdmissions;
    int m_virtualW = engine::presentation_defaults::VIRTUAL_WIDTH;
    int m_virtualH = engine::presentation_defaults::VIRTUAL_HEIGHT;
    float m_uiCanvasW = static_cast<float>(
        engine::presentation_defaults::VIRTUAL_WIDTH);
    float m_uiCanvasH = static_cast<float>(
        engine::presentation_defaults::VIRTUAL_HEIGHT);
    float m_uiOffsetX = 0.0f;
    float m_uiOffsetY = 0.0f;
    container::String m_pendingScreenshotFilename;

    struct UiSrvCacheEntry final {
        uint32_t srvIndex = UINT32_MAX;
        uint64_t lastUsedFrame = 0;
    };

    struct UiSrvCacheLifecycleCounters final {
        uint64_t cacheHits = 0;
        uint64_t cacheMisses = 0;
        uint64_t published = 0;
        uint64_t failures = 0;
        uint64_t evictions = 0;
        uint64_t resets = 0;
    };

    // CPU resource identities are monotonic, preventing allocator address
    // reuse after destruction/reload from aliasing an old GPU descriptor.
    // Idle entries are retired fence-safely so hot reload remains bounded.
    container::HashMap<uint64_t, UiSrvCacheEntry> m_textureCache;
    container::HashMap<uint64_t, UiSrvCacheEntry> m_glyphCache;
    UiSrvCacheLifecycleCounters m_uiTextureLifecycle;
    UiSrvCacheLifecycleCounters m_uiGlyphLifecycle;
    uint64_t m_uiSrvCacheFrame = 0;
    uint64_t m_uiSrvPressureBlockedUntilFrame = 0;

    uint32_t getOrCreateTextureSrv(const RawTexture* tex);
    uint32_t getOrCreateGlyphSrv(uint64_t glyphIdentity, const void* pixels, uint32_t w, uint32_t h);
    void processUiSrvInvalidations();
    void pruneUiSrvCaches();
    void releaseUiSrvCaches();
    [[nodiscard]] bool makeUiSrvCacheRoom(
        container::HashMap<uint64_t, UiSrvCacheEntry>& cache,
        size_t budget);
};

} // namespace engine
