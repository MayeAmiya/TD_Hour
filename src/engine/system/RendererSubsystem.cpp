#include "core/container/container_types.h"
#include "RendererSubsystem.h"
#include "debug/debug.h"
#include "DX12Renderer.h"
#include "Renderer.h"
#include "CommandLine.h"
#include "presentation/camera/GameCameraInput.h"
#include "presentation/fx/content/FxListCatalog.h"
#include "presentation/fx/content/ParticleSystemCatalog.h"
#include "presentation/fx/runtime/FxPresentationSnapshot.h"
#include "presentation/render/RenderGameDataSettings.h"
#include "engine/renderer/runtime/RenderDefaults.h"
#include "presentation/render/PresentationDefaults.h"
#include "core/constants/Paths.h"
#include "core/platform/runtime_threads.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

[[nodiscard]] bool retiredWorldDomain(
    uint64_t presentationEpoch, uint64_t sessionRevision,
    uint64_t retiredPresentationEpoch,
    uint64_t retiredSessionRevision) noexcept {
    return (retiredPresentationEpoch != 0u &&
            presentationEpoch <= retiredPresentationEpoch) ||
        (retiredSessionRevision != 0u &&
         sessionRevision != 0u &&
         sessionRevision <= retiredSessionRevision);
}

[[nodiscard]] bool sameObservedWindowOutput(
    const engine::RenderWindowOutputState& left,
    const engine::RenderWindowOutputState& right) noexcept {
    return left.logicalWidth == right.logicalWidth &&
        left.logicalHeight == right.logicalHeight &&
        left.pixelWidth == right.pixelWidth &&
        left.pixelHeight == right.pixelHeight &&
        left.outputWidth == right.outputWidth &&
        left.outputHeight == right.outputHeight &&
        left.refreshRateHz == right.refreshRateHz &&
        left.displayMode == right.displayMode;
}

[[nodiscard]] bool applySdlWindowOutput(
    SDL_Window* window, engine::RenderDisplayMode mode,
    uint32_t width, uint32_t height, uint32_t refreshRateHz) {
    if (!window) return false;
    if (mode == engine::RenderDisplayMode::Windowed) {
        if (!SDL_SetWindowFullscreen(window, false) ||
            !SDL_SetWindowFullscreenMode(window, nullptr)) {
            return false;
        }
        return SDL_SetWindowSize(
            window, static_cast<int>(std::max(1u, width)),
            static_cast<int>(std::max(1u, height)));
    }
    if (mode == engine::RenderDisplayMode::BorderlessFullscreen) {
        return SDL_SetWindowFullscreenMode(window, nullptr) &&
            SDL_SetWindowFullscreen(window, true);
    }

    const SDL_DisplayID displayId = SDL_GetDisplayForWindow(window);
    SDL_DisplayMode closest{};
    return displayId != 0u &&
        SDL_GetClosestFullscreenDisplayMode(
            displayId, static_cast<int>(std::max(1u, width)),
            static_cast<int>(std::max(1u, height)),
            static_cast<float>(refreshRateHz), false, &closest) &&
        SDL_SetWindowFullscreenMode(window, &closest) &&
        SDL_SetWindowFullscreen(window, true);
}

[[nodiscard]] std::pair<uint32_t, uint32_t> initialWindowExtent() noexcept {
    uint32_t width = engine::render_defaults::WINDOW_WIDTH;
    uint32_t height = engine::render_defaults::WINDOW_HEIGHT;
    const engine::CommandLine& commandLine =
        engine::CommandLine::instance();
    if (const auto resolution = commandLine.getResolutionParam()) {
        width = resolution->first;
        height = resolution->second;
    }
    if (commandLine.hasParam("render-width")) {
        width = static_cast<uint32_t>(std::max(
            1, commandLine.getIntParam("render-width", static_cast<int>(width))));
    }
    if (commandLine.hasParam("render-height")) {
        height = static_cast<uint32_t>(std::max(
            1, commandLine.getIntParam("render-height", static_cast<int>(height))));
    }
    return {width, height};
}

} // namespace

#if TD_DEBUG_ENABLED
namespace {

container::SharedPtr<const engine::render::TerrainRenderSnapshot>
makeRoadJunctionShowcaseTerrain() {
    auto terrain = std::make_shared<engine::render::TerrainRenderSnapshot>();
    terrain->revision = 1;
    terrain->layoutRevision = 1;
    terrain->width = 80;
    terrain->height = 60;
    terrain->cellWorldSize = 10.0f;
    terrain->heightWorldScale = 1.0f;
    terrain->heights.resize(
        static_cast<size_t>(terrain->width) * terrain->height, 0u);
    terrain->playableMinimum = {0.0f, 0.0f, 0.0f};
    terrain->playableMaximum = {790.0f, 590.0f, 0.0f};

    const auto road = [](float x0, float y0, float x1, float y1) {
        return engine::render::TerrainRoadRenderSegment{
            .styleName = "DirtRoad3",
            .textureName = "TRDirtRoad3.tga",
            .start = {x0, y0, 0.0f},
            .end = {x1, y1, 0.0f},
            .width = 33.0f,
            .widthInTexture = 0.95f,
        };
    };
    // Left T, middle Y, right cross. Shared endpoints deliberately exercise
    // the same graph/trim/patch path as authored map roads.
    terrain->roads = {
        road(180.0f, 300.0f, 80.0f, 300.0f),
        road(180.0f, 300.0f, 280.0f, 300.0f),
        road(180.0f, 300.0f, 180.0f, 410.0f),
        road(400.0f, 300.0f, 400.0f, 410.0f),
        road(400.0f, 300.0f, 310.0f, 220.0f),
        road(400.0f, 300.0f, 490.0f, 220.0f),
        road(620.0f, 300.0f, 520.0f, 300.0f),
        road(620.0f, 300.0f, 720.0f, 300.0f),
        road(620.0f, 300.0f, 620.0f, 200.0f),
        road(620.0f, 300.0f, 620.0f, 410.0f),
    };
    return terrain;
}

} // namespace
#endif

RendererSubsystem::RendererSubsystem() {
    setName("Renderer");
    m_preAttachOwnerThread = std::this_thread::get_id();
}

RendererSubsystem::~RendererSubsystem() {
    shutdown();
}

