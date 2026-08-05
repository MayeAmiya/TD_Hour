#include "DX12RendererWorldAssetRuntime.h"
#include "UiSrvInvalidation.h"
#include "core/constants/Paths.h"
#include "core/platform/runtime_threads.h"

#include <SDL3/SDL.h>

#include <utility>

namespace engine {

namespace render {
namespace {

static_assert(
    ground_decals::performance_limits::kHardMaximumInstancesPerFrame ==
    render_game_data_limits::kMaximumGroundProjectorsPerFrame);
static_assert(
    ground_decals::performance_limits::kHardMaximumResidentTextures ==
    render_game_data_limits::kMaximumGroundProjectorTextures);
static_assert(
    particle_render::performance_limits::kHardMaximumSourceParticles ==
    render_game_data_limits::kMaximumParticles);

} // namespace

// Small renderer-local owner for RayEffect and Tracer typed FX.
// It is intentionally transient and bounded; commands contain detached world
// fallbacks, so source-object deletion cannot cut off an admitted effect.

} // namespace render

DX12Renderer::DX12Renderer() = default;

void DX12Renderer::setTacticalRadarPanel(
    float left, float top, float width, float height, bool visible) noexcept {
    m_tacticalRadarPanel = {
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .visible = visible,
    };
}

DX12Renderer::~DX12Renderer() {
    shutdown();
}

bool DX12Renderer::init(uint32_t width, uint32_t height, bool fullscreen) {
    // Compatibility entry used by focused standalone callers. Production
    // uses RendererSubsystem's split Main-window/Render-device bootstrap.
    // Keeping the owner assertion here prevents this convenience path from
    // reintroducing SDL queries on a render or worker thread.
    if (!platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Main) ||
        !initWindow(width, height, fullscreen)) {
        return false;
    }
    SDL_Window* const window = m_window;
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    void* const nativeWindowHandle = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    int logicalWidth = 0;
    int logicalHeight = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    if (!nativeWindowHandle ||
        !SDL_GetWindowSize(window, &logicalWidth, &logicalHeight) ||
        !SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight) ||
        logicalWidth <= 0 || logicalHeight <= 0 ||
        pixelWidth <= 0 || pixelHeight <= 0) {
        return false;
    }
    const RenderWindowOutputState initialOutput{
        .logicalWidth = static_cast<uint32_t>(logicalWidth),
        .logicalHeight = static_cast<uint32_t>(logicalHeight),
        .pixelWidth = static_cast<uint32_t>(pixelWidth),
        .pixelHeight = static_cast<uint32_t>(pixelHeight),
        .outputWidth = static_cast<uint32_t>(logicalWidth),
        .outputHeight = static_cast<uint32_t>(logicalHeight),
        .displayMode = fullscreen
            ? RenderDisplayMode::BorderlessFullscreen
            : RenderDisplayMode::Windowed,
        .revision = 1,
    };
    return initializeRenderResources(nativeWindowHandle, initialOutput);
}

