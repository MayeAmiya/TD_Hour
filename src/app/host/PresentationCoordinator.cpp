#include "PresentationCoordinator.h"
#include "runtime/GameLogicIntent.h"
#include "runtime/GameUiProjection.h"
#include "core/platform/runtime_mailbox.h"

#include "CommandLine.h"
#include "DX12Renderer.h"
#include "ui/ingame/InGameGuiSubsystem.h"
#include "TextureManager.h"
#include "debug/debug.h"
#include "engine/renderer/world/overlay/ObjectUiOverlayPresentation.h"
#include "engine/renderer/world/radar/TacticalRadarPresentation.h"
#include "system/AudioSubsystem.h"
#include "system/RendererSubsystem.h"
#include "app/runtime/GameLogic.h"
#include "game/fx/runtime/GameFxEvents.h"
#include "game/ini/GameDataLoader.h"
#include "game/session/core/GameSession.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

namespace app {
namespace {

struct ExtractedWorldFrame final {
    uint64_t sessionRevision = 0;
    bool sessionActive = false;
    std::optional<engine::render::WorldRenderSnapshot> snapshot;
};

struct SelectionHitProxy final {
    engine::ObjectId object = engine::INVALID_OBJECT_ID;
    engine::render::RenderVector worldPosition{};
    float worldRadius = 1.0f;
    engine::render::ObjectUiSelectionBoundsKind boundsKind =
        engine::render::ObjectUiSelectionBoundsKind::Sphere;
    float majorRadius = 1.0f;
    float minorRadius = 1.0f;
    float height = 2.0f;
    float yawRadians = 0.0f;
    bool selectable = false;
    bool shrubberyTarget = false;
    bool mineTarget = false;
    bool forceAttackable = false;
    bool owned = false;
    bool structure = false;
    engine::render::LocalVisibilityRenderCellState visibility =
        engine::render::LocalVisibilityRenderCellState::Shrouded;
};

struct OrderWaypointHitProxy final {
    engine::render::RenderEntityId identity = 0;
    engine::ObjectId actor = engine::INVALID_OBJECT_ID;
    uint32_t sourceSequence = 0;
    engine::selection::LocalOrderWaypointKind kind =
        engine::selection::LocalOrderWaypointKind::Move;
    engine::render::RenderVector worldPosition{};
    float radius = 4.0f;
};

struct SelectionRay final {
    engine::render::RenderVector origin{};
    engine::render::RenderVector direction{0.0f, 0.0f, -1.0f};
};

[[nodiscard]] engine::render::RenderVector normalized(
    engine::render::RenderVector value) noexcept {
    const float lengthSquared = value.x() * value.x() +
        value.y() * value.y() + value.z() * value.z();
    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= math::EPSILON * math::EPSILON) {
        return {};
    }
    return value * (1.0f / std::sqrt(lengthSquared));
}

[[nodiscard]] engine::render::RenderVector cross(
    const engine::render::RenderVector& left,
    const engine::render::RenderVector& right) noexcept {
    return {
        left.y() * right.z() - left.z() * right.y(),
        left.z() * right.x() - left.x() * right.z(),
        left.x() * right.y() - left.y() * right.x(),
    };
}

[[nodiscard]] std::optional<SelectionRay> selectionRay(
    const engine::render::RenderCameraSnapshot& camera,
    const engine::render::RenderViewportMetrics& viewport,
    math::vec2 pointer) noexcept {
    if (!viewport.valid() || !std::isfinite(pointer.x()) ||
        !std::isfinite(pointer.y())) {
        return std::nullopt;
    }
    const float width = viewport.virtualWidth;
    const float height = viewport.virtualHeight;
    const float tacticalHeight = height * std::clamp(
        camera.tacticalViewportHeightScale, 0.1f, 1.0f);
    const engine::render::RenderVector forward = normalized(
        camera.target - camera.position);
    const engine::render::RenderVector right = normalized(
        cross(forward, camera.up));
    const engine::render::RenderVector up = normalized(cross(right, forward));
    if (forward == engine::render::RenderVector{} ||
        right == engine::render::RenderVector{} ||
        up == engine::render::RenderVector{}) {
        return std::nullopt;
    }
    const float ndcX = pointer.x() * 2.0f / width - 1.0f;
    const float ndcY = 1.0f - pointer.y() * 2.0f / tacticalHeight;
    const float aspect = engine::render::renderCameraEffectiveAspectRatio(
        camera, viewport.fullAspectRatio());
    const float tangent = std::tan(
        engine::render::renderCameraVerticalFovRadians(
            camera, viewport.fullAspectRatio()) * 0.5f);
    const engine::render::RenderVector direction = normalized(
        forward + right * (ndcX * tangent * aspect) +
        up * (ndcY * tangent));
    if (direction == engine::render::RenderVector{}) return std::nullopt;
    return SelectionRay{.origin = camera.position, .direction = direction};
}

[[nodiscard]] bool slab(float origin, float direction, float minimum,
                        float maximum, float& nearT, float& farT) noexcept {
    if (std::abs(direction) <= math::EPSILON) {
        return origin >= minimum && origin <= maximum;
    }
    float first = (minimum - origin) / direction;
    float second = (maximum - origin) / direction;
    if (first > second) std::swap(first, second);
    nearT = std::max(nearT, first);
    farT = std::min(farT, second);
    return nearT <= farT;
}

[[nodiscard]] std::optional<float> rayHitDistance(
    const SelectionRay& ray, const SelectionHitProxy& proxy) noexcept {
    const auto kind = proxy.boundsKind;
    const float major = std::max(0.01f, proxy.majorRadius);
    const float minor = std::max(0.01f, proxy.minorRadius);
    const float height = std::max(0.01f, proxy.height);
    const engine::render::RenderVector relative =
        ray.origin - proxy.worldPosition;

    if (kind == engine::render::ObjectUiSelectionBoundsKind::Sphere) {
        const float radius = major;
        const float b = relative.x() * ray.direction.x() +
            relative.y() * ray.direction.y() +
            relative.z() * ray.direction.z();
        const float c = relative.x() * relative.x() +
            relative.y() * relative.y() + relative.z() * relative.z() -
            radius * radius;
        const float discriminant = b * b - c;
        if (discriminant < 0.0f) return std::nullopt;
        const float root = std::sqrt(discriminant);
        const float nearT = -b - root;
        const float farT = -b + root;
        if (farT < 0.0f) return std::nullopt;
        return std::max(0.0f, nearT);
    }

    const float cosine = std::cos(proxy.yawRadians);
    const float sine = std::sin(proxy.yawRadians);
    const float originX = relative.x() * cosine + relative.y() * sine;
    const float originY = -relative.x() * sine + relative.y() * cosine;
    const float directionX =
        ray.direction.x() * cosine + ray.direction.y() * sine;
    const float directionY =
        -ray.direction.x() * sine + ray.direction.y() * cosine;

    if (kind == engine::render::ObjectUiSelectionBoundsKind::Box) {
        float nearT = 0.0f;
        float farT = std::numeric_limits<float>::max();
        if (!slab(originX, directionX, -major, major, nearT, farT) ||
            !slab(originY, directionY, -minor, minor, nearT, farT) ||
            !slab(relative.z(), ray.direction.z(), 0.0f, height,
                  nearT, farT)) {
            return std::nullopt;
        }
        return farT >= 0.0f
            ? std::optional<float>{std::max(0.0f, nearT)}
            : std::nullopt;
    }

    // Cylinder uses the authored major/minor ellipse and the same bottom-
    // centre Z convention as gameplay GeometryInfo.
    const float dx = directionX / major;
    const float dy = directionY / minor;
    const float ox = originX / major;
    const float oy = originY / minor;
    const float a = dx * dx + dy * dy;
    const float b = 2.0f * (ox * dx + oy * dy);
    const float c = ox * ox + oy * oy - 1.0f;
    float nearT = 0.0f;
    float farT = std::numeric_limits<float>::max();
    if (!slab(relative.z(), ray.direction.z(), 0.0f, height,
              nearT, farT)) {
        return std::nullopt;
    }
    if (a <= math::EPSILON) return c <= 0.0f ? std::optional<float>{nearT}
                                              : std::nullopt;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) return std::nullopt;
    const float root = std::sqrt(discriminant);
    float sideNear = (-b - root) / (2.0f * a);
    float sideFar = (-b + root) / (2.0f * a);
    if (sideNear > sideFar) std::swap(sideNear, sideFar);
    nearT = std::max(nearT, sideNear);
    farT = std::min(farT, sideFar);
    return nearT <= farT && farT >= 0.0f
        ? std::optional<float>{std::max(0.0f, nearT)} : std::nullopt;
}

struct SelectionEndpoint final {
    engine::render::WorldPreparationStamp stamp;
    container::Vector<SelectionHitProxy> proxies;
    container::Vector<OrderWaypointHitProxy> waypoints;
};

[[nodiscard]] std::optional<math::vec2> projectSelectionProxy(
    const SelectionHitProxy& proxy,
    const engine::render::RenderCameraSnapshot& camera,
    const engine::render::RenderViewportMetrics& viewport) noexcept {
    engine::render::RenderVector anchor = proxy.worldPosition;
    // GeometryInfo uses a bottom-centre origin for boxes/cylinders. Project
    // the selectable volume centre for drag selection; using the ground
    // contact point makes a rectangle visibly covering a tall unit miss it.
    if (proxy.boundsKind !=
        engine::render::ObjectUiSelectionBoundsKind::Sphere) {
        anchor[2] += std::max(0.0f, proxy.height) * 0.5f;
    }
    return engine::render::ObjectIconOverlayPresentation::projectWorldAnchor(
        anchor, camera, viewport);
}