void RendererSubsystem::init() {
    TD_LOG_INFO("[Renderer] Initializing SDL3 + DX12...");

    if (m_renderOwnerState.load(std::memory_order_acquire) ==
        RenderOwnerState::Attached) {
        throw std::logic_error(
            "RendererSubsystem initialized while render owner is attached");
    }
    m_preAttachOwnerThread = std::this_thread::get_id();
    m_renderOwnerState.store(
        RenderOwnerState::PreAttach, std::memory_order_release);

    m_dx12Renderer = nullptr;
    m_snapshotQueue.reset();
    m_renderViewStates.reset();
    m_presentationCameraOverrides.reset();
    m_windowOutputRequests.reset();
    m_windowOutputStates.reset();
    m_mainWindowOutput = {};
    m_nativeWindowHandle = nullptr;
    m_nextWindowOutputRevision = 1;
    m_lastMainWindowRequestRevision = 0;
    m_lastQueuedWindowRequestRevision.store(0, std::memory_order_release);
    m_feedback.reset();
    m_deferredSnapshot.reset();
    m_submittedWorldPrepared = false;
    m_renderControls.reset();
    m_renderControlOverflowed.store(false, std::memory_order_release);
    m_renderControlRejected.store(false, std::memory_order_release);
    // F1 is the normal interactive control.  Keep a command-line equivalent
    // for repeatable map captures and automated graphics diagnosis.
    const engine::CommandLine& commandLine =
        engine::CommandLine::instance();
    m_worldSkeletonMode = commandLine.getBoolParam("world-skeleton", false);
    m_worldTextureOnlyMode = commandLine.getBoolParam(
        "world-texture-only", false);
#if TD_DEBUG_ENABLED
    const container::String debugWorldAsset =
        commandLine.getParam("debug-world-asset");
    const container::String debugWorldAnimation =
        engine::CommandLine::instance().getParam("debug-world-animation");
    const bool debugWorldVisibility =
        engine::CommandLine::instance().getBoolParam("debug-world-visibility", false);
    const bool debugMaterialEffects =
        engine::CommandLine::instance().getBoolParam("debug-material-effects", false);
    const bool debugRoadJunctions =
        engine::CommandLine::instance().getBoolParam("debug-road-junctions", false);
    const container::String debugFxList =
        engine::CommandLine::instance().getParam("debug-fx");
    const bool standaloneDebugFx = !commandLine.hasParam("direct-start") &&
        !debugFxList.empty();
    m_debugWorldEnabled = engine::CommandLine::instance().getBoolParam("debug-world", false) ||
                          !debugWorldAsset.empty() ||
                          debugWorldVisibility;
    m_debugWorldEnabled = m_debugWorldEnabled || standaloneDebugFx ||
        debugMaterialEffects || debugRoadJunctions;
#else
    m_debugWorldEnabled = false;
#endif
    m_debugWorldStart = std::chrono::steady_clock::now();

    auto renderer = std::make_unique<engine::DX12Renderer>();
    // Smooth confirmed movement and skeletal-pose endpoints by default. The
    // explicit opt-out remains useful for parity captures and diagnosis.
    const bool worldInterpolationEnabled =
        commandLine.getBoolParam("world-interpolation", true) &&
        !commandLine.getBoolParam("no-interpolation", false);
    renderer->setWorldInterpolationEnabled(worldInterpolationEnabled);
    const auto [initialWidth, initialHeight] = initialWindowExtent();
    if (!renderer->initWindow(initialWidth, initialHeight, false)) {
        TD_LOG_ERROR("[Renderer] SDL window init failed");
        return;
    }
    m_dx12Renderer = renderer.get();
    engine::Renderer::setInstance(std::move(renderer));

    SDL_Window* const window = m_dx12Renderer->getSDLWindow();
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    m_nativeWindowHandle = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    engine::RenderWindowOutputState initialOutput;
    if (!m_nativeWindowHandle ||
        !observeMainThreadWindow(initialOutput)) {
        TD_LOG_ERROR(
            "[Renderer] Main-thread window bootstrap observation failed");
        m_dx12Renderer = nullptr;
        engine::Renderer::setInstance(
            std::make_unique<engine::StubRenderer>());
        return;
    }
    initialOutput.revision = m_nextWindowOutputRevision++;
    m_mainWindowOutput = initialOutput;

    TD_LOG_INFO(
        "[Renderer] Main-thread SDL window ready logical={}x{} pixel={}x{} revision={}",
        initialOutput.logicalWidth, initialOutput.logicalHeight,
        initialOutput.pixelWidth, initialOutput.pixelHeight,
        initialOutput.revision);
#if TD_DEBUG_ENABLED
    if (m_debugWorldEnabled) {
        if (!debugWorldAsset.empty()) {
            TD_LOG_INFO("[Renderer] Debug world asset enabled: '{}'", debugWorldAsset);
        } else {
            TD_LOG_INFO("[Renderer] Debug world enabled (--debug-world=true)");
        }
    }
#endif
}

bool RendererSubsystem::initializeRenderBackend() {
    if (!m_dx12Renderer || !m_nativeWindowHandle ||
        !m_mainWindowOutput.validPixelExtent() ||
        !m_dx12Renderer->initializeRenderResources(
            m_nativeWindowHandle, m_mainWindowOutput)) {
        TD_LOG_ERROR("[Renderer] D3D12 backend init failed");
        return false;
    }
    m_dx12Renderer->setVirtualResolution(
        engine::presentation_defaults::VIRTUAL_WIDTH,
        engine::presentation_defaults::VIRTUAL_HEIGHT);
    m_dx12Renderer->setWorldSkeletonMode(m_worldSkeletonMode);
    m_dx12Renderer->setWorldTextureOnlyMode(m_worldTextureOnlyMode);

#if TD_DEBUG_ENABLED
    const container::String debugWorldAsset =
        engine::CommandLine::instance().getParam("debug-world-asset");
    const container::String debugWorldAnimation =
        engine::CommandLine::instance().getParam("debug-world-animation");
    const bool debugWorldVisibility = engine::CommandLine::instance()
        .getBoolParam("debug-world-visibility", false);
    const bool debugMaterialEffects = engine::CommandLine::instance()
        .getBoolParam("debug-material-effects", false);
    const bool debugRoadJunctions = engine::CommandLine::instance()
        .getBoolParam("debug-road-junctions", false);
    const container::String debugFxList =
        engine::CommandLine::instance().getParam("debug-fx");
    const bool standaloneDebugFx =
        !engine::CommandLine::instance().hasParam("direct-start") &&
        !debugFxList.empty();
    m_dx12Renderer->setDebugWorldAnimation(debugWorldAnimation);
    m_dx12Renderer->setDebugWorldVisibility(debugWorldVisibility);
    m_dx12Renderer->setDebugMaterialEffects(debugMaterialEffects);
    if (!debugWorldAsset.empty() &&
        !m_dx12Renderer->setDebugWorldAsset(debugWorldAsset)) {
        TD_LOG_WARN(
            "[Renderer] Falling back to diagnostic cube for '{}'",
            debugWorldAsset);
    }
    if (debugRoadJunctions) {
        if (!m_dx12Renderer->setDebugWorldTerrain(
                makeRoadJunctionShowcaseTerrain())) {
            TD_LOG_ERROR(
                "[Renderer] Debug road-junction terrain was rejected");
        }
    }
    if (standaloneDebugFx) {
        constexpr container::StringView particleRoots[] = {
            "data/ini/ParticleSystem"};
        constexpr container::StringView fxListRoots[] = {
            "data/ini/default/FXList", "data/ini/FXList"};
        auto particles =
            std::make_shared<engine::fx::ParticleSystemCatalog>();
        auto fxLists = std::make_shared<engine::fx::FxListCatalog>();
        container::String error;
        const bool particlesLoaded =
            particles->loadFromVfsLoadDirectories(particleRoots, &error);
        error.clear();
        const bool fxListsLoaded =
            fxLists->loadFromVfsLoadDirectories(fxListRoots, &error);
        if (particlesLoaded && fxListsLoaded) {
            fxLists->resolveReferences(*particles);
            if (fxLists->find(debugFxList)) {
                math::vec3 position{
                    engine::CommandLine::instance().getFloatParam(
                        "debug-fx-x", 0.0f),
                    engine::CommandLine::instance().getFloatParam(
                        "debug-fx-y", 0.0f),
                    engine::CommandLine::instance().getFloatParam(
                        "debug-fx-z", 1.0f),
                };
                m_dx12Renderer->configureFxContent(particles, fxLists);
                m_dx12Renderer->focusDebugWorldCamera(
                    position,
                    engine::CommandLine::instance().getFloatParam(
                        "debug-fx-camera-distance", 55.0f));
                engine::fx::FxPresentationSnapshot snapshot;
                snapshot.sessionEpoch =
                    std::numeric_limits<uint64_t>::max();
                snapshot.invocations.push_back({
                    .fxListName = debugFxList,
                    .anchorKind = engine::fx::
                        FxPresentationAnchorKind::WorldPosition,
                    .primary = {
                        .position = {
                            position.x(), position.y(), position.z()},
                    },
                    .overrideRadius = std::max(
                        0.0f,
                        engine::CommandLine::instance().getFloatParam(
                            "debug-fx-radius", 0.0f)),
                    .eventId = 1,
                    .variationSeed = 1,
                });
                m_dx12Renderer->submitFxSnapshot(snapshot);
            }
        }
    }
#endif

    TD_LOG_INFO(
        "[Renderer] D3D12 backend initialized on render thread (WND authored canvas {}x{}; output follows observed pixels)",
        engine::presentation_defaults::VIRTUAL_WIDTH,
        engine::presentation_defaults::VIRTUAL_HEIGHT);
    return true;
}

