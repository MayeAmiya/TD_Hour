#pragma once

#include "core/container/container_types.h"

#include "system/SubsystemInterface.h"
#include "engine/renderer/runtime/RendererStats.h"
#include "engine/system/RendererFeedbackChannel.h"
#include "presentation/render/RenderSceneSnapshot.h"
#include "engine/renderer/world/terrain/GroundDecalPresentation.h"
#include "engine/renderer/world/pipeline/WorldRenderFrameQueue.h"
#include "engine/fx/runtime/FxPresentationCommands.h"
#include "core/platform/runtime_mailbox.h"
#include "presentation/render/RenderGameDataSettings.h"
#include "presentation/camera/GameCameraState.h"
#include "engine/renderer/runtime/RendererInputViewport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace engine {
class DX12Renderer;
struct GameCameraInput;
struct RenderGameDataSettings;
struct ResolvedRenderFeatureSnapshot;
struct ResolvedRenderDisplaySnapshot;
struct RenderDisplayCapabilities;
struct RenderWindowOutputState;
class TextureManager;
namespace fx {
class ParticleSystemCatalog;
class FxListCatalog;
struct FxPresentationSnapshot;
}
}

// RendererSubsystem: initializes SDL3 + DX12 renderer and sets virtual resolution.
class RendererSubsystem : public SubsystemInterface {
public:
    RendererSubsystem();
    ~RendererSubsystem() override;

    void init() override;
    void reset() override;
    void shutdown() override;

    // Called by the dedicated render thread before/after its frame loop. Once
    // attached, all D3D-mutating public commands are serialized through the
    // control mailbox and executed only by that thread.
    void attachRenderThread();
    void detachRenderThread() noexcept;
    void releaseRenderResourcesOnRenderThread();
    void publishRenderFeedback();
    // Main/event-thread pump for output requests submitted by logic or
    // presentation. All SDL window/fullscreen/display calls live behind this
    // boundary; the render owner receives only immutable observed pixels.
    void serviceMainThreadWindow();
    [[nodiscard]] bool setMainThreadMouseGrab(bool enabled) noexcept;
    void requestResize(uint32_t width, uint32_t height);
    [[nodiscard]] engine::RendererInputViewport inputViewport() const noexcept;

    // The world pass is deliberately separate from Renderer::draw* so the
    // legacy UI facade remains 2D-only. Preparation starts before beginFrame;
    // submission finishes between beginFrame/endFrame and before UI.
    void prepareWorldPass();
    void renderWorldPass(engine::TextureManager& objectIconTextures);
    void setTacticalRadarPanel(float left, float top, float width,
                               float height, bool visible) noexcept;
    // Called by the application/game extraction after a completed logic tick.
    // The payload is copied/moved into renderer-owned memory before async
    // preparation begins, preserving a strict snapshot boundary.
    // Main/presentation retains only its newest source when this short state
    // history is full. World snapshots are replaceable; deterministic logic
    // and ordered event journals are never back-pressured by rendering.
    [[nodiscard]] bool canAcceptWorldSnapshot() const noexcept;
    [[nodiscard]] size_t queuedWorldSnapshotCount() const noexcept {
        return m_snapshotQueue.size();
    }
    [[nodiscard]] bool submitWorldSnapshot(
        engine::render::WorldRenderSnapshot& snapshot);
    // View state is independent from world preparation. Main/presentation may
    // overwrite an unconsumed camera sample without queuing stale camera
    // motion; the render thread consumes the newest complete value each frame.
    void submitRenderViewState(engine::render::RenderViewState view);
    // Local camera presentation is allowed to advance between confirmed
    // logic ticks. It updates only the renderer-owned view endpoint; the
    // caller separately mirrors the resulting pose to logic for the next
    // confirmed extraction.
    void submitPresentationCamera(
        const engine::GameCameraState& camera,
        uint64_t expectedSessionRevision);
    void releasePresentationCamera(
        const engine::GameCameraState& settledCamera,
        uint64_t expectedSessionRevision);
    void clearPresentationCamera(uint64_t expectedSessionRevision);
    void captureScreenshot(container::String filename);
    // Called once when the active GameSession disappears. The retired domain
    // becomes a producer- and consumer-side tombstone before queued,
    // deferred and prepared frames are released. A late value from that
    // domain can therefore never revive the old world.
    void retireWorldPresentation(
        engine::render::WorldPreparationStamp retiredWorld);
    void applyRenderQualitySettings(
        const engine::ResolvedRenderFeatureSnapshot& feature,
        const engine::ResolvedRenderDisplaySnapshot& display);
    [[nodiscard]] engine::RenderDisplayCapabilities
    renderDisplayCapabilities() const noexcept;
    // Ordered one-shot FX bypass the newest-only world queue. Immutable
    // catalogs are supplied by the application composition root and retained
    // by the renderer-owned presentation runtime.
    void submitFxSnapshot(
        const engine::fx::FxPresentationSnapshot& snapshot,
        container::SharedPtr<const engine::fx::ParticleSystemCatalog> particles,
        container::SharedPtr<const engine::fx::FxListCatalog> fxLists,
        const engine::RenderGameDataSettings& settings);
    [[nodiscard]] container::Vector<engine::fx::FxSoundCommand>
    takeFxSoundCommands();
    [[nodiscard]] container::Vector<
        engine::render::RenderAnimationCompletionFeedback>
    takeAnimationCompletions();
    void clearFxPresentation();
    void submitGroundDecalPresentation(
        const engine::render::GroundDecalPresentationBatch& batch);
    void clearGroundDecalPresentation(uint64_t presentationEpoch = 0);
    // Value-only renderer presentation camera for the audio hand-off. It is
    // populated solely by CAMERA_*_SLAVE_MODE after W3D bone evaluation; the
    // caller supplies the current GameSession epoch so an old rendered match
    // can never affect a new session's listener.
    [[nodiscard]] std::optional<engine::render::RenderCameraSnapshot>
    scriptCameraSlaveListenerOverride(uint64_t expectedPresentationEpoch) noexcept;
    [[nodiscard]] std::optional<engine::render::WorldFrameRenderStats>
    lastWorldFrameStats() const noexcept;
    // Last view actually selected by the render thread, including its A/B
    // endpoints, interpolation alpha and presentation camera. Input/picking
    // must use this sample instead of the newest logic snapshot.
    [[nodiscard]] std::optional<engine::render::RenderViewState>
    lastPresentedRenderView() const noexcept;
    void toggleDebugWorld();
    [[nodiscard]] bool debugWorldEnabled() const noexcept {
        return m_debugWorldEnabled.load(std::memory_order_acquire);
    }
    // Renderer-only camera path used when no GameSession owns the world
    // camera. Input stays value-only across the subsystem boundary.
    [[nodiscard]] bool applyDebugWorldCameraInput(
        const engine::GameCameraInput& input, float deltaSeconds) noexcept;
    [[nodiscard]] bool zoomDebugWorld(float wheelUnits) noexcept;
    // Keeps the debug UI and the renderer's geometry-only view in lockstep.
    // Unlike a game option, this is never published into a world snapshot.
    void setWorldSkeletonMode(bool enabled);
    [[nodiscard]] bool worldSkeletonMode() const noexcept {
        return m_worldSkeletonMode;
    }
    // F3 material-isolation diagnostic. It samples only the base texture on
    // the normal world PSO and remains independent of the UI renderer.
    void toggleWorldTextureOnlyMode();
    void setWorldTextureOnlyMode(bool enabled);
    [[nodiscard]] bool worldTextureOnlyMode() const noexcept {
        return m_worldTextureOnlyMode;
    }

private:
    enum class RenderOwnerState : uint8_t {
        PreAttach,
        Attached,
        Stopped,
    };