[[nodiscard]] const SelectionHitProxy* findSelectionProxy(
    const SelectionEndpoint* endpoint,
    engine::ObjectId object) noexcept {
    if (!endpoint || !object) return nullptr;
    const auto found = std::lower_bound(
        endpoint->proxies.begin(), endpoint->proxies.end(), object,
        [](const SelectionHitProxy& proxy, engine::ObjectId sought) {
            return proxy.object.value < sought.value;
        });
    return found != endpoint->proxies.end() && found->object == object
        ? &*found : nullptr;
}

[[nodiscard]] SelectionHitProxy interpolateSelectionProxy(
    const SelectionHitProxy& current,
    const SelectionEndpoint* previous,
    float alpha) noexcept {
    SelectionHitProxy result = current;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    if (alpha >= 1.0f) return result;
    const SelectionHitProxy* from =
        findSelectionProxy(previous, current.object);
    if (!from) return result;
    result.worldPosition = from->worldPosition +
        (current.worldPosition - from->worldPosition) * alpha;
    result.worldRadius = std::lerp(
        from->worldRadius, current.worldRadius, alpha);
    result.majorRadius = std::lerp(
        from->majorRadius, current.majorRadius, alpha);
    result.minorRadius = std::lerp(
        from->minorRadius, current.minorRadius, alpha);
    result.height = std::lerp(from->height, current.height, alpha);
    const float yawDelta = std::remainder(
        current.yawRadians - from->yawRadians, math::TWO_PI);
    result.yawRadians = from->yawRadians + yawDelta * alpha;
    return result;
}

[[nodiscard]] std::optional<float> terrainHeightAt(
    const engine::render::TerrainRenderSnapshot& terrain,
    float worldX, float worldY) noexcept {
    if (!terrain.isValid() || !std::isfinite(worldX) ||
        !std::isfinite(worldY) || terrain.cellWorldSize <= math::EPSILON) {
        return std::nullopt;
    }
    const float gridX = worldX / terrain.cellWorldSize +
        static_cast<float>(terrain.borderSize);
    const float gridY = worldY / terrain.cellWorldSize +
        static_cast<float>(terrain.borderSize);
    if (gridX < 0.0f || gridY < 0.0f ||
        gridX > static_cast<float>(terrain.width - 1) ||
        gridY > static_cast<float>(terrain.height - 1)) {
        return std::nullopt;
    }
    const int32_t x0 = std::clamp(
        static_cast<int32_t>(std::floor(gridX)), 0, terrain.width - 2);
    const int32_t y0 = std::clamp(
        static_cast<int32_t>(std::floor(gridY)), 0, terrain.height - 2);
    const float tx = std::clamp(gridX - static_cast<float>(x0), 0.0f, 1.0f);
    const float ty = std::clamp(gridY - static_cast<float>(y0), 0.0f, 1.0f);
    const float bottom = std::lerp(
        terrain.heightWorld(x0, y0), terrain.heightWorld(x0 + 1, y0), tx);
    const float top = std::lerp(
        terrain.heightWorld(x0, y0 + 1),
        terrain.heightWorld(x0 + 1, y0 + 1), tx);
    return std::lerp(bottom, top, ty);
}

#if TD_DEBUG_ENABLED
void applyDebugWorldVisibilityOverride(
    engine::render::WorldRenderSnapshot& snapshot) {
    if (!snapshot.terrain || snapshot.terrain->width <= 1 ||
        snapshot.terrain->height <= 1) {
        return;
    }

    engine::render::LocalVisibilityRenderSnapshot visibility;
    visibility.presentationEpoch = snapshot.presentationEpoch;
    visibility.revision = 1;
    visibility.terrainLayoutRevision = snapshot.terrain->layoutRevision;
    visibility.observerPlayer = 0;
    visibility.width = snapshot.terrain->width - 1;
    visibility.height = snapshot.terrain->height - 1;
    visibility.borderSize = snapshot.terrain->borderSize;
    visibility.cellWorldSize = snapshot.terrain->cellWorldSize;
    visibility.enabled = true;
    visibility.dirtyRegion = {
        .minX = 0,
        .minY = 0,
        .maxX = visibility.width - 1,
        .maxY = visibility.height - 1,
    };
    visibility.cells.resize(
        static_cast<size_t>(visibility.width) *
        static_cast<size_t>(visibility.height));
    for (int32_t y = 0; y < visibility.height; ++y) {
        for (int32_t x = 0; x < visibility.width; ++x) {
            const int32_t band = (x * 3) / std::max(visibility.width, 1);
            visibility.cells[static_cast<size_t>(y) * visibility.width + x] =
                static_cast<uint8_t>(std::clamp(band, 0, 2));
        }
    }
    snapshot.localVisibility = std::move(visibility);
}
#endif

} // namespace

class PresentationCoordinator::Impl final {
public:
    Impl(RendererSubsystem& renderer, engine::AudioSubsystem& audio,
         runtime::GameLogicIntentMailbox& logicIntents)
        : m_renderer(renderer), m_audio(audio), m_logicIntents(logicIntents) {}

    void configureDebugOptions();
    void setGameProjection(const runtime::GameUiProjection& projection) {
        m_gameProjection = projection;
    }
    void setWaypointMode(bool enabled) noexcept {
        m_waypointMode = enabled;
    }
    void setWorldInputOcclusionQuery(
        WorldInputOcclusionQuery query) {
        m_worldInputOcclusionQuery = std::move(query);
    }
    void extractAndSubmit(int frameCount);
    void admitExtractedWorldFrame();
    void closeWorldFrameIngress() noexcept {
        m_extractedWorldFrames.close();
    }
    void admitRenderAnimationFeedback();
    void admitAudioCompletions();
    void admitRenderStartupReadiness();
    void applyNonSessionRenderQuality();
    [[nodiscard]] engine::UiDrawList recordUi(
        engine::TextureManager& textureManager,
        InGameGuiSubsystem& inGameGui, bool debugWorldOnly);
    void renderRecordedUi(engine::TextureManager& textureManager,
                          const engine::UiDrawList& uiDrawList);
    void render(engine::TextureManager& textureManager,
                InGameGuiSubsystem& inGameGui, bool debugWorldOnly);
    [[nodiscard]] std::optional<engine::render::RenderVector> radarWorldAt(
        float screenX, float screenY) const;
    [[nodiscard]] engine::ObjectId radarObjectAt(
        float screenX, float screenY) const;
    [[nodiscard]] std::optional<engine::render::RenderVector>
    lastRadarEventWorld() const noexcept { return m_lastRadarEventWorld; }
    [[nodiscard]] std::optional<engine::render::RenderVector> terrainWorldAt(
        float screenX, float screenY) const;
    [[nodiscard]] std::optional<math::vec2> projectWorldToVirtual(
        engine::render::RenderVector world) const;
    [[nodiscard]] engine::ObjectId selectableObjectAt(
        float screenX, float screenY) const;
    [[nodiscard]] std::optional<PresentedOrderWaypointTarget>
    orderWaypointAt(float screenX, float screenY) const;
    [[nodiscard]] engine::ObjectId targetableObjectAt(
        float screenX, float screenY, bool allowShrubbery,
        bool allowMines, bool forceAttack) const;
    [[nodiscard]] PresentedWorldInputTarget worldInputTargetAt(
        float screenX, float screenY, bool allowShrubbery,
        bool allowMines, bool forceAttack) const;
    [[nodiscard]] std::optional<engine::render::RenderVector>
    objectWorldPosition(engine::ObjectId object) const;
    [[nodiscard]] std::optional<engine::GameCameraState>
    presentedCamera() const;
    [[nodiscard]] container::Vector<engine::ObjectId>
    selectableObjectsInRectangle(float startX, float startY,
                                 float endX, float endY) const;
    [[nodiscard]] bool hasRadarInput() const noexcept {
        return m_radarInputVisible && static_cast<bool>(m_radarInputTerrain);
    }
    [[nodiscard]] bool worldOnlyPresentation() const noexcept {
        return m_worldOnlyPresentation;
    }

private:
    void updateHoverAndRadar(engine::render::WorldRenderSnapshot& snapshot);
    void refreshPresentedHover();
    void clearRadarInput() noexcept;
    [[nodiscard]] const SelectionEndpoint* selectionEndpoint(
        uint64_t worldRevision,
        const engine::render::RenderViewState& view) const noexcept;
    [[nodiscard]] std::optional<engine::render::RenderViewState>
    presentedView() const noexcept;