void RendererSubsystem::reset() {
    // Renderer state persists. In particular, reset must never reopen a
    // Stopped owner contract or make caller-thread D3D access legal again.
}

void RendererSubsystem::shutdown() {
    TD_LOG_INFO("[Renderer] Shutdown");
    m_renderOwnerState.store(
        RenderOwnerState::Stopped, std::memory_order_release);
    m_renderControls.close();
    m_windowOutputRequests.close();
    m_windowOutputStates.close();
    // Replacing the global instance destroys DX12Renderer while this
    // subsystem still controls the shutdown order, releasing WorldRenderer
    // before D3D12Device and SDL.
    m_dx12Renderer = nullptr;
    m_renderControlOverflowed.store(false, std::memory_order_release);
    m_renderViewStates.close();
    m_presentationCameraOverrides.close();
    m_nativeWindowHandle = nullptr;
    m_debugWorldEnabled.store(false, std::memory_order_release);
    m_worldSkeletonMode = false;
    m_worldTextureOnlyMode = false;
    // Do not reset the SPSC slots here: on a render-thread failure the main
    // producer may still be completing its current publication. init() resets
    // them only after both sides have stopped.
    m_deferredSnapshot.reset();
    m_submittedWorldPrepared = false;
    engine::Renderer::setInstance(std::make_unique<engine::StubRenderer>());
}

void RendererSubsystem::attachRenderThread() {
    if (!platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        throw std::logic_error(
            "RendererSubsystem render ownership attached from wrong thread");
    }
    RenderOwnerState expected = RenderOwnerState::PreAttach;
    if (!m_renderOwnerState.compare_exchange_strong(
            expected, RenderOwnerState::Attached,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        throw std::logic_error(
            "RendererSubsystem render ownership attached from invalid state");
    }
    m_renderControlOverflowed.store(false, std::memory_order_release);
    m_renderControlRejected.store(false, std::memory_order_release);
    if (!initializeRenderBackend()) {
        m_renderOwnerState.store(
            RenderOwnerState::Stopped, std::memory_order_release);
        m_renderControls.close();
        throw std::runtime_error("Renderer backend initialization failed");
    }
}

void RendererSubsystem::detachRenderThread() noexcept {
    if (!platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        rejectRenderControl();
        return;
    }
    m_renderOwnerState.store(
        RenderOwnerState::Stopped, std::memory_order_release);
    m_renderControls.close();
}

void RendererSubsystem::releaseRenderResourcesOnRenderThread() {
    if (!platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        throw std::logic_error(
            "Renderer resources released from wrong thread");
    }
    m_renderOwnerState.store(
        RenderOwnerState::Stopped, std::memory_order_release);
    m_renderControls.close();
    if (m_dx12Renderer) m_dx12Renderer->shutdownRenderResources();
}

bool RendererSubsystem::enqueueRenderControl(
    std::function<void()> command) {
    if (!command) return false;

    const RenderOwnerState ownerState =
        m_renderOwnerState.load(std::memory_order_acquire);
    if (ownerState == RenderOwnerState::PreAttach) {
        if (std::this_thread::get_id() != m_preAttachOwnerThread) {
            rejectRenderControl();
            return false;
        }
        command();
        return true;
    }
    if (ownerState == RenderOwnerState::Stopped) {
        rejectRenderControl();
        return false;
    }
    if (platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        command();
        return true;
    }
    if (m_renderControls.tryPush(std::move(command))) return true;
    if (m_renderControls.closed() ||
        m_renderOwnerState.load(std::memory_order_acquire) !=
            RenderOwnerState::Attached) {
        rejectRenderControl();
        return false;
    }
    if (!m_renderControls.closed()) {
        const bool alreadyOverflowed = m_renderControlOverflowed.exchange(
            true, std::memory_order_acq_rel);
        if (!alreadyOverflowed) {
            TD_LOG_ERROR(
                "[Renderer] Ordered render-control mailbox overflow; stopping instead of blocking a producer or dropping commands silently");
        }
    }
    return false;
}

void RendererSubsystem::rejectRenderControl() noexcept {
    const bool alreadyRejected = m_renderControlRejected.exchange(
        true, std::memory_order_acq_rel);
    if (!alreadyRejected) {
        TD_LOG_WARN(
            "[Renderer] Rejected render-control command outside its active owner contract");
    }
}

void RendererSubsystem::drainRenderControls() {
    if (!platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        throw std::logic_error(
            "Renderer controls drained from wrong thread");
    }
    if (m_renderOwnerState.load(std::memory_order_acquire) !=
        RenderOwnerState::Attached) {
        return;
    }
    if (m_renderControlOverflowed.exchange(false,
                                           std::memory_order_acq_rel)) {
        throw std::runtime_error(
            "ordered render-control mailbox overflowed");
    }
    static_cast<void>(m_renderControls.drain(
        [](std::function<void()> command) { command(); }));
}

void RendererSubsystem::requestResize(uint32_t width, uint32_t height) {
    static_cast<void>(width);
    static_cast<void>(height);
    if (!isMainWindowOwner()) {
        rejectRenderControl();
        return;
    }
    engine::RenderWindowOutputState observed;
    if (observeMainThreadWindow(observed)) {
        publishMainThreadWindowState(std::move(observed));
    }
}

engine::RendererInputViewport
RendererSubsystem::inputViewport() const noexcept {
    const engine::Renderer& renderer = engine::Renderer::instance();
    return {
        .width = m_mainWindowOutput.logicalWidth,
        .height = m_mainWindowOutput.logicalHeight,
        .uiScaleX = renderer.getScaleX(),
        .uiScaleY = renderer.getScaleY(),
        .fullscreen = m_mainWindowOutput.displayMode !=
            engine::RenderDisplayMode::Windowed,
    };
}

bool RendererSubsystem::isMainWindowOwner() const noexcept {
    return std::this_thread::get_id() == m_preAttachOwnerThread &&
        platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Main);
}