    bool initializeRenderBackend();
    [[nodiscard]] bool isMainWindowOwner() const noexcept;
    [[nodiscard]] bool observeMainThreadWindow(
        engine::RenderWindowOutputState& output,
        uint64_t requestRevision = 0,
        bool applySucceeded = true) const noexcept;
    [[nodiscard]] bool applyMainThreadWindowRequest(
        const engine::ResolvedRenderDisplaySnapshot& display);
    void publishMainThreadWindowState(
        engine::RenderWindowOutputState state,
        bool forcePublication = false);
    bool enqueueRenderControl(std::function<void()> command);
    void drainRenderControls();
    bool startNextWorldPreparation();
    void rejectRenderControl() noexcept;

    engine::DX12Renderer* m_dx12Renderer = nullptr; // owned by Renderer::instance()
    std::atomic<bool> m_debugWorldEnabled{false};
    bool m_worldSkeletonMode = false;
    bool m_worldTextureOnlyMode = false;
    std::chrono::steady_clock::time_point m_debugWorldStart{};
    float m_debugWorldElapsedSeconds = 0.0f;
    // Display A/B plus newest candidate C are renderer-owned. Older unread
    // world states are replaceable and must not become presentation latency.
    engine::render::WorldRenderFrameQueue<3> m_snapshotQueue;
    platform::runtime::LatestValueMailbox<engine::render::RenderViewState>
        m_renderViewStates;
    platform::runtime::LatestValueMailbox<
        engine::render::PresentationCameraOverride>
        m_presentationCameraOverrides;
    platform::runtime::LatestValueMailbox<
        engine::ResolvedRenderDisplaySnapshot> m_windowOutputRequests;
    platform::runtime::LatestValueMailbox<engine::RenderWindowOutputState>
        m_windowOutputStates;
    engine::RenderWindowOutputState m_mainWindowOutput;
    void* m_nativeWindowHandle = nullptr;
    uint64_t m_nextWindowOutputRevision = 1;
    uint64_t m_lastMainWindowRequestRevision = 0;
    std::atomic<uint64_t> m_lastQueuedWindowRequestRevision{0};
    // Serializes producer admission against retirement. Extraction and
    // presentation admission run on different threads, so a check followed
    // by publication must be indivisible with respect to tombstone advance.
    mutable std::mutex m_worldIngressMutex;
    engine::RendererFeedbackChannel m_feedback;
    std::optional<engine::render::WorldRenderSnapshot> m_deferredSnapshot;
    bool m_submittedWorldPrepared = false;
    std::atomic<RenderOwnerState> m_renderOwnerState{
        RenderOwnerState::PreAttach};
    std::thread::id m_preAttachOwnerThread{};
    platform::runtime::BoundedMailbox<std::function<void()>, 2048>
        m_renderControls;
    // Producers never wait for the render thread. Saturation is a terminal
    // contract failure rather than permission to lose an ordered command.
    std::atomic<bool> m_renderControlOverflowed{false};
    std::atomic<bool> m_renderControlRejected{false};
};