    RendererSubsystem& m_renderer;
    engine::AudioSubsystem& m_audio;
    runtime::GameLogicIntentMailbox& m_logicIntents;
    runtime::GameUiProjection m_gameProjection;
    WorldInputOcclusionQuery m_worldInputOcclusionQuery;
    container::SharedPtr<const engine::render::TerrainRenderSnapshot>
        m_radarInputTerrain;
    engine::render::ObjectUiRenderState m_radarInputObjects;
    container::SharedPtr<const engine::render::TerrainRenderSnapshot>
        m_worldInputTerrain;
    engine::render::TacticalRadarLayout m_radarInputLayout;
    bool m_radarInputPolicyVisible = false;
    bool m_radarInputVisible = false;
    float m_radarInputTargetZ = 0.0f;
    bool m_radarInputSpectator = false;
    container::Vector<SelectionEndpoint> m_selectionEndpoints;
    bool m_selectionInputValid = false;
    engine::ObjectId m_lastPostedHoveredObject =
        engine::INVALID_OBJECT_ID;
    uint64_t m_lastPostedHoverSessionRevision = 0;
    uint64_t m_lastWaypointPruneSelectionRevision = 0;
    bool m_waypointMode = false;
    bool m_sessionPresentationActive = false;
    uint64_t m_lastStartupReadyLoadingRevision = 0;
    uint64_t m_lastStartupReadySessionRevision = 0;
    uint64_t m_lastStartupFailedLoadingRevision = 0;
    uint64_t m_lastStartupFailedSessionRevision = 0;
    uint64_t m_lastStartupProgressViewRevision = 0;
    uint64_t m_nextRenderViewRevision = 1;
    engine::render::WorldPreparationStamp m_activeWorldDomain;
    std::atomic<uint64_t> m_retiredPresentationEpoch{0};
    std::atomic<uint64_t> m_retiredSessionRevision{0};
    // A world snapshot is replaceable state, not an event journal. Keeping
    // every confirmed endpoint when presentation runs below the logic rate
    // creates permanent input-to-image latency and unbounded memory growth.
    // Ordered one-shots travel on their dedicated FX/audio feedback streams.
    platform::runtime::LatestValueMailbox<ExtractedWorldFrame>
        m_extractedWorldFrames;
    std::optional<ExtractedWorldFrame> m_deferredExtractedWorldFrame;
    uint64_t m_lastPresentationSyncReportFrame = UINT64_MAX;
    uint64_t m_deferredAnimationFeedbackEpoch = 0;
    container::Vector<engine::render::RenderAnimationCompletionFeedback>
        m_deferredAnimationFeedback;
    bool m_worldOnlyPresentation = false;
    std::optional<engine::render::RenderVector> m_lastRadarEventWorld;
#if TD_DEBUG_ENABLED
    container::String m_debugWorldMap;
    bool m_debugWorldVisibilityOverride = false;
    uint64_t m_debugVisualObjectId = 0;
    container::String m_debugFxList;
    int m_debugFxDelayFrames = 10;
    math::vec3 m_debugFxOffset{0.0f, 0.0f, 1.0f};
    float m_debugFxRadius = 0.0f;
    bool m_debugFxSubmitted = false;
#endif
};

void PresentationCoordinator::Impl::configureDebugOptions() {
#if TD_DEBUG_ENABLED
    m_worldOnlyPresentation = engine::CommandLine::instance().getBoolParam(
        "debug-world-only", false);
    m_debugWorldMap =
        engine::CommandLine::instance().getParam("debug-world-map");
    m_debugWorldVisibilityOverride = engine::CommandLine::instance()
        .getBoolParam("debug-world-visibility", false);
    m_debugVisualObjectId = static_cast<uint64_t>(std::max(
        0, engine::CommandLine::instance().getIntParam(
               "debug-visual-object", 0)));
    m_debugFxList = engine::CommandLine::instance().getParam("debug-fx");
    m_debugFxDelayFrames = std::max(
        0, engine::CommandLine::instance().getIntParam(
               "debug-fx-delay-frames", 10));
    m_debugFxOffset = {
        engine::CommandLine::instance().getFloatParam("debug-fx-x", 0.0f),
        engine::CommandLine::instance().getFloatParam("debug-fx-y", 0.0f),
        engine::CommandLine::instance().getFloatParam("debug-fx-z", 1.0f),
    };
    m_debugFxRadius = std::max(
        0.0f,
        engine::CommandLine::instance().getFloatParam("debug-fx-radius", 0.0f));
#endif
}

std::optional<engine::render::RenderVector>
PresentationCoordinator::Impl::radarWorldAt(float screenX,
                                             float screenY) const {
    if (!hasRadarInput()) return std::nullopt;
    const std::optional<engine::render::RenderViewState> view =
        presentedView();
    if (!view || !view->viewport.valid()) return std::nullopt;
    const math::vec2 pointer = view->viewport.logicalToVirtual(
        {screenX, screenY});
    return engine::render::tacticalRadarWorldForPixel(
        *m_radarInputTerrain, m_radarInputLayout, pointer,
        m_radarInputTargetZ);
}

engine::ObjectId PresentationCoordinator::Impl::radarObjectAt(
    float screenX, float screenY) const {
    if (!hasRadarInput()) return engine::INVALID_OBJECT_ID;
    const std::optional<engine::render::RenderViewState> view =
        presentedView();
    if (!view || !view->viewport.valid()) {
        return engine::INVALID_OBJECT_ID;
    }
    const math::vec2 pointer = view->viewport.logicalToVirtual(
        {screenX, screenY});
    const std::optional<engine::render::RenderEntityId> hit =
        engine::render::tacticalRadarObjectHitTest(
            m_radarInputObjects, *m_radarInputTerrain,
            m_radarInputLayout, pointer, m_radarInputSpectator);
    if (!hit || *hit == 0 ||
        *hit > std::numeric_limits<uint32_t>::max()) {
        return engine::INVALID_OBJECT_ID;
    }
    return engine::ObjectId{static_cast<uint32_t>(*hit)};
}

std::optional<engine::render::RenderVector>
PresentationCoordinator::Impl::terrainWorldAt(
    float screenX, float screenY) const {
    if (!m_worldInputTerrain || !m_worldInputTerrain->isValid())
        return std::nullopt;
    const std::optional<engine::render::RenderViewState> view =
        presentedView();
    if (!view || !view->viewport.valid()) return std::nullopt;
    const math::vec2 pointer = view->viewport.logicalToVirtual(
        {screenX, screenY});
    const std::optional<SelectionRay> ray = selectionRay(
        view->camera, view->viewport, pointer);
    if (!ray) return std::nullopt;

    const engine::render::RenderVector minimum =
        m_worldInputTerrain->worldPosition(0, 0);
    const engine::render::RenderVector maximum =
        m_worldInputTerrain->worldPosition(
            m_worldInputTerrain->width - 1,
            m_worldInputTerrain->height - 1);
    float nearT = 0.0f;
    float farT = std::max(1.0f, view->camera.farClip);
    if (!slab(ray->origin.x(), ray->direction.x(),
              std::min(minimum.x(), maximum.x()),
              std::max(minimum.x(), maximum.x()), nearT, farT) ||
        !slab(ray->origin.y(), ray->direction.y(),
              std::min(minimum.y(), maximum.y()),
              std::max(minimum.y(), maximum.y()), nearT, farT) ||
        farT < 0.0f) {
        return std::nullopt;
    }
    nearT = std::max(nearT, 0.0f);
    if (nearT > farT) return std::nullopt;

    constexpr size_t kRayMarchSteps = 64u;
    float previousT = nearT;
    engine::render::RenderVector previousPoint =
        ray->origin + ray->direction * previousT;
    std::optional<float> previousHeight = terrainHeightAt(
        *m_worldInputTerrain, previousPoint.x(), previousPoint.y());
    float previousDelta = previousHeight
        ? previousPoint.z() - *previousHeight
        : std::numeric_limits<float>::max();
    for (size_t step = 1u; step <= kRayMarchSteps; ++step) {
        const float t = std::lerp(
            nearT, farT,
            static_cast<float>(step) /
                static_cast<float>(kRayMarchSteps));
        const engine::render::RenderVector point =
            ray->origin + ray->direction * t;
        const std::optional<float> height = terrainHeightAt(
            *m_worldInputTerrain, point.x(), point.y());
        if (!height) continue;
        const float delta = point.z() - *height;
        if (previousHeight && previousDelta >= 0.0f && delta <= 0.0f) {
            float low = previousT;
            float high = t;
            for (size_t iteration = 0; iteration < 16u; ++iteration) {
                const float middle = (low + high) * 0.5f;
                const engine::render::RenderVector sample =
                    ray->origin + ray->direction * middle;
                const std::optional<float> sampleHeight = terrainHeightAt(
                    *m_worldInputTerrain, sample.x(), sample.y());
                if (!sampleHeight || sample.z() > *sampleHeight)
                    low = middle;
                else
                    high = middle;
            }
            engine::render::RenderVector result =
                ray->origin + ray->direction * high;
            if (const std::optional<float> finalHeight = terrainHeightAt(
                    *m_worldInputTerrain, result.x(), result.y())) {
                result = {result.x(), result.y(), *finalHeight};
            }
            return result;
        }
        previousT = t;
        previousHeight = height;
        previousDelta = delta;
    }
    return std::nullopt;
}

std::optional<math::vec2>
PresentationCoordinator::Impl::projectWorldToVirtual(
    engine::render::RenderVector world) const {
    const std::optional<engine::render::RenderViewState> view =
        presentedView();
    if (!view || !view->viewport.valid()) return std::nullopt;
    return engine::render::ObjectIconOverlayPresentation::projectWorldAnchor(
        world, view->camera, view->viewport);
}

engine::ObjectId PresentationCoordinator::Impl::selectableObjectAt(
    float screenX, float screenY) const {
    return targetableObjectAt(
        screenX, screenY, false, false, false);
}