bool RendererSubsystem::observeMainThreadWindow(
    engine::RenderWindowOutputState& output,
    uint64_t requestRevision,
    bool applySucceeded) const noexcept {
    if (!isMainWindowOwner() || !m_dx12Renderer) return false;
    SDL_Window* const window = m_dx12Renderer->getSDLWindow();
    if (!window) return false;

    int logicalWidth = 0;
    int logicalHeight = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    if (!SDL_GetWindowSize(window, &logicalWidth, &logicalHeight) ||
        !SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight) ||
        logicalWidth <= 0 || logicalHeight <= 0 ||
        pixelWidth <= 0 || pixelHeight <= 0) {
        return false;
    }

    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    const bool fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0u;
    const SDL_DisplayMode* fullscreenMode =
        SDL_GetWindowFullscreenMode(window);
    const engine::RenderDisplayMode mode = !fullscreen
        ? engine::RenderDisplayMode::Windowed
        : fullscreenMode
            ? engine::RenderDisplayMode::ExclusiveFullscreen
            : engine::RenderDisplayMode::BorderlessFullscreen;
    const SDL_DisplayID displayId = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* currentMode = displayId != 0u
        ? SDL_GetCurrentDisplayMode(displayId) : nullptr;

    output = {
        .logicalWidth = static_cast<uint32_t>(logicalWidth),
        .logicalHeight = static_cast<uint32_t>(logicalHeight),
        .pixelWidth = static_cast<uint32_t>(pixelWidth),
        .pixelHeight = static_cast<uint32_t>(pixelHeight),
        .outputWidth = mode != engine::RenderDisplayMode::Windowed &&
                currentMode && currentMode->w > 0
            ? static_cast<uint32_t>(currentMode->w)
            : static_cast<uint32_t>(logicalWidth),
        .outputHeight = mode != engine::RenderDisplayMode::Windowed &&
                currentMode && currentMode->h > 0
            ? static_cast<uint32_t>(currentMode->h)
            : static_cast<uint32_t>(logicalHeight),
        .refreshRateHz = currentMode &&
                std::isfinite(currentMode->refresh_rate) &&
                currentMode->refresh_rate > 0.0f
            ? static_cast<uint32_t>(
                  std::lround(currentMode->refresh_rate))
            : 0u,
        .displayMode = mode,
        .requestRevision = requestRevision,
        .applySucceeded = applySucceeded,
    };
    return true;
}

void RendererSubsystem::publishMainThreadWindowState(
    engine::RenderWindowOutputState state,
    bool forcePublication) {
    if (!isMainWindowOwner() || !state.validPixelExtent() ||
        !state.validLogicalExtent()) {
        return;
    }
    if (!forcePublication && m_mainWindowOutput.revision != 0u &&
        sameObservedWindowOutput(state, m_mainWindowOutput)) {
        return;
    }
    state.revision = m_nextWindowOutputRevision++;
    if (state.revision == 0u) state.revision = m_nextWindowOutputRevision++;
    if (state.applySucceeded) m_mainWindowOutput = state;
    static_cast<void>(m_windowOutputStates.publish(std::move(state)));
}

bool RendererSubsystem::applyMainThreadWindowRequest(
    const engine::ResolvedRenderDisplaySnapshot& display) {
    if (!isMainWindowOwner() || !m_dx12Renderer) return false;
    SDL_Window* const window = m_dx12Renderer->getSDLWindow();
    if (!window) return false;

    const engine::RenderDisplaySettings& requested = display.effective;
    const bool outputChanged =
        requested.displayMode != m_mainWindowOutput.displayMode ||
        (requested.displayMode == engine::RenderDisplayMode::Windowed &&
         (requested.width != m_mainWindowOutput.logicalWidth ||
          requested.height != m_mainWindowOutput.logicalHeight)) ||
        (requested.displayMode ==
             engine::RenderDisplayMode::ExclusiveFullscreen &&
         (requested.width != m_mainWindowOutput.outputWidth ||
          requested.height != m_mainWindowOutput.outputHeight ||
          (requested.refreshRateHz != 0u &&
           requested.refreshRateHz !=
               m_mainWindowOutput.refreshRateHz)));
    if (!outputChanged) return true;

    bool applied = applySdlWindowOutput(
        window, requested.displayMode, requested.width,
        requested.height, requested.refreshRateHz);
    if (applied) applied = SDL_SyncWindow(window);

    engine::RenderWindowOutputState observed;
    if (applied) {
        applied = observeMainThreadWindow(
            observed, display.revision, true) &&
            observed.displayMode == requested.displayMode;
    }
    if (applied) {
        publishMainThreadWindowState(std::move(observed), true);
        return true;
    }

    const container::String failure = SDL_GetError();
    // Restore the last accepted platform mode when an output request changed
    // only part of SDL's state before failing. The render mailbox is not
    // updated with unconfirmed pixels, so the last valid swapchain remains
    // authoritative even if platform rollback also fails.
    if (m_mainWindowOutput.validLogicalExtent()) {
        const bool restored = applySdlWindowOutput(
            window, m_mainWindowOutput.displayMode,
            m_mainWindowOutput.outputWidth,
            m_mainWindowOutput.outputHeight,
            m_mainWindowOutput.refreshRateHz);
        if (restored) static_cast<void>(SDL_SyncWindow(window));
    }
    engine::RenderWindowOutputState failed = m_mainWindowOutput;
    failed.requestRevision = display.revision;
    failed.applySucceeded = false;
    publishMainThreadWindowState(std::move(failed), true);
    TD_LOG_WARN(
        "[Renderer] Main-thread output apply failed for {}x{} mode={} revision={}: {}",
        requested.width, requested.height,
        static_cast<uint32_t>(requested.displayMode), display.revision,
        failure);
    return false;
}