bool DX12Renderer::initWindow(
    uint32_t width, uint32_t height, bool fullscreen) {
    TD_LOG_INFO("[DX12Renderer] Initializing SDL window...");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        TD_LOG_ERROR("[DX12Renderer] SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    uint32_t flags = SDL_WINDOW_RESIZABLE;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;

    m_window = SDL_CreateWindow(WINDOW_TITLE.data(),
        static_cast<int>(width), static_cast<int>(height), flags);
    if (!m_window) {
        TD_LOG_ERROR("[DX12Renderer] SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    TD_LOG_INFO("[DX12Renderer] Window created: {}x{}", width, height);

    m_width = width;
    m_height = height;
    m_logicalWidth = width;
    m_logicalHeight = height;
    updateUiViewport();
    m_fullscreen = fullscreen;
    SDL_ShowCursor();
    SDL_StartTextInput(m_window);
    return true;
}

bool DX12Renderer::initializeRenderResources(
    void* nativeWindowHandle,
    const RenderWindowOutputState& initialOutput) {
    if (!m_window || !nativeWindowHandle ||
        !initialOutput.validPixelExtent() ||
        !initialOutput.validLogicalExtent()) {
        TD_LOG_ERROR(
            "[DX12Renderer] Cannot initialize D3D12 without a cached Main-thread window state");
        return false;
    }
    const uint32_t width = initialOutput.pixelWidth;
    const uint32_t height = initialOutput.pixelHeight;
    const bool fullscreen = initialOutput.displayMode !=
        RenderDisplayMode::Windowed;
    TD_LOG_INFO("[DX12Renderer] Initializing native DX12 resources...");

    if (!m_d3d12.init(nativeWindowHandle, width, height, fullscreen)) {
        TD_LOG_ERROR("[DX12Renderer] D3D12Device init failed");
        return false;
    }

    m_worldRenderer = std::make_unique<render::WorldRenderer>(m_d3d12);
    if (!m_worldRenderer->init()) {
        TD_LOG_ERROR("[DX12Renderer] WorldRenderer init failed");
        m_worldRenderer.reset();
        m_d3d12.shutdown();
        return false;
    }
    m_worldAssets = std::make_unique<WorldAssetRuntime>(m_d3d12);
    m_worldAssets->view.worldInterpolation.setEnabled(
        m_worldInterpolationEnabled);
    if (!m_worldAssets->particleRenderer.isInitialized()) {
        TD_LOG_ERROR("[DX12Renderer] ParticleRenderer init failed");
        m_worldAssets.reset();
        m_worldRenderer.reset();
        m_d3d12.shutdown();
        return false;
    }
    if (!m_worldAssets->projectileTrailRenderer.isInitialized()) {
        TD_LOG_ERROR("[DX12Renderer] ProjectileTrailRenderer init failed");
        m_worldAssets.reset();
        m_worldRenderer.reset();
        m_d3d12.shutdown();
        return false;
    }
    if (!m_worldAssets->trackMarkRenderer.isInitialized()) {
        TD_LOG_ERROR("[DX12Renderer] TrackMarkRenderer init failed");
        m_worldAssets.reset();
        m_worldRenderer.reset();
        m_d3d12.shutdown();
        return false;
    }
    if (!m_worldAssets->waypointRenderer.isInitialized()) {
        TD_LOG_ERROR("[DX12Renderer] WaypointRenderer init failed");
        m_worldAssets.reset();
        m_worldRenderer.reset();
        m_d3d12.shutdown();
        return false;
    }
    if (!m_worldAssets->groundProjectorRenderer.isInitialized()) {
        TD_LOG_ERROR("[DX12Renderer] GroundProjectorRenderer init failed");
        m_worldAssets.reset();
        m_worldRenderer.reset();
        m_d3d12.shutdown();
        return false;
    }
    if (!m_worldAssets->fx.typed.worldRenderer().isInitialized()) {
        TD_LOG_ERROR("[DX12Renderer] Typed FX world renderer init failed");
        m_worldAssets.reset();
        m_worldRenderer.reset();
        m_d3d12.shutdown();
        return false;
    }

    m_width = width;
    m_height = height;
    m_logicalWidth = initialOutput.logicalWidth;
    m_logicalHeight = initialOutput.logicalHeight;
    updateUiViewport();
    m_fullscreen = fullscreen;
    m_lastWindowOutputRevision = initialOutput.revision;
    m_worldAssets->output.displayWidth = initialOutput.outputWidth;
    m_worldAssets->output.displayHeight = initialOutput.outputHeight;
    m_worldAssets->output.displayMode = initialOutput.displayMode;
    m_worldAssets->output.displayRefreshRateHz = initialOutput.refreshRateHz;
    m_worldAssets->output.appliedOutputWidth = initialOutput.outputWidth;
    m_worldAssets->output.appliedOutputHeight = initialOutput.outputHeight;
    m_worldAssets->output.appliedOutputMode = initialOutput.displayMode;
    m_worldAssets->output.appliedOutputRefreshRateHz =
        initialOutput.refreshRateHz;
    m_worldAssets->output.displayPixelWidth = initialOutput.pixelWidth;
    m_worldAssets->output.displayPixelHeight = initialOutput.pixelHeight;
    m_worldAssets->output.appliedOutputRevision = initialOutput.revision;
    m_worldAssets->output.hasAppliedOutput = true;
    m_worldAssets->output.displayPixelExtentValid = true;
    m_worldAssets->output.lastOutputApplySucceeded = true;

    int lw = 0, lh = 0;
    getWindowSize(lw, lh);
    TD_LOG_INFO(
        "[DX12Renderer] Init output={}x{} WND-authored={}x{} scale=({:.3f},{:.3f})",
        lw, lh, m_virtualW, m_virtualH,
        lw > 0 ? static_cast<float>(lw) / static_cast<float>(m_virtualW) : 1.0f,
        lh > 0 ? static_cast<float>(lh) / static_cast<float>(m_virtualH) : 1.0f);

    TD_LOG_INFO("[DX12Renderer] Init complete (native DX12)");
    m_renderResourcesInitialized = true;
    return true;
}

void DX12Renderer::setWorldInterpolationEnabled(bool enabled) noexcept {
    m_worldInterpolationEnabled = enabled;
    if (m_worldAssets) {
        m_worldAssets->view.worldInterpolation.setEnabled(enabled);
    }
}

void DX12Renderer::shutdown() {
    shutdownRenderResources();
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    TD_LOG_INFO("[DX12Renderer] Shutdown complete");
}

void DX12Renderer::shutdownRenderResources() {
    if (!m_renderResourcesInitialized) return;
    m_pendingAnimationCompletions.clear();
    m_pendingAnimationEndpointAdmissions.clear();
    TD_LOG_INFO("[DX12Renderer] Shutting down...");
    if (m_worldAssets &&
        (m_worldAssets->debugWorld.preparationPending() ||
         m_worldAssets->submittedPreparationPending ||
         m_worldAssets->pipeline.isPreparing())) {
        static_cast<void>(m_worldAssets->pipeline.finishPreparation());
        m_worldAssets->debugWorld.cancelPreparation();
        m_worldAssets->submittedPreparationPending = false;
    }
    // Keep world GPU resources alive until every command list that references
    // them has completed, then release them before tearing down the device.
    if (m_worldRenderer || m_worldAssets ||
        !m_textureCache.empty() || !m_glyphCache.empty()) {
        if (!m_d3d12.waitIdle()) {
            TD_LOG_WARN(
                "[DX12Renderer] GPU idle wait failed; continuing degraded teardown");
        }
    }
    if (m_worldAssets) {
        m_worldAssets->pipeline.resetPresentationEpoch(0);
        m_worldAssets->fx.runtime.reset();
        m_worldAssets->fx.typed.worldRenderer().shutdown();
        m_worldAssets->groundProjectorRenderer.shutdown();
        m_worldAssets->waypointRenderer.shutdown();
        m_worldAssets->trackMarkRenderer.shutdown();
        m_worldAssets->projectileTrailRenderer.shutdown();
        m_worldAssets->particleRenderer.shutdown();
        m_worldAssets->terrainUploads.resetState();
        m_worldAssets->terrain.reset();
        m_worldAssets->residency.assets.clear();
        m_worldAssets->residency.animations.clear();
        m_worldAssets->residency.skyboxTextureOverrides.reset(*m_worldAssets->residency.textures);
        m_worldAssets->residency.treeTextureOverrides.reset(*m_worldAssets->residency.textures);
        // All SRV-bearing world owners are gone at this point, so shutdown is
        // also a valid source-generation boundary: discard decoded payloads,
        // aliases, the VFS index and negative lookups together with GPU SRVs.
        m_worldAssets->residency.textures->resetSourceCache();
    }
    m_worldAssets.reset();
    m_worldRenderer.reset();
    // These keys point into the descriptor heap owned by the current device
    // generation. They must never survive shutdown/re-init.
    releaseUiSrvCaches();
    m_uiSrvCacheFrame = 0;
    m_uiSrvPressureBlockedUntilFrame = 0;
    m_pendingScreenshotFilename.clear();
    m_d3d12.shutdown();
    m_renderResourcesInitialized = false;
}

bool DX12Renderer::resizePresentationTargets(
    uint32_t width, uint32_t height) {
    if (width == 0u || height == 0u) return false;
    if (!m_d3d12.resize(width, height)) return false;

    m_width = width;
    m_height = height;
    if (m_worldAssets) {
        m_worldAssets->output.displayPixelWidth = width;
        m_worldAssets->output.displayPixelHeight = height;
        m_worldAssets->output.displayPixelExtentValid = true;
    }

    int lw = 0, lh = 0;
    getWindowSize(lw, lh);
    TD_LOG_INFO("[DX12Renderer] Resize pixel={}x{} logical={}x{}", width, height, lw, lh);
    return true;
}

void DX12Renderer::resize(uint32_t width, uint32_t height) {
    static_cast<void>(width);
    static_cast<void>(height);
    TD_LOG_WARN(
        "[DX12Renderer] Rejected legacy resize without a Main-thread observed output revision");
}

bool DX12Renderer::applyWindowOutputState(
    const RenderWindowOutputState& output) {
    if (!m_worldAssets || output.revision == 0u ||
        output.revision <= m_lastWindowOutputRevision) {
        return output.revision != 0u;
    }
    m_lastWindowOutputRevision = output.revision;
    const bool requestedApply = output.requestRevision != 0u;
    if (requestedApply) {
        ++m_worldAssets->output.outputApplyAttempts;
        m_worldAssets->output.lastOutputAttemptRevision = output.requestRevision;
    }
    if (!output.applySucceeded || !output.validPixelExtent() ||
        !output.validLogicalExtent()) {
        if (requestedApply) ++m_worldAssets->output.outputApplyFailed;
        m_worldAssets->output.lastOutputApplySucceeded = false;
        return false;
    }

    if (!resizePresentationTargets(output.pixelWidth, output.pixelHeight)) {
        if (requestedApply) ++m_worldAssets->output.outputApplyFailed;
        m_worldAssets->output.lastOutputApplySucceeded = false;
        return false;
    }

    m_logicalWidth = output.logicalWidth;
    m_logicalHeight = output.logicalHeight;
    updateUiViewport();
    m_fullscreen = output.displayMode != RenderDisplayMode::Windowed;
    m_worldAssets->output.appliedOutputMode = output.displayMode;
    m_worldAssets->output.appliedOutputWidth = output.outputWidth;
    m_worldAssets->output.appliedOutputHeight = output.outputHeight;
    m_worldAssets->output.appliedOutputRefreshRateHz = output.refreshRateHz;
    m_worldAssets->output.displayPixelWidth = output.pixelWidth;
    m_worldAssets->output.displayPixelHeight = output.pixelHeight;
    m_worldAssets->output.displayPixelExtentValid = true;
    m_worldAssets->output.hasAppliedOutput = true;
    m_worldAssets->output.appliedOutputRevision = output.revision;
    m_worldAssets->output.lastOutputApplySucceeded = true;
    if (requestedApply) ++m_worldAssets->output.outputApplySucceeded;
    return true;
}


} // namespace engine