std::optional<PresentedOrderWaypointTarget>
PresentationCoordinator::Impl::orderWaypointAt(
    float screenX, float screenY) const {
    if (!m_selectionInputValid) return std::nullopt;
    const std::optional<engine::render::RenderViewState> view =
        presentedView();
    if (!view) return std::nullopt;
    const SelectionEndpoint* endpoint =
        selectionEndpoint(view->worldBRevision, *view);
    if (!endpoint) endpoint = selectionEndpoint(view->worldARevision, *view);
    if (!endpoint) return std::nullopt;
    const math::vec2 pointer = view->viewport.logicalToVirtual(
        {screenX, screenY});
    if (m_worldInputOcclusionQuery &&
        m_worldInputOcclusionQuery(pointer.x(), pointer.y())) {
        return std::nullopt;
    }
    const std::optional<SelectionRay> ray = selectionRay(
        view->camera, view->viewport, pointer);
    if (!ray) return std::nullopt;

    float bestDistance = std::numeric_limits<float>::max();
    const OrderWaypointHitProxy* best = nullptr;
    for (const OrderWaypointHitProxy& waypoint : endpoint->waypoints) {
        SelectionHitProxy volume;
        volume.worldPosition = waypoint.worldPosition;
        volume.boundsKind =
            engine::render::ObjectUiSelectionBoundsKind::Sphere;
        volume.majorRadius = std::max(1.0f, waypoint.radius);
        volume.minorRadius = volume.majorRadius;
        volume.height = volume.majorRadius * 2.0f;
        const std::optional<float> distance = rayHitDistance(*ray, volume);
        if (!distance) continue;
        if (!best || *distance < bestDistance ||
            (*distance == bestDistance &&
             waypoint.identity < best->identity)) {
            best = &waypoint;
            bestDistance = *distance;
        }
    }
    if (!best) return std::nullopt;
    return PresentedOrderWaypointTarget{
        .actor = best->actor,
        .sourceSequence = best->sourceSequence,
        .kind = best->kind,
    };
}

engine::ObjectId PresentationCoordinator::Impl::targetableObjectAt(
    float screenX, float screenY, bool allowShrubbery,
    bool allowMines, bool forceAttack) const {
    if (!m_selectionInputValid) return engine::INVALID_OBJECT_ID;
    const std::optional<engine::render::RenderViewState> view =
        presentedView();
    if (!view) return engine::INVALID_OBJECT_ID;
    const SelectionEndpoint* currentEndpoint =
        selectionEndpoint(view->worldBRevision, *view);
    const SelectionEndpoint* previousEndpoint =
        selectionEndpoint(view->worldARevision, *view);
    // A newest-value producer may legitimately skip an intermediate endpoint.
    // Use whichever displayed endpoint is retained and collapse interpolation
    // locally; rejecting the whole gesture leaves selection permanently inert
    // after one dropped A or B sample.
    if (!currentEndpoint) currentEndpoint = previousEndpoint;
    if (!previousEndpoint) previousEndpoint = currentEndpoint;
    if (!currentEndpoint) {
        return engine::INVALID_OBJECT_ID;
    }
    const container::Span<const SelectionHitProxy> proxies{
        currentEndpoint->proxies};
    const engine::render::RenderCameraSnapshot& camera = view->camera;
    const float alpha = view->interpolationAlpha;
    const math::vec2 pointer = view->viewport.logicalToVirtual(
        {screenX, screenY});
    if (m_worldInputOcclusionQuery &&
        m_worldInputOcclusionQuery(pointer.x(), pointer.y())) {
        return engine::INVALID_OBJECT_ID;
    }
    const std::optional<SelectionRay> ray = selectionRay(
        camera, view->viewport, pointer);
    if (!ray) return engine::INVALID_OBJECT_ID;
    float bestDistance = std::numeric_limits<float>::max();
    engine::ObjectId best = engine::INVALID_OBJECT_ID;
    for (const SelectionHitProxy& endpointProxy : proxies) {
        const SelectionHitProxy proxy = interpolateSelectionProxy(
            endpointProxy, previousEndpoint, alpha);
        const bool eligible = proxy.selectable ||
            (allowShrubbery && proxy.shrubberyTarget) ||
            (allowMines && proxy.mineTarget) ||
            (forceAttack && proxy.forceAttackable);
        if (!eligible ||
            proxy.visibility != engine::render::
                LocalVisibilityRenderCellState::Visible) {
            continue;
        }
        const std::optional<float> distance = rayHitDistance(*ray, proxy);
        if (!distance) continue;
        if (!best || *distance < bestDistance ||
            (*distance == bestDistance &&
             proxy.object.value < best.value)) {
            best = proxy.object;
            bestDistance = *distance;
        }
    }
    return best;
}

std::optional<engine::render::RenderVector>
PresentationCoordinator::Impl::objectWorldPosition(
    engine::ObjectId object) const {
    if (!object) return std::nullopt;
    const std::optional<engine::render::RenderViewState> view =
        presentedView();
    if (!view) return std::nullopt;
    const SelectionEndpoint* current =
        selectionEndpoint(view->worldBRevision, *view);
    const SelectionEndpoint* previous =
        selectionEndpoint(view->worldARevision, *view);
    if (!current) current = previous;
    if (!previous) previous = current;
    if (!current) return std::nullopt;
    const auto found = std::lower_bound(
        current->proxies.begin(), current->proxies.end(), object,
        [](const SelectionHitProxy& proxy, engine::ObjectId sought) {
            return proxy.object < sought;
        });
    if (found == current->proxies.end() || found->object != object) {
        return std::nullopt;
    }
    return interpolateSelectionProxy(*found, previous,
                                     view->interpolationAlpha)
        .worldPosition;
}