void RendererSubsystem::serviceMainThreadWindow() {
    if (!isMainWindowOwner()) {
        rejectRenderControl();
        return;
    }
    engine::ResolvedRenderDisplaySnapshot request;
    if (!m_windowOutputRequests.tryTake(request) ||
        request.revision <= m_lastMainWindowRequestRevision) {
        return;
    }
    m_lastMainWindowRequestRevision = request.revision;
    static_cast<void>(applyMainThreadWindowRequest(request));
}

bool RendererSubsystem::setMainThreadMouseGrab(bool enabled) noexcept {
    if (!m_dx12Renderer) return !enabled;
    SDL_Window* const window = m_dx12Renderer->getSDLWindow();
    if (!window) return !enabled;
    return SDL_SetWindowMouseGrab(window, enabled);
}

void RendererSubsystem::renderWorldPass(engine::TextureManager& objectIconTextures) {
    if (m_renderOwnerState.load(std::memory_order_acquire) !=
            RenderOwnerState::Attached ||
        !platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        rejectRenderControl();
        return;
    }
    if (!m_dx12Renderer) return;
    if (m_submittedWorldPrepared) {
        const engine::DX12Renderer::PreparedWorldRenderResult result =
            m_dx12Renderer->renderPreparedWorldWithStatus(
                &objectIconTextures);
        m_submittedWorldPrepared = result.preparationPending();
        return;
    }
    if (m_dx12Renderer->hasPreparedWorld()) {
        m_dx12Renderer->renderPreparedWorld(&objectIconTextures);
        return;
    }
    if (!m_debugWorldEnabled) return;

    m_dx12Renderer->renderDebugWorld(m_debugWorldElapsedSeconds);
}

bool RendererSubsystem::startNextWorldPreparation() {
    if (!m_dx12Renderer || m_submittedWorldPrepared) {
        return m_submittedWorldPrepared;
    }
    // Reject stale tombstoned endpoints in one pass, then begin exactly one
    // asynchronous C preparation. This helper is safe both before beginFrame
    // and immediately after endFrame.
    while (!m_submittedWorldPrepared) {
        if (!m_deferredSnapshot) {
            engine::render::WorldRenderSnapshot nextSnapshot;
            if (!m_snapshotQueue.tryConsumeNewest(nextSnapshot)) break;
            m_deferredSnapshot = std::move(nextSnapshot);
        }
        m_submittedWorldPrepared = m_dx12Renderer->prepareWorldSnapshot(
            std::move(*m_deferredSnapshot));
        m_deferredSnapshot.reset();
    }
    return m_submittedWorldPrepared;
}

void RendererSubsystem::setTacticalRadarPanel(
    float left, float top, float width, float height, bool visible) noexcept {
    if (m_renderOwnerState.load(std::memory_order_acquire) !=
            RenderOwnerState::Attached ||
        !platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        rejectRenderControl();
        return;
    }
    if (m_dx12Renderer) {
        m_dx12Renderer->setTacticalRadarPanel(
            left, top, width, height, visible);
    }
}

void RendererSubsystem::prepareWorldPass() {
    if (m_renderOwnerState.load(std::memory_order_acquire) !=
            RenderOwnerState::Attached ||
        !platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        rejectRenderControl();
        return;
    }
    drainRenderControls();
    if (!m_dx12Renderer) return;
    m_dx12Renderer->pumpWorldCpuResourceCompletions();

    engine::RenderWindowOutputState newestOutput;
    if (m_windowOutputStates.tryTake(newestOutput)) {
        static_cast<void>(
            m_dx12Renderer->applyWindowOutputState(newestOutput));
    }

    engine::render::RenderViewState newestView;
    if (m_renderViewStates.tryTake(newestView)) {
        m_dx12Renderer->setRenderViewState(std::move(newestView));
    }

    engine::render::PresentationCameraOverride newestCameraOverride;
    if (m_presentationCameraOverrides.tryTake(newestCameraOverride)) {
        m_dx12Renderer->setPresentationCameraOverride(
            std::move(newestCameraOverride));
    }

    // Preparation and A->B display interpolation may span several present
    // frames. Do not consume another endpoint while the renderer already owns
    // an in-flight C; the SPSC FIFO retains every later confirmed tick.
    if (m_submittedWorldPrepared) {
        return;
    }

    static_cast<void>(startNextWorldPreparation());
    if (m_submittedWorldPrepared) return;
    if (!m_debugWorldEnabled) return;

    m_debugWorldElapsedSeconds = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - m_debugWorldStart).count();
    m_dx12Renderer->prepareDebugWorld(m_debugWorldElapsedSeconds);
}

bool RendererSubsystem::canAcceptWorldSnapshot() const noexcept {
    return m_snapshotQueue.hasCapacity();
}

bool RendererSubsystem::submitWorldSnapshot(
    engine::render::WorldRenderSnapshot& snapshot) {
    std::lock_guard ingressLock(m_worldIngressMutex);
    const uint64_t retiredEpoch = m_feedback.retiredPresentationEpoch();
    const uint64_t retiredSession = m_feedback.retiredSessionRevision();
    if (snapshot.presentationEpoch == 0u || snapshot.sessionRevision == 0u ||
        retiredWorldDomain(
            snapshot.presentationEpoch, snapshot.sessionRevision,
            retiredEpoch, retiredSession)) {
        return true;
    }
    return m_snapshotQueue.tryPublish(snapshot);
}

void RendererSubsystem::submitRenderViewState(
    engine::render::RenderViewState view) {
    std::lock_guard ingressLock(m_worldIngressMutex);
    const uint64_t retiredEpoch = m_feedback.retiredPresentationEpoch();
    const uint64_t retiredSession = m_feedback.retiredSessionRevision();
    if (view.sourceWorld.presentationEpoch == 0u ||
        view.sourceWorld.sessionRevision == 0u ||
        retiredWorldDomain(
            view.sourceWorld.presentationEpoch,
            view.sourceWorld.sessionRevision,
            retiredEpoch, retiredSession)) {
        return;
    }
    static_cast<void>(m_renderViewStates.publish(std::move(view)));
}

void RendererSubsystem::submitPresentationCamera(
    const engine::GameCameraState& camera,
    uint64_t expectedSessionRevision) {
    std::optional<engine::render::RenderViewState> view =
        lastPresentedRenderView();
    if (!view || view->sourceWorld.sessionRevision != expectedSessionRevision) {
        return;
    }
    const engine::GameCameraState state = camera.sanitized();
    engine::render::PresentationCameraOverride cameraOverride{
        .sourceWorld = view->sourceWorld,
        .camera = view->camera,
        .active = true,
    };
    cameraOverride.camera.position = state.position;
    cameraOverride.camera.visibilityDistance = state.visibilityDistance;
    cameraOverride.camera.target = state.target;
    cameraOverride.camera.up = state.up;
    cameraOverride.camera.verticalFovRadians = state.verticalFovRadians;
    cameraOverride.camera.horizontalFovRadians = state.horizontalFovRadians;
    cameraOverride.camera.tacticalViewportHeightScale =
        state.tacticalViewportHeightScale;
    cameraOverride.camera.nearClip = state.nearClip;
    cameraOverride.camera.farClip = state.farClip;
    cameraOverride.camera.fogEnabled = state.fogEnabled;
    cameraOverride.camera.fogColor = state.fogColor;
    cameraOverride.camera.fogStartDistance = state.fogStartDistance;
    cameraOverride.camera.fogEndDistance = state.fogEndDistance;
    cameraOverride.camera.cameraCutRevision = state.cameraCutRevision;

    std::lock_guard ingressLock(m_worldIngressMutex);
    const uint64_t retiredEpoch = m_feedback.retiredPresentationEpoch();
    const uint64_t retiredSession = m_feedback.retiredSessionRevision();
    if (retiredWorldDomain(
            cameraOverride.sourceWorld.presentationEpoch,
            cameraOverride.sourceWorld.sessionRevision,
            retiredEpoch, retiredSession)) {
        return;
    }
    static_cast<void>(m_presentationCameraOverrides.publish(
        std::move(cameraOverride)));
}

void RendererSubsystem::releasePresentationCamera(
    const engine::GameCameraState& settledCamera,
    uint64_t expectedSessionRevision) {
    std::optional<engine::render::RenderViewState> view =
        lastPresentedRenderView();
    if (!view || view->sourceWorld.sessionRevision != expectedSessionRevision) {
        return;
    }
    const engine::GameCameraState state = settledCamera.sanitized();
    engine::render::PresentationCameraOverride cameraOverride{
        .sourceWorld = view->sourceWorld,
        .camera = view->camera,
        .releaseWhenBaseMatches = true,
        .active = true,
    };
    cameraOverride.camera.position = state.position;
    cameraOverride.camera.visibilityDistance = state.visibilityDistance;
    cameraOverride.camera.target = state.target;
    cameraOverride.camera.up = state.up;
    cameraOverride.camera.verticalFovRadians = state.verticalFovRadians;
    cameraOverride.camera.horizontalFovRadians = state.horizontalFovRadians;
    cameraOverride.camera.tacticalViewportHeightScale =
        state.tacticalViewportHeightScale;
    cameraOverride.camera.nearClip = state.nearClip;
    cameraOverride.camera.farClip = state.farClip;
    cameraOverride.camera.fogEnabled = state.fogEnabled;
    cameraOverride.camera.fogColor = state.fogColor;
    cameraOverride.camera.fogStartDistance = state.fogStartDistance;
    cameraOverride.camera.fogEndDistance = state.fogEndDistance;
    cameraOverride.camera.cameraCutRevision = state.cameraCutRevision;

    std::lock_guard ingressLock(m_worldIngressMutex);
    const uint64_t retiredEpoch = m_feedback.retiredPresentationEpoch();
    const uint64_t retiredSession = m_feedback.retiredSessionRevision();
    if (retiredWorldDomain(
            cameraOverride.sourceWorld.presentationEpoch,
            cameraOverride.sourceWorld.sessionRevision,
            retiredEpoch, retiredSession)) {
        return;
    }
    static_cast<void>(m_presentationCameraOverrides.publish(
        std::move(cameraOverride)));
}

void RendererSubsystem::clearPresentationCamera(
    uint64_t expectedSessionRevision) {
    if (expectedSessionRevision == 0u) return;
    std::lock_guard ingressLock(m_worldIngressMutex);
    static_cast<void>(m_presentationCameraOverrides.publish({
        .sourceWorld = {
            .sessionRevision = expectedSessionRevision,
        },
        .active = false,
    }));
}

void RendererSubsystem::captureScreenshot(container::String filename) {
    if (filename.empty()) return;
    enqueueRenderControl(
        [this, filename = std::move(filename)]() mutable {
            if (m_dx12Renderer) {
                static_cast<void>(m_dx12Renderer->captureScreenshot(filename));
            }
        });
}

void RendererSubsystem::retireWorldPresentation(
    engine::render::WorldPreparationStamp retiredWorld) {
    if (retiredWorld.presentationEpoch == 0u ||
        retiredWorld.sessionRevision == 0u) {
        return;
    }

    std::lock_guard ingressLock(m_worldIngressMutex);
    const auto retirementFloor = m_feedback.retire(retiredWorld);
    const uint64_t retiredEpoch = retirementFloor.presentationEpoch;
    const uint64_t retiredSession = retirementFloor.sessionRevision;

    enqueueRenderControl([this, retiredWorld, retiredEpoch, retiredSession] {
        // Drain on the consumer thread; reset() is intentionally reserved for
        // a fully stopped producer/consumer pair. A newer session may already
        // have published while this ordered control was in flight; preserve
        // that value and discard only domains covered by the tombstone.
        engine::render::WorldRenderSnapshot queuedSnapshot;
        while (m_snapshotQueue.tryConsumeOldest(queuedSnapshot)) {
            if (retiredWorldDomain(
                    queuedSnapshot.presentationEpoch,
                    queuedSnapshot.sessionRevision,
                    retiredEpoch, retiredSession)) {
                continue;
            }
            // FIFO publication guarantees every later value belongs to this
            // domain or a newer one. Preserve the first legal endpoint and
            // leave the remaining ordered suffix in the queue.
            m_deferredSnapshot = std::move(queuedSnapshot);
            break;
        }
        if (m_deferredSnapshot && retiredWorldDomain(
                m_deferredSnapshot->presentationEpoch,
                m_deferredSnapshot->sessionRevision,
                retiredEpoch, retiredSession)) {
            m_deferredSnapshot.reset();
        }
        engine::render::RenderViewState queuedView;
        const bool hasQueuedView = m_renderViewStates.tryTake(queuedView);
        const bool retiresAcceptedWorld = !m_dx12Renderer ||
            m_dx12Renderer->worldPresentationEpoch() <=
                retiredWorld.presentationEpoch;
        if (retiresAcceptedWorld) {
            m_submittedWorldPrepared = false;
        }
        if (m_dx12Renderer) {
            m_dx12Renderer->retireWorldPresentation(retiredWorld);
            if (hasQueuedView && !retiredWorldDomain(
                    queuedView.sourceWorld.presentationEpoch,
                    queuedView.sourceWorld.sessionRevision,
                    retiredEpoch, retiredSession)) {
                m_dx12Renderer->setRenderViewState(std::move(queuedView));
            }
        }
    });
}