container::Vector<engine::ObjectId>
PresentationCoordinator::Impl::selectableObjectsInRectangle(
    float startX, float startY, float endX, float endY) const {
    container::Vector<engine::ObjectId> result;
    if (!m_selectionInputValid) return result;
    const std::optional<engine::render::RenderViewState> view =
        presentedView();
    if (!view) return result;
    const SelectionEndpoint* currentEndpoint =
        selectionEndpoint(view->worldBRevision, *view);
    const SelectionEndpoint* previousEndpoint =
        selectionEndpoint(view->worldARevision, *view);
    if (!currentEndpoint) currentEndpoint = previousEndpoint;
    if (!previousEndpoint) previousEndpoint = currentEndpoint;
    if (!currentEndpoint) return result;
    const container::Span<const SelectionHitProxy> proxies{
        currentEndpoint->proxies};
    const engine::render::RenderCameraSnapshot& camera = view->camera;
    const float alpha = view->interpolationAlpha;
    const math::vec2 first = view->viewport.logicalToVirtual(
        {startX, startY});
    const math::vec2 second = view->viewport.logicalToVirtual(
        {endX, endY});
    const float left = std::min(first.x(), second.x());
    const float right = std::max(first.x(), second.x());
    const float top = std::min(first.y(), second.y());
    const float bottom = std::max(first.y(), second.y());
    for (const SelectionHitProxy& endpointProxy : proxies) {
        const SelectionHitProxy proxy = interpolateSelectionProxy(
            endpointProxy, previousEndpoint, alpha);
        // Keep locally controlled structures in the candidate set. The
        // logic-thread selection policy normally rejects structures from a
        // drag, but needs the complete set for ZH's unique-building exception.
        // Presentation only finds geometric candidates. Ownership and the
        // ZH unit/structure drag rules are authoritative in
        // LocalSelectionPolicy; duplicating ownership here lets a stale or
        // observer-relative render relationship erase otherwise valid units.
        if (!proxy.selectable ||
            proxy.visibility != engine::render::
                LocalVisibilityRenderCellState::Visible) {
            continue;
        }
        const auto center = projectSelectionProxy(
            proxy, camera, view->viewport);
        if (!center || center->x() < left || center->x() > right ||
            center->y() < top || center->y() > bottom) {
            continue;
        }
        // RefCode CanSelectDrawable projects each candidate and rejects it
        // when the deepest WND or any parent is opaque. Testing only the drag
        // rectangle endpoints lets units underneath the ControlBar leak into
        // an otherwise valid world capture.
        if (m_worldInputOcclusionQuery &&
            m_worldInputOcclusionQuery(center->x(), center->y())) {
            continue;
        }
        result.push_back(proxy.object);
    }
    std::sort(result.begin(), result.end(),
              [](engine::ObjectId lhs, engine::ObjectId rhs) {
                  return lhs.value < rhs.value;
              });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

const SelectionEndpoint* PresentationCoordinator::Impl::selectionEndpoint(
    uint64_t worldRevision,
    const engine::render::RenderViewState& view) const noexcept {
    const auto found = std::find_if(
        m_selectionEndpoints.begin(), m_selectionEndpoints.end(),
        [worldRevision, &view](const SelectionEndpoint& endpoint) {
            return endpoint.stamp.worldRevision == worldRevision &&
                endpoint.stamp.presentationEpoch ==
                    view.sourceWorld.presentationEpoch &&
                endpoint.stamp.sessionRevision ==
                    view.sourceWorld.sessionRevision &&
                endpoint.stamp.loadingRevision ==
                    view.sourceWorld.loadingRevision;
        });
    return found == m_selectionEndpoints.end() ? nullptr : &*found;
}

std::optional<engine::render::RenderViewState>
PresentationCoordinator::Impl::presentedView() const noexcept {
    std::optional<engine::render::RenderViewState> view =
        m_renderer.lastPresentedRenderView();
    if (!view || view->sourceWorld.sessionRevision !=
            m_gameProjection.sessionRevision ||
        view->sourceWorld.presentationEpoch == 0u) {
        return std::nullopt;
    }
    return view;
}

std::optional<engine::GameCameraState>
PresentationCoordinator::Impl::presentedCamera() const {
    const std::optional<engine::render::RenderViewState> view =
        presentedView();
    if (!view) return std::nullopt;
    const engine::render::RenderCameraSnapshot& source = view->camera;
    return engine::GameCameraState{
        .position = source.position,
        .target = source.target,
        .up = source.up,
        .verticalFovRadians = source.verticalFovRadians,
        .horizontalFovRadians = source.horizontalFovRadians,
        .tacticalViewportHeightScale =
            source.tacticalViewportHeightScale,
        .nearClip = source.nearClip,
        .farClip = source.farClip,
        .visibilityDistance = source.visibilityDistance,
        .fogEnabled = source.fogEnabled,
        .fogColor = source.fogColor,
        .fogStartDistance = source.fogStartDistance,
        .fogEndDistance = source.fogEndDistance,
        .cameraCutRevision = source.cameraCutRevision,
    }.sanitized();
}

void PresentationCoordinator::Impl::clearRadarInput() noexcept {
    m_radarInputTerrain.reset();
    m_radarInputObjects = {};
    m_radarInputLayout = {};
    m_radarInputPolicyVisible = false;
    m_radarInputVisible = false;
    m_radarInputTargetZ = 0.0f;
    m_radarInputSpectator = false;
}

void PresentationCoordinator::Impl::updateHoverAndRadar(
    engine::render::WorldRenderSnapshot& snapshot) {
    if (!snapshot.tacticalRadar.events.empty()) {
        m_lastRadarEventWorld = snapshot.tacticalRadar.events.back().worldPosition;
    } else {
        m_lastRadarEventWorld.reset();
    }
    container::Vector<SelectionHitProxy> selectionHitProxies;
    selectionHitProxies.reserve(snapshot.objectUi.objects.size());
    for (const engine::render::ObjectUiRenderSnapshot& object :
         snapshot.objectUi.objects) {
        if (object.objectId == 0 ||
            object.objectId > std::numeric_limits<uint32_t>::max()) {
            continue;
        }
        selectionHitProxies.push_back({
            .object = engine::ObjectId{
                static_cast<uint32_t>(object.objectId)},
            .worldPosition = object.worldPosition,
            .worldRadius = object.worldRadius,
            .boundsKind = object.selectionBounds,
            .majorRadius = object.selectionMajorRadius,
            .minorRadius = object.selectionMinorRadius,
            .height = object.selectionHeight,
            .yawRadians = object.selectionYawRadians,
            .selectable = object.selectable,
            .shrubberyTarget = object.shrubberyTarget,
            .mineTarget = object.mineTarget,
            .forceAttackable = object.forceAttackable,
            .owned = object.relationship ==
                engine::render::ObjectUiRelationship::Owned,
            .structure = object.structure,
            .visibility = object.visibility,
        });
    }
    std::sort(
        selectionHitProxies.begin(), selectionHitProxies.end(),
        [](const SelectionHitProxy& left, const SelectionHitProxy& right) {
            return left.object.value < right.object.value;
        });
    container::Vector<OrderWaypointHitProxy> waypointHitProxies;
    waypointHitProxies.reserve(snapshot.objectUi.waypoints.size());
    for (const engine::render::OrderWaypointRenderSnapshot& waypoint :
         snapshot.objectUi.waypoints) {
        if (!waypoint.identity || waypoint.actorObjectId == 0 ||
            waypoint.sourceSequence == 0) {
            continue;
        }
        const engine::selection::LocalOrderWaypointKind kind = [&] {
            switch (waypoint.kind) {
            case engine::render::OrderWaypointRenderKind::Move:
                return engine::selection::LocalOrderWaypointKind::Move;
            case engine::render::OrderWaypointRenderKind::Attack:
                return engine::selection::LocalOrderWaypointKind::Attack;
            case engine::render::OrderWaypointRenderKind::AttackMove:
                return engine::selection::LocalOrderWaypointKind::AttackMove;
            case engine::render::OrderWaypointRenderKind::Build:
                return engine::selection::LocalOrderWaypointKind::Build;
            case engine::render::OrderWaypointRenderKind::Guard:
                return engine::selection::LocalOrderWaypointKind::Guard;
            case engine::render::OrderWaypointRenderKind::Ability:
                return engine::selection::LocalOrderWaypointKind::Ability;
            }
            return engine::selection::LocalOrderWaypointKind::Move;
        }();
        waypointHitProxies.push_back({
            .identity = waypoint.identity,
            .actor = engine::ObjectId{waypoint.actorObjectId},
            .sourceSequence = waypoint.sourceSequence,
            .kind = kind,
            .worldPosition = waypoint.worldPosition,
            .radius = std::max(1.0f, waypoint.selectionRadius),
        });
    }
    if (m_gameProjection.commandUi.selectedOrderWaypoint) {
        const bool stillPresented = std::any_of(
            waypointHitProxies.begin(), waypointHitProxies.end(),
            [&](const OrderWaypointHitProxy& waypoint) {
                return waypoint.actor ==
                           m_gameProjection.commandUi.selectedObject &&
                    waypoint.sourceSequence == m_gameProjection.commandUi.
                        selectedOrderWaypointSourceSequence &&
                    waypoint.kind == m_gameProjection.commandUi.
                        selectedOrderWaypointKind;
            });
        const uint64_t selectionRevision =
            m_gameProjection.commandUi.selectionRevision;
        if (!stillPresented && selectionRevision != 0 &&
            m_lastWaypointPruneSelectionRevision != selectionRevision &&
            m_logicIntents.post(
                runtime::ResetLocalSelectionIntent{},
                m_gameProjection.sessionRevision)) {
            m_lastWaypointPruneSelectionRevision = selectionRevision;
        }
    } else {
        m_lastWaypointPruneSelectionRevision = 0;
    }
    m_selectionInputValid = true;
    SelectionEndpoint endpoint{
        .stamp = {
            .worldRevision = snapshot.simulationFrame,
            .simulationFrame = snapshot.simulationFrame,
            .presentationEpoch = snapshot.presentationEpoch,
            .sessionRevision = snapshot.sessionRevision,
            .loadingRevision = snapshot.loadingRevision,
        },
        .proxies = std::move(selectionHitProxies),
        .waypoints = std::move(waypointHitProxies),
    };
    std::erase_if(
        m_selectionEndpoints,
        [&endpoint](const SelectionEndpoint& existing) {
            return existing.stamp.worldRevision ==
                    endpoint.stamp.worldRevision &&
                existing.stamp.presentationEpoch ==
                    endpoint.stamp.presentationEpoch &&
                existing.stamp.sessionRevision ==
                    endpoint.stamp.sessionRevision &&
                existing.stamp.loadingRevision ==
                    endpoint.stamp.loadingRevision;
        });
    m_selectionEndpoints.push_back(std::move(endpoint));
    // Two seconds at the authoritative 30 Hz logic cadence covers ordinary
    // render lag.  In addition, never retire an endpoint referenced by the
    // renderer's displayed A/B view even when presentation is further behind.
    constexpr size_t kRetainedSelectionEndpoints = 64u;
    const std::optional<engine::render::RenderViewState> displayedView =
        m_renderer.lastPresentedRenderView();
    while (m_selectionEndpoints.size() > kRetainedSelectionEndpoints) {
        const auto retire = std::find_if(
            m_selectionEndpoints.begin(), m_selectionEndpoints.end(),
            [&displayedView](const SelectionEndpoint& candidate) {
                if (!displayedView) return true;
                const bool sameDomain =
                    candidate.stamp.presentationEpoch ==
                        displayedView->sourceWorld.presentationEpoch &&
                    candidate.stamp.sessionRevision ==
                        displayedView->sourceWorld.sessionRevision &&
                    candidate.stamp.loadingRevision ==
                        displayedView->sourceWorld.loadingRevision;
                return !sameDomain ||
                    (candidate.stamp.worldRevision !=
                         displayedView->worldARevision &&
                     candidate.stamp.worldRevision !=
                         displayedView->worldBRevision);
            });
        if (retire == m_selectionEndpoints.end()) break;
        m_selectionEndpoints.erase(retire);
    }

    if (snapshot.tacticalRadar.visible && snapshot.terrain &&
        displayedView && displayedView->viewport.valid()) {
        m_radarInputTerrain = snapshot.terrain;
        m_radarInputObjects = snapshot.objectUi;
        m_radarInputPolicyVisible = true;
        m_radarInputLayout = {};
        m_radarInputTargetZ = snapshot.camera.target.z();
        m_radarInputSpectator = snapshot.tacticalRadar.spectator;
        m_radarInputVisible = false;
    } else {
        clearRadarInput();
    }
    m_worldInputTerrain = snapshot.terrain;
}

void PresentationCoordinator::Impl::refreshPresentedHover() {
    if (!m_selectionInputValid || !m_gameProjection.hasSession) return;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    static_cast<void>(SDL_GetMouseState(&pointerX, &pointerY));
    const std::optional<engine::render::RenderVector> radar =
        radarWorldAt(pointerX, pointerY);
    const engine::ObjectId hovered = radar
        ? radarObjectAt(pointerX, pointerY)
        : selectableObjectAt(pointerX, pointerY);
    if (hovered == m_lastPostedHoveredObject &&
        m_lastPostedHoverSessionRevision ==
            m_gameProjection.sessionRevision) {
        return;
    }
    if (!m_logicIntents.post(
        runtime::SetHoveredObjectIntent{.object = hovered},
        m_gameProjection.sessionRevision)) {
        return;
    }
    m_lastPostedHoveredObject = hovered;
    m_lastPostedHoverSessionRevision = m_gameProjection.sessionRevision;
}

void PresentationCoordinator::Impl::extractAndSubmit(int frameCount) {
    engine::GameLogic& gameLogic = engine::GameLogic::instance();
    engine::GameSession* session = gameLogic.currentSession();
    if (!session) {
        if (m_sessionPresentationActive) {
            m_retiredPresentationEpoch.store(
                m_activeWorldDomain.presentationEpoch,
                std::memory_order_release);
            m_retiredSessionRevision.store(
                m_activeWorldDomain.sessionRevision,
                std::memory_order_release);
            m_renderer.retireWorldPresentation(m_activeWorldDomain);
            m_activeWorldDomain = {};
            m_sessionPresentationActive = false;
        }
        static_cast<void>(m_extractedWorldFrames.publish({
            .sessionRevision =
                gameLogic.sessionRevision(),
            .sessionActive = false,
        }));
        m_audio.clearPresentationSession();
        m_renderer.clearGroundDecalPresentation();
#if TD_DEBUG_ENABLED
        if (m_debugFxList.empty()) m_renderer.clearFxPresentation();
#else
        m_renderer.clearFxPresentation();
#endif
        return;
    }

    const uint64_t sessionRevision = gameLogic.sessionRevision();
    const engine::GameSessionPresentationSnapshot sessionPresentation =
        session->presentationPort().snapshot();
    const uint64_t presentationEpoch = sessionPresentation.scriptEpoch;
    if (m_sessionPresentationActive &&
        m_activeWorldDomain.presentationEpoch != 0u &&
        (m_activeWorldDomain.presentationEpoch != presentationEpoch ||
         m_activeWorldDomain.sessionRevision != sessionRevision)) {
        // Next/Retry and rollback replace one live session with another; they
        // do not pass through a null-session extraction. Retire the previous
        // domain before publishing the candidate so old prepared frames,
        // animation feedback and main-thread selection endpoints share the
        // same tombstone boundary as an ordinary session shutdown.
        m_retiredPresentationEpoch.store(
            m_activeWorldDomain.presentationEpoch,
            std::memory_order_release);
        m_retiredSessionRevision.store(
            m_activeWorldDomain.sessionRevision,
            std::memory_order_release);
        m_renderer.retireWorldPresentation(m_activeWorldDomain);
        m_activeWorldDomain = {};
        m_sessionPresentationActive = false;
    }
    m_sessionPresentationActive = true;
    const bool loading = gameLogic.isLoading();
    const engine::GamePresentationContentSnapshot presentationContent =
        session->mediaPresentationPort().content();
    static_cast<void>(m_audio.activatePresentationSession(
        sessionPresentation.audioEpoch,
        presentationContent.audioContentLayers()));

    const engine::GameCameraState& camera =
        sessionPresentation.camera;
#if TD_DEBUG_ENABLED
    if (!loading && !m_debugFxSubmitted && !m_debugFxList.empty() &&
        frameCount >= m_debugFxDelayFrames) {
        const math::vec3 position = camera.target + m_debugFxOffset;
        m_debugFxSubmitted = session->presentationPort().emitFxInvocation({
            .fxListName = m_debugFxList,
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = {.position = position},
            .overrideRadius = m_debugFxRadius,
        });
        if (m_debugFxSubmitted) {
            TD_LOG_INFO(
                "[DebugFx] Submitted '{}' at ({:.2f}, {:.2f}, {:.2f}), radius={:.2f}",
                m_debugFxList, position.x(), position.y(), position.z(),
                m_debugFxRadius);
        }
    }
#endif

    if (const auto slaveListener =
        m_renderer.scriptCameraSlaveListenerOverride(
                sessionPresentation.scriptEpoch)) {
        m_audio.publishPresentationCameraListener(
            slaveListener->position, slaveListener->target, slaveListener->up);
    } else {
        m_audio.publishListener(camera);
    }

    const auto& featureQuality = sessionPresentation.featureQuality;
    const auto renderQuality =
        game::GameDataLoader::instance().renderQualitySettingsSnapshot();
    if (featureQuality && renderQuality) {
        m_renderer.applyRenderQualitySettings(
            *featureQuality, renderQuality->display);
    }
    if (loading) {
        // Session bootstrap may enqueue authored speech, music controls,
        // one-shot FX and decals before the startup scene is ready. Do not
        // consume any of those transient streams under the Loading WND: an
        // opening line, launch effect or explosion could otherwise finish
        // before the player sees the scene. Empty value snapshots establish
        // the new epochs and retire presentation left by the previous map.
        engine::audio::AudioPresentationSnapshot silentBootstrap;
        silentBootstrap.sessionEpoch = sessionPresentation.audioEpoch;
        silentBootstrap.simulationFrame = session->confirmedTick();
        m_audio.submitPresentationSnapshot(std::move(silentBootstrap));

        engine::fx::FxPresentationSnapshot emptyFxBootstrap;
        emptyFxBootstrap.sessionEpoch = sessionPresentation.fxEpoch;
        emptyFxBootstrap.simulationFrame = session->confirmedTick();
        emptyFxBootstrap.logicFramesPerSecond = static_cast<uint32_t>(
            std::max(1, sessionPresentation.gameSpeedFramesPerSecond));
        m_renderer.submitFxSnapshot(
            emptyFxBootstrap,
            presentationContent.particleSystems,
            presentationContent.fxLists,
            *sessionPresentation.renderSettings);
        m_renderer.clearGroundDecalPresentation(
            sessionPresentation.scriptEpoch);
    } else {
        m_renderer.submitFxSnapshot(
            session->mediaPresentationPort().takeFx(
                session->confirmedTick()),
            presentationContent.particleSystems,
            presentationContent.fxLists,
            *sessionPresentation.renderSettings);
        m_renderer.submitGroundDecalPresentation(
            session->renderExtractionPort().takeGroundDecals());
        engine::audio::AudioPresentationSnapshot audioPresentation =
            session->mediaPresentationPort().takeAudio(
                session->confirmedTick());
        m_audio.appendFxSoundCommands(
            audioPresentation, m_renderer.takeFxSoundCommands());
        m_audio.submitPresentationSnapshot(std::move(audioPresentation));
    }

    engine::selection::LocalSelectionState& localSelection =
        engine::GameLogic::instance().localSelection();
    container::Vector<engine::ObjectId> extractionSelection{
        localSelection.selected().begin(),
        localSelection.selected().end()};
    if (const engine::selection::LocalOrderWaypointSelection waypoint =
            localSelection.selectedOrderWaypoint();
        waypoint && !std::binary_search(
            extractionSelection.begin(), extractionSelection.end(),
            waypoint.actor)) {
        extractionSelection.insert(
            std::lower_bound(extractionSelection.begin(),
                             extractionSelection.end(), waypoint.actor),
            waypoint.actor);
    }
    engine::GameSessionRenderExtractionPort extraction =
        session->renderExtractionPort();
    auto worldSnapshot = extraction.world(
        extraction.camera(camera),
        session->confirmedTick(), extractionSelection,
        localSelection.hovered(),
        m_waypointMode,
        loading);
    worldSnapshot.sessionRevision =
        sessionRevision;
    worldSnapshot.loadingRevision = loading
        ? gameLogic.loadingRevision() : 0u;
#if TD_DEBUG_ENABLED
    worldSnapshot.debugVisualTraceObjectId = m_debugVisualObjectId;
#endif
    m_activeWorldDomain = {
        .worldRevision = worldSnapshot.simulationFrame,
        .simulationFrame = worldSnapshot.simulationFrame,
        .presentationEpoch = worldSnapshot.presentationEpoch,
        .sessionRevision = worldSnapshot.sessionRevision,
        .loadingRevision = worldSnapshot.loadingRevision,
    };
    // In-game UI is an overlay. The tactical world always occupies the full
    // render target; hiding UI only changes overlay visibility.
    worldSnapshot.camera.tacticalViewportHeightScale = 1.0f;
    if (m_worldOnlyPresentation) {
        worldSnapshot.tacticalRadar = {};
    }
#if TD_DEBUG_ENABLED
    if (m_debugWorldVisibilityOverride && !m_debugWorldMap.empty()) {
        applyDebugWorldVisibilityOverride(worldSnapshot);
    }
#endif
    const uint64_t extractedSimulationFrame = worldSnapshot.simulationFrame;
    static_cast<void>(m_extractedWorldFrames.publish({
        .sessionRevision = sessionRevision,
        .sessionActive = true,
        .snapshot = std::move(worldSnapshot),
    }));
    if (extractedSimulationFrame % 300u == 0u &&
        m_lastPresentationSyncReportFrame != extractedSimulationFrame) {
        m_lastPresentationSyncReportFrame = extractedSimulationFrame;
        const auto rendered = m_renderer.lastWorldFrameStats();
        const uint64_t displayedSimulationFrame = rendered
            ? rendered->simulationFrame
            : 0u;
        const uint64_t lag = extractedSimulationFrame > displayedSimulationFrame
            ? extractedSimulationFrame - displayedSimulationFrame
            : 0u;
        TD_LOG_INFO(
            "[PresentationSync] confirmed={} displayed={} lag={} extractedQueue={} rendererQueue={}",
            extractedSimulationFrame, displayedSimulationFrame, lag,
            m_extractedWorldFrames.size(),
            m_renderer.queuedWorldSnapshotCount());
    }
}

void PresentationCoordinator::Impl::admitExtractedWorldFrame() {
    // Always replace an unsent endpoint with the newest complete state. If
    // the renderer is temporarily full, replaying the older deferred value
    // first would recreate a FIFO backlog on the main thread.
    ExtractedWorldFrame newest;
    if (m_extractedWorldFrames.tryTake(newest)) {
        m_deferredExtractedWorldFrame = std::move(newest);
    }
    while (m_deferredExtractedWorldFrame) {
        ExtractedWorldFrame& frame = *m_deferredExtractedWorldFrame;
        if (!frame.sessionActive || !frame.snapshot) {
            clearRadarInput();
            m_worldInputTerrain.reset();
            m_selectionEndpoints.clear();
            m_selectionInputValid = false;
            m_lastPostedHoveredObject = engine::INVALID_OBJECT_ID;
            m_lastPostedHoverSessionRevision = 0;
            m_deferredExtractedWorldFrame.reset();
            return;
        }
        const uint64_t retiredEpoch = m_retiredPresentationEpoch.load(
            std::memory_order_acquire);
        const uint64_t retiredSession = m_retiredSessionRevision.load(
            std::memory_order_acquire);
        if ((retiredEpoch != 0u &&
             frame.snapshot->presentationEpoch <= retiredEpoch) ||
            (retiredSession != 0u &&
             frame.snapshot->sessionRevision <= retiredSession)) {
            m_deferredExtractedWorldFrame.reset();
            return;
        }
        if (frame.sessionRevision != m_gameProjection.sessionRevision) {
            m_deferredExtractedWorldFrame.reset();
            return;
        }
        if (!m_renderer.canAcceptWorldSnapshot()) return;
        updateHoverAndRadar(*frame.snapshot);
        uint64_t viewRevision = m_nextRenderViewRevision++;
        if (viewRevision == 0u) viewRevision = m_nextRenderViewRevision++;
        const engine::render::WorldPreparationStamp sourceWorld{
            .worldRevision = frame.snapshot->simulationFrame,
            .simulationFrame = frame.snapshot->simulationFrame,
            .presentationEpoch = frame.snapshot->presentationEpoch,
            .sessionRevision = frame.snapshot->sessionRevision,
            .loadingRevision = frame.snapshot->loadingRevision,
        };
        m_renderer.submitRenderViewState({
            .sourceWorld = sourceWorld,
            .viewRevision = viewRevision,
            .worldARevision = sourceWorld.worldRevision,
            .worldBRevision = sourceWorld.worldRevision,
            .camera = frame.snapshot->camera,
        });
        if (!m_renderer.submitWorldSnapshot(*frame.snapshot)) return;
        m_deferredExtractedWorldFrame.reset();
    }
}

void PresentationCoordinator::Impl::admitAudioCompletions() {
    if (engine::GameSession* session =
            engine::GameLogic::instance().currentSession()) {
        static_cast<void>(session->mediaPresentationPort()
            .admitAudioCompletions(m_audio.takeNaturalCompletions()));
    }
}

void PresentationCoordinator::Impl::admitRenderAnimationFeedback() {
    container::Vector<engine::render::RenderAnimationCompletionFeedback>
        incoming = m_renderer.takeAnimationCompletions();
    engine::GameSession* session =
        engine::GameLogic::instance().currentSession();
    if (!session) {
        m_deferredAnimationFeedback.clear();
        m_deferredAnimationFeedbackEpoch = 0;
        return;
    }

    engine::GameSessionPresentationPort presentation =
        session->presentationPort();
    const uint64_t epoch = presentation.snapshot().scriptEpoch;
    if (epoch == 0 || m_deferredAnimationFeedbackEpoch != epoch) {
        m_deferredAnimationFeedback.clear();
        m_deferredAnimationFeedbackEpoch = epoch;
    }

    const auto record = [&presentation](
        const engine::render::RenderAnimationCompletionFeedback& feedback) {
        static_cast<void>(presentation.recordAnimationCompletion(feedback));
    };
    if (!engine::GameLogic::instance().isLoading()) {
        for (const auto& feedback : m_deferredAnimationFeedback) {
            record(feedback);
        }
        m_deferredAnimationFeedback.clear();
        for (const auto& feedback : incoming) record(feedback);
        return;
    }

    // The startup endpoint itself is already an observable renderer fact and
    // must reach GameSession before tick zero. Otherwise a state asserted for
    // exactly tick zero and cleared on tick one can return to the initial
    // value before that initial generation is admitted, erasing the transient
    // entirely. Resource readiness and natural completion remain deferred so
    // Loading cannot advance an authored animation clock behind the WND.
    for (const auto& feedback : incoming) {
        if (feedback.kind ==
            engine::render::RenderAnimationFeedbackKind::EndpointPublished) {
            record(feedback);
            continue;
        }
        const auto duplicate = std::find_if(
            m_deferredAnimationFeedback.begin(),
            m_deferredAnimationFeedback.end(),
            [&feedback](const auto& current) {
                return current.presentationEpoch == feedback.presentationEpoch &&
                    current.objectId == feedback.objectId &&
                    current.channelIndex == feedback.channelIndex &&
                    current.generation == feedback.generation &&
                    current.phase == feedback.phase;
            });
        if (duplicate != m_deferredAnimationFeedback.end()) {
            if (feedback.simulationFrame > duplicate->simulationFrame ||
                (feedback.simulationFrame == duplicate->simulationFrame &&
                 feedback.kind > duplicate->kind)) {
                *duplicate = feedback;
            }
            continue;
        }
        // A completion is a terminal fact for one object/channel generation.
        // Never evict an unrelated fact merely because Loading has delayed
        // admission; doing so can leave that visual channel permanently
        // waiting for a completion which the renderer already published.
        m_deferredAnimationFeedback.push_back(feedback);
    }
}

void PresentationCoordinator::Impl::admitRenderStartupReadiness() {
    engine::GameLogic& gameLogic = engine::GameLogic::instance();
    if (!gameLogic.isLoading()) {
        m_lastStartupReadyLoadingRevision = 0;
        m_lastStartupReadySessionRevision = 0;
        m_lastStartupFailedLoadingRevision = 0;
        m_lastStartupFailedSessionRevision = 0;
        m_lastStartupProgressViewRevision = 0;
        return;
    }
    const auto stats = m_renderer.lastWorldFrameStats();
    if (!stats ||
        stats->loadingRevision != gameLogic.loadingRevision() ||
        stats->sessionRevision != gameLogic.sessionRevision()) {
        return;
    }
    if (stats->viewRevision != m_lastStartupProgressViewRevision) {
        engine::StartupSceneProgress progress;
        using RenderState = engine::render::StartupSceneTicketState;
        switch (stats->startupSceneTicket.state) {
        case RenderState::Inactive:
            progress.state = engine::StartupSceneProgressState::Inactive;
            break;
        case RenderState::Pending:
            progress.state = engine::StartupSceneProgressState::Pending;
            break;
        case RenderState::Ready:
            progress.state = engine::StartupSceneProgressState::Ready;
            break;
        case RenderState::Degraded:
            progress.state = engine::StartupSceneProgressState::Degraded;
            break;
        case RenderState::Failed:
            progress.state = engine::StartupSceneProgressState::Failed;
            break;
        }
        progress.requiredTotal = stats->startupSceneTicket.requiredTotal;
        progress.requiredReady = stats->startupSceneTicket.requiredReady;
        progress.requiredPending = stats->startupSceneTicket.requiredPending;
        progress.requiredFailed = stats->startupSceneTicket.requiredFailed;
        progress.optionalTotal = stats->startupSceneTicket.optionalTotal;
        progress.optionalReady = stats->startupSceneTicket.optionalReady;
        progress.optionalPending = stats->startupSceneTicket.optionalPending;
        progress.optionalDegraded =
            stats->startupSceneTicket.optionalDegraded;
        if (m_logicIntents.post(
                runtime::NotifyRenderStartupProgressIntent{
                    .loadingRevision = stats->loadingRevision,
                    .sessionRevision = stats->sessionRevision,
                    .progress = progress,
                }, stats->sessionRevision)) {
            m_lastStartupProgressViewRevision = stats->viewRevision;
        }
    }
    if (stats->startupSceneFailed) {
        if (m_lastStartupFailedLoadingRevision == stats->loadingRevision &&
            m_lastStartupFailedSessionRevision == stats->sessionRevision) {
            return;
        }
        if (m_logicIntents.post(
                runtime::NotifyRenderStartupFailureIntent{
                    .loadingRevision = stats->loadingRevision,
                    .sessionRevision = stats->sessionRevision,
                    .error = "renderer failed to prepare required startup terrain",
                }, stats->sessionRevision)) {
            m_lastStartupFailedLoadingRevision = stats->loadingRevision;
            m_lastStartupFailedSessionRevision = stats->sessionRevision;
        }
        return;
    }
    if (!stats->startupSceneReady) return;
    if (m_lastStartupReadyLoadingRevision == stats->loadingRevision &&
        m_lastStartupReadySessionRevision == stats->sessionRevision) {
        return;
    }
    if (m_logicIntents.post(
            runtime::NotifyRenderStartupFrameSubmittedIntent{
                .loadingRevision = stats->loadingRevision,
                .sessionRevision = stats->sessionRevision,
            }, stats->sessionRevision)) {
        m_lastStartupReadyLoadingRevision = stats->loadingRevision;
        m_lastStartupReadySessionRevision = stats->sessionRevision;
    }
}

void PresentationCoordinator::Impl::applyNonSessionRenderQuality() {
    if (engine::GameLogic::instance().currentSession()) return;
    const auto quality =
        game::GameDataLoader::instance().renderQualitySettingsSnapshot();
    if (quality) {
        m_renderer.applyRenderQualitySettings(
            quality->feature, quality->display);
    }
}

void PresentationCoordinator::Impl::render(
    engine::TextureManager& textureManager, InGameGuiSubsystem& inGameGui,
    bool debugWorldOnly) {
    engine::UiDrawList uiDrawList = recordUi(
        textureManager, inGameGui, debugWorldOnly);
    renderRecordedUi(textureManager, uiDrawList);
}

engine::UiDrawList PresentationCoordinator::Impl::recordUi(
    engine::TextureManager& textureManager, InGameGuiSubsystem& inGameGui,
    bool debugWorldOnly) {
    engine::UiDrawList drawList;
    refreshPresentedHover();
    engine::UiDrawListRenderer recorder(
        drawList, engine::Renderer::instance());
    if (m_gameProjection.isGameDomain()) {
        inGameGui.setExternalGameplayHudSuppressed(debugWorldOnly);
        inGameGui.render(recorder, textureManager);
        if (const uint64_t loadingRevision =
                inGameGui.loadingRevisionReadyForPresent();
            loadingRevision != 0u &&
            m_gameProjection.gameState == engine::GameState::Loading) {
            drawList.setLoadingPresentationStamp({
                .loadingRevision = loadingRevision,
                .sessionRevision = m_gameProjection.sessionRevision,
            });
        }
        if (!inGameGui.loadingPresentationActive() &&
            m_gameProjection.audioPresentationEpoch != 0u &&
            (m_gameProjection.gameState == engine::GameState::Running ||
             m_gameProjection.gameState == engine::GameState::Paused ||
             m_gameProjection.gameState == engine::GameState::Result)) {
            drawList.setAudioPresentationReleaseStamp({
                .presentationEpoch =
                    m_gameProjection.audioPresentationEpoch,
            });
        }
        const std::optional<gui::GameWndRect> radarPanel =
            inGameGui.layer().tacticalRadarPanel();
        if (radarPanel) {
            drawList.setTacticalRadarPanel({
                .left = radarPanel->left,
                .top = radarPanel->top,
                .width = radarPanel->width,
                .height = radarPanel->height,
            });
        }
        if (radarPanel && m_radarInputPolicyVisible && m_radarInputTerrain) {
            m_radarInputLayout = engine::render::tacticalRadarLayout(
                *m_radarInputTerrain, radarPanel->left, radarPanel->top,
                radarPanel->width, radarPanel->height);
            m_radarInputVisible = m_radarInputLayout.width > 0.0f &&
                m_radarInputLayout.height > 0.0f;
        } else {
            m_radarInputLayout = {};
            m_radarInputVisible = false;
        }
    } else {
        m_radarInputLayout = {};
        m_radarInputVisible = false;
    }
    return drawList;
}

void PresentationCoordinator::Impl::renderRecordedUi(
    engine::TextureManager& textureManager,
    const engine::UiDrawList& uiDrawList) {
    if (const auto& panel = uiDrawList.tacticalRadarPanel()) {
        m_renderer.setTacticalRadarPanel(
            panel->left, panel->top, panel->width, panel->height, true);
    } else {
        m_renderer.setTacticalRadarPanel(0.0f, 0.0f, 0.0f, 0.0f, false);
    }
    m_renderer.prepareWorldPass();
    engine::Renderer::instance().beginFrame();
    m_renderer.renderWorldPass(textureManager);
    engine::UiRenderer{}.submit(engine::Renderer::instance(), uiDrawList);
    engine::Renderer::instance().endFrame();
    m_renderer.publishRenderFeedback();
    if (const auto& loading = uiDrawList.loadingPresentationStamp()) {
        static_cast<void>(m_logicIntents.post(
            runtime::NotifyLoadingScreenPresentedIntent{
                .loadingRevision = loading->loadingRevision,
            }, loading->sessionRevision));
    }
    if (const auto& release =
            uiDrawList.audioPresentationReleaseStamp()) {
        static_cast<void>(m_audio.requestPresentationPlaybackRelease(
            release->presentationEpoch));
    }
}

PresentationCoordinator::PresentationCoordinator(
    RendererSubsystem& renderer, engine::AudioSubsystem& audio,
    runtime::GameLogicIntentMailbox& logicIntents)
    : m_impl(std::make_unique<Impl>(renderer, audio, logicIntents)) {}

PresentationCoordinator::~PresentationCoordinator() = default;

void PresentationCoordinator::configureDebugOptions() {
    m_impl->configureDebugOptions();
}

void PresentationCoordinator::setGameProjection(
    const runtime::GameUiProjection& projection) {
    m_impl->setGameProjection(projection);
}

void PresentationCoordinator::setWaypointMode(bool enabled) noexcept {
    m_impl->setWaypointMode(enabled);
}

void PresentationCoordinator::setWorldInputOcclusionQuery(
    WorldInputOcclusionQuery query) {
    m_impl->setWorldInputOcclusionQuery(std::move(query));
}

void PresentationCoordinator::extractAndSubmit(int frameCount) {
    m_impl->extractAndSubmit(frameCount);
}

void PresentationCoordinator::admitExtractedWorldFrame() {
    m_impl->admitExtractedWorldFrame();
}

void PresentationCoordinator::closeWorldFrameIngress() noexcept {
    m_impl->closeWorldFrameIngress();
}

void PresentationCoordinator::admitRenderAnimationFeedback() {
    m_impl->admitRenderAnimationFeedback();
}

void PresentationCoordinator::admitAudioCompletions() {
    m_impl->admitAudioCompletions();
}

void PresentationCoordinator::admitRenderStartupReadiness() {
    m_impl->admitRenderStartupReadiness();
}

void PresentationCoordinator::applyNonSessionRenderQuality() {
    m_impl->applyNonSessionRenderQuality();
}

engine::UiDrawList PresentationCoordinator::recordUi(
    engine::TextureManager& textureManager, InGameGuiSubsystem& inGameGui,
    bool debugWorldOnly) {
    return m_impl->recordUi(
        textureManager, inGameGui, debugWorldOnly);
}

void PresentationCoordinator::renderRecordedUi(
    engine::TextureManager& textureManager,
    const engine::UiDrawList& uiDrawList) {
    m_impl->renderRecordedUi(textureManager, uiDrawList);
}

void PresentationCoordinator::render(
    engine::TextureManager& textureManager, InGameGuiSubsystem& inGameGui,
    bool debugWorldOnly) {
    m_impl->render(textureManager, inGameGui, debugWorldOnly);
}

std::optional<engine::render::RenderVector>
PresentationCoordinator::radarWorldAt(float screenX, float screenY) const {
    return m_impl->radarWorldAt(screenX, screenY);
}

engine::ObjectId PresentationCoordinator::radarObjectAt(
    float screenX, float screenY) const {
    return m_impl->radarObjectAt(screenX, screenY);
}

std::optional<engine::render::RenderVector>
PresentationCoordinator::terrainWorldAt(float screenX, float screenY) const {
    return m_impl->terrainWorldAt(screenX, screenY);
}

std::optional<math::vec2> PresentationCoordinator::projectWorldToVirtual(
    engine::render::RenderVector world) const {
    return m_impl->projectWorldToVirtual(world);
}

engine::ObjectId PresentationCoordinator::selectableObjectAt(
    float screenX, float screenY) const {
    return m_impl->selectableObjectAt(screenX, screenY);
}

std::optional<PresentedOrderWaypointTarget>
PresentationCoordinator::orderWaypointAt(
    float screenX, float screenY) const {
    return m_impl->orderWaypointAt(screenX, screenY);
}

engine::ObjectId PresentationCoordinator::targetableObjectAt(
    float screenX, float screenY, bool allowShrubbery,
    bool allowMines, bool forceAttack) const {
    return m_impl->targetableObjectAt(
        screenX, screenY, allowShrubbery, allowMines, forceAttack);
}

PresentedWorldInputTarget PresentationCoordinator::Impl::worldInputTargetAt(
    float screenX, float screenY, bool allowShrubbery,
    bool allowMines, bool forceAttack) const {
    if (const std::optional<engine::render::RenderVector> radar =
            radarWorldAt(screenX, screenY)) {
        return {
            .surface = PresentedWorldInputSurface::Radar,
            .position = radar,
            .object = radarObjectAt(screenX, screenY),
        };
    }
    const std::optional<engine::render::RenderVector> terrain =
        terrainWorldAt(screenX, screenY);
    if (!terrain) return {};
    return {
        .surface = PresentedWorldInputSurface::Terrain,
        .position = terrain,
        .object = targetableObjectAt(
            screenX, screenY, allowShrubbery, allowMines, forceAttack),
    };
}

PresentedWorldInputTarget PresentationCoordinator::worldInputTargetAt(
    float screenX, float screenY, bool allowShrubbery,
    bool allowMines, bool forceAttack) const {
    return m_impl->worldInputTargetAt(
        screenX, screenY, allowShrubbery, allowMines, forceAttack);
}

std::optional<engine::render::RenderVector>
PresentationCoordinator::objectWorldPosition(
    engine::ObjectId object) const {
    return m_impl->objectWorldPosition(object);
}

std::optional<engine::GameCameraState>
PresentationCoordinator::presentedCamera() const {
    return m_impl->presentedCamera();
}

container::Vector<engine::ObjectId>
PresentationCoordinator::selectableObjectsInRectangle(
    float startX, float startY, float endX, float endY) const {
    return m_impl->selectableObjectsInRectangle(
        startX, startY, endX, endY);
}

bool PresentationCoordinator::hasRadarInput() const noexcept {
    return m_impl->hasRadarInput();
}

std::optional<engine::render::RenderVector>
PresentationCoordinator::lastRadarEventWorld() const {
    return m_impl->lastRadarEventWorld();
}

bool PresentationCoordinator::worldOnlyPresentation() const noexcept {
    return m_impl->worldOnlyPresentation();
}

} // namespace app