void RendererSubsystem::applyRenderQualitySettings(
    const engine::ResolvedRenderFeatureSnapshot& feature,
    const engine::ResolvedRenderDisplaySnapshot& display) {
    uint64_t queued = m_lastQueuedWindowRequestRevision.load(
        std::memory_order_acquire);
    while (display.revision > queued) {
        if (m_lastQueuedWindowRequestRevision.compare_exchange_weak(
                queued, display.revision, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            static_cast<void>(m_windowOutputRequests.publish(display));
            break;
        }
    }
    enqueueRenderControl([this, feature, display] {
        if (!m_dx12Renderer) return;
        m_dx12Renderer->applyRenderFeatureQuality(feature);
        m_dx12Renderer->applyRenderDisplaySettings(display);
    });
}

engine::RenderDisplayCapabilities
RendererSubsystem::renderDisplayCapabilities() const noexcept {
    engine::RenderDisplayCapabilities result{
        .maximumWidth = engine::render_game_data_limits::kMaximumRenderDimension,
        .maximumHeight = engine::render_game_data_limits::kMaximumRenderDimension,
        .maximumAnisotropy = 16u,
        .supportsFxaa = m_dx12Renderer && m_dx12Renderer->fxaaAvailable(),
    };
    if (!isMainWindowOwner()) {
        result.supportsBorderlessFullscreen = false;
        result.supportsExclusiveFullscreen = false;
        return result;
    }
    if (!m_dx12Renderer || !m_dx12Renderer->getSDLWindow()) {
        result.supportsBorderlessFullscreen = false;
        result.supportsExclusiveFullscreen = false;
        return result;
    }

    const SDL_DisplayID displayId = SDL_GetDisplayForWindow(
        m_dx12Renderer->getSDLWindow());
    if (displayId == 0u) {
        result.supportsBorderlessFullscreen = false;
        result.supportsExclusiveFullscreen = false;
        return result;
    }
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(displayId);
    result.supportsBorderlessFullscreen = desktop != nullptr;
    if (desktop) {
        result.desktopWidth = desktop->w > 0
            ? static_cast<uint32_t>(desktop->w) : 0u;
        result.desktopHeight = desktop->h > 0
            ? static_cast<uint32_t>(desktop->h) : 0u;
        if (std::isfinite(desktop->refresh_rate) &&
            desktop->refresh_rate > 0.0f) {
            result.desktopRefreshRateHz = static_cast<uint32_t>(
                std::lround(desktop->refresh_rate));
        }
    }

    int modeCount = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(
        displayId, &modeCount);
    result.supportsExclusiveFullscreen = modes && modeCount > 0;
    uint32_t maximumModeWidth = result.desktopWidth;
    uint32_t maximumModeHeight = result.desktopHeight;
    for (int i = 0; modes && i < modeCount; ++i) {
        if (!modes[i]) continue;
        if (modes[i]->w > 0) {
            maximumModeWidth = std::max(
                maximumModeWidth,
                static_cast<uint32_t>(modes[i]->w));
        }
        if (modes[i]->h > 0) {
            maximumModeHeight = std::max(
                maximumModeHeight,
                static_cast<uint32_t>(modes[i]->h));
        }
    }
    if (modes) SDL_free(modes);
    if (maximumModeWidth != 0u) {
        result.maximumWidth = std::min(
            maximumModeWidth,
            engine::render_game_data_limits::kMaximumRenderDimension);
    }
    if (maximumModeHeight != 0u) {
        result.maximumHeight = std::min(
            maximumModeHeight,
            engine::render_game_data_limits::kMaximumRenderDimension);
    }
    return result;
}

void RendererSubsystem::submitFxSnapshot(
    const engine::fx::FxPresentationSnapshot& snapshot,
    container::SharedPtr<const engine::fx::ParticleSystemCatalog> particles,
    container::SharedPtr<const engine::fx::FxListCatalog> fxLists,
    const engine::RenderGameDataSettings& settings) {
    std::lock_guard ingressLock(m_worldIngressMutex);
    const uint64_t retiredEpoch = m_feedback.retiredPresentationEpoch();
    if (snapshot.sessionEpoch == 0u ||
        (retiredEpoch != 0u && snapshot.sessionEpoch <= retiredEpoch)) {
        return;
    }
    enqueueRenderControl(
        [this, snapshot, particles = std::move(particles),
         fxLists = std::move(fxLists), settings]() mutable {
            if (!m_dx12Renderer) return;
            const uint64_t retired = m_feedback.retiredPresentationEpoch();
            if (retired != 0u && snapshot.sessionEpoch <= retired) return;
            m_dx12Renderer->configureFxContent(
                std::move(particles), std::move(fxLists), settings);
            m_dx12Renderer->submitFxSnapshot(snapshot);
        });
}

container::Vector<engine::fx::FxSoundCommand>
RendererSubsystem::takeFxSoundCommands() {
    const RenderOwnerState ownerState =
        m_renderOwnerState.load(std::memory_order_acquire);
    if (ownerState == RenderOwnerState::Stopped) {
        return {};
    }
    if (ownerState == RenderOwnerState::PreAttach) {
        if (std::this_thread::get_id() != m_preAttachOwnerThread) return {};
        return m_dx12Renderer
            ? m_dx12Renderer->takeFxSoundCommands()
            : container::Vector<engine::fx::FxSoundCommand>{};
    }
    return m_feedback.takeFxSounds();
}

container::Vector<engine::render::RenderAnimationCompletionFeedback>
RendererSubsystem::takeAnimationCompletions() {
    const RenderOwnerState ownerState =
        m_renderOwnerState.load(std::memory_order_acquire);
    if (ownerState == RenderOwnerState::Stopped) {
        return {};
    }
    if (ownerState == RenderOwnerState::PreAttach) {
        if (std::this_thread::get_id() != m_preAttachOwnerThread) return {};
        return m_dx12Renderer
            ? m_dx12Renderer->takeAnimationCompletions()
            : container::Vector<
                  engine::render::RenderAnimationCompletionFeedback>{};
    }
    return m_feedback.takeAnimationCompletions();
}

void RendererSubsystem::clearFxPresentation() {
    enqueueRenderControl([this] {
        if (m_dx12Renderer) m_dx12Renderer->clearFxPresentation();
    });
}

void RendererSubsystem::submitGroundDecalPresentation(
    const engine::render::GroundDecalPresentationBatch& batch) {
    std::lock_guard ingressLock(m_worldIngressMutex);
    const uint64_t retiredEpoch = m_feedback.retiredPresentationEpoch();
    if (batch.presentationEpoch == 0u ||
        (retiredEpoch != 0u &&
         batch.presentationEpoch <= retiredEpoch)) {
        return;
    }
    enqueueRenderControl([this, batch] {
        const uint64_t retired = m_feedback.retiredPresentationEpoch();
        if (retired != 0u && batch.presentationEpoch <= retired) return;
        if (m_dx12Renderer) {
            m_dx12Renderer->submitGroundDecalPresentation(batch);
        }
    });
}

void RendererSubsystem::clearGroundDecalPresentation(
    uint64_t presentationEpoch) {
    enqueueRenderControl([this, presentationEpoch] {
        if (m_dx12Renderer) {
            m_dx12Renderer->clearGroundDecalPresentation(presentationEpoch);
        }
    });
}

std::optional<engine::render::RenderCameraSnapshot>
RendererSubsystem::scriptCameraSlaveListenerOverride(
    uint64_t expectedPresentationEpoch) noexcept {
    const uint64_t retiredEpoch = m_feedback.retiredPresentationEpoch();
    if (expectedPresentationEpoch == 0u ||
        (retiredEpoch != 0u &&
         expectedPresentationEpoch <= retiredEpoch)) {
        return std::nullopt;
    }
    const RenderOwnerState ownerState =
        m_renderOwnerState.load(std::memory_order_acquire);
    if (ownerState == RenderOwnerState::Stopped) return std::nullopt;
    if (ownerState == RenderOwnerState::PreAttach) {
        if (std::this_thread::get_id() != m_preAttachOwnerThread) {
            return std::nullopt;
        }
        return m_dx12Renderer
            ? m_dx12Renderer->scriptCameraSlaveListenerOverride(
                  expectedPresentationEpoch)
            : std::nullopt;
    }
    const auto camera = m_feedback.camera(expectedPresentationEpoch);
    if (camera.sampled) return camera.camera;
    if (m_feedback.claimCameraQuery(expectedPresentationEpoch)) {
        try {
            const bool queued = enqueueRenderControl(
                [this, expectedPresentationEpoch] {
                    const uint64_t retired =
                        m_feedback.retiredPresentationEpoch();
                    if (retired != 0u &&
                        expectedPresentationEpoch <= retired) {
                        return;
                    }
                    const auto camera = m_dx12Renderer
                        ? m_dx12Renderer->scriptCameraSlaveListenerOverride(
                              expectedPresentationEpoch)
                        : std::nullopt;
                    m_feedback.publishCamera(
                        expectedPresentationEpoch, camera);
                });
            if (!queued) {
                m_feedback.abandonCameraQuery(expectedPresentationEpoch);
            }
        } catch (...) {
            m_feedback.abandonCameraQuery(expectedPresentationEpoch);
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<engine::render::WorldFrameRenderStats>
RendererSubsystem::lastWorldFrameStats() const noexcept {
    const RenderOwnerState ownerState =
        m_renderOwnerState.load(std::memory_order_acquire);
    if (ownerState == RenderOwnerState::Stopped) return std::nullopt;
    if (ownerState == RenderOwnerState::PreAttach) {
        if (std::this_thread::get_id() != m_preAttachOwnerThread) {
            return std::nullopt;
        }
        return m_dx12Renderer ? m_dx12Renderer->lastWorldFrameStats()
                              : std::nullopt;
    }
    return m_feedback.lastWorldFrameStats();
}

std::optional<engine::render::RenderViewState>
RendererSubsystem::lastPresentedRenderView() const noexcept {
    const RenderOwnerState ownerState =
        m_renderOwnerState.load(std::memory_order_acquire);
    if (ownerState == RenderOwnerState::Stopped) return std::nullopt;
    if (ownerState == RenderOwnerState::PreAttach) {
        if (std::this_thread::get_id() != m_preAttachOwnerThread ||
            !m_dx12Renderer) {
            return std::nullopt;
        }
        return m_dx12Renderer->currentRenderViewState();
    }
    return m_feedback.lastPresentedRenderView();
}

void RendererSubsystem::publishRenderFeedback() {
    if (m_renderOwnerState.load(std::memory_order_acquire) !=
            RenderOwnerState::Attached ||
        !platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        rejectRenderControl();
        return;
    }
    if (!m_dx12Renderer) return;
    const uint64_t feedbackEpoch =
        m_dx12Renderer->worldPresentationEpoch();
    auto sounds = m_dx12Renderer->takeFxSoundCommands();
    auto animations = m_dx12Renderer->takeAnimationCompletions();
    auto stats = m_dx12Renderer->lastWorldFrameStats();
    auto renderView = m_dx12Renderer->currentRenderViewState();
    m_feedback.publish(
        feedbackEpoch, std::move(sounds), std::move(animations),
        std::move(stats), std::move(renderView));
    // The endpoint may have been published by renderWorldPass. Start C now,
    // after endFrame/present, so CPU preparation overlaps the complete A->B
    // display interval instead of waiting for the next render-loop prologue.
    static_cast<void>(startNextWorldPreparation());
}

void RendererSubsystem::toggleDebugWorld() {
    const bool enabled = !m_debugWorldEnabled.load(std::memory_order_acquire);
    m_debugWorldEnabled.store(enabled, std::memory_order_release);
    enqueueRenderControl([this] {
        m_debugWorldStart = std::chrono::steady_clock::now();
        m_debugWorldElapsedSeconds = 0.0f;
    });
    TD_LOG_INFO("[Renderer] Debug world {}", enabled ? "enabled" : "disabled");
}

bool RendererSubsystem::applyDebugWorldCameraInput(
    const engine::GameCameraInput& input, float deltaSeconds) noexcept {
    if (!m_debugWorldEnabled.load(std::memory_order_acquire) ||
        !input.hasManualInput()) {
        return false;
    }
    try {
        enqueueRenderControl([this, input, deltaSeconds] {
            if (m_dx12Renderer) {
                static_cast<void>(m_dx12Renderer->applyDebugWorldCameraInput(
                    input, deltaSeconds));
            }
        });
        return true;
    } catch (...) {
        return false;
    }
}

bool RendererSubsystem::zoomDebugWorld(float wheelUnits) noexcept {
    if (!m_debugWorldEnabled.load(std::memory_order_acquire)) return false;
    try {
        enqueueRenderControl([this, wheelUnits] {
            if (m_dx12Renderer) {
                static_cast<void>(
                    m_dx12Renderer->zoomDebugWorldCamera(wheelUnits));
            }
        });
        return true;
    } catch (...) {
        return false;
    }
}

void RendererSubsystem::setWorldSkeletonMode(bool enabled) {
    m_worldSkeletonMode = enabled;
    enqueueRenderControl([this, enabled] {
        if (m_dx12Renderer) m_dx12Renderer->setWorldSkeletonMode(enabled);
    });
    TD_LOG_INFO("[Renderer] World render mode: {}", enabled ? "SKELETON" : "TEXTURED");
}

void RendererSubsystem::toggleWorldTextureOnlyMode() {
    setWorldTextureOnlyMode(!m_worldTextureOnlyMode);
}

void RendererSubsystem::setWorldTextureOnlyMode(bool enabled) {
    m_worldTextureOnlyMode = enabled;
    enqueueRenderControl([this, enabled] {
        if (m_dx12Renderer) m_dx12Renderer->setWorldTextureOnlyMode(enabled);
    });
    TD_LOG_INFO("[Renderer] World material diagnostic: {}",
                m_worldTextureOnlyMode ? "TEXTURE_ONLY" : "NORMAL");
}
