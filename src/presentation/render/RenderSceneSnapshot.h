#pragma once

#include "presentation/render/RenderFxSnapshot.h"
#include "presentation/render/RenderOverlaySnapshot.h"
#include "presentation/render/RenderViewSnapshot.h"
#include "presentation/render/RenderWorldHotSnapshot.h"
#include "presentation/render/TerrainRenderSnapshot.h"

namespace engine::render {

struct WorldRenderSnapshot {
    uint64_t simulationFrame = 0;
    uint64_t presentationEpoch = 0;
    uint64_t sessionRevision = 0;
    uint64_t loadingRevision = 0;
    // Debug-only selector value. Zero is disabled; production behavior never
    // branches on it and Release emits no visual trace logging.
    uint64_t debugVisualTraceObjectId = 0;
    // One immutable handle is created when GameSession starts and forwarded
    // unchanged through every frame. A07 camera/input policy is consumed by
    // the logic-side controller before extraction. Renderer-side camera,
    // terrain/water/shadow/device and object-feedback consumers read only this
    // frozen descriptor. A12 particle admission travels on the separate
    // lossless FX stream but receives operational values from this same
    // session snapshot at the composition root. No consumer may query
    // GameDataLoader, Options.ini, GlobalData, or a live GameSession. The
    // shared handle avoids copying VertexWater map strings every frame.
    container::SharedPtr<const ::engine::RenderGameDataSettings>
        renderGameDataSettings;
    // Feature quality is frozen when GameSession starts. Display quality is
    // deliberately absent: it is renderer-local and may publish a new safe-
    // frame revision without changing simulation/extraction state.
    container::SharedPtr<const ::engine::ResolvedRenderFeatureSnapshot>
        renderFeatureQuality;
    RenderCameraSnapshot camera;
    ScreenFadeRenderState screenFade;
    BlackAndWhiteRenderState blackAndWhite;
    MotionBlurRenderState motionBlur;
    CameraSlaveRenderState cameraSlave;
    SkyboxRenderState skybox;
    TreeSwayRenderState treeSway;
    WeatherRenderState weather;
    ScreenShakeRenderState screenShake;
    ClientOptionsRenderState clientOptions;
    ObjectIconRenderState objectIcons;
    WorldFeedbackRenderState worldFeedback;
    ObjectUiRenderState objectUi;
    TacticalRadarRenderState tacticalRadar;
    SharedSnapshotVector<TerrainBibRenderData> terrainBibs;
    ViewCompatibilityRenderState viewCompatibility;
    LocalVisibilityRenderSnapshot localVisibility;
    container::SharedPtr<const TerrainRenderSnapshot> terrain;
    SharedSnapshotVector<RenderModelPhaseDependency> visualAssetDependencies;
    SharedSnapshotVector<RenderAnimationEndpointAdmission>
        animationEndpointAdmissions;
    SharedSnapshotVector<RenderEntitySnapshot> entities;
    SharedSnapshotVector<ProjectileRenderSnapshot> projectiles;
    SharedSnapshotVector<TrackMarkRenderInput> trackMarks;

    void sealSharedColumns() {
        objectIcons.icons.seal();
        worldFeedback.animations.seal();
        worldFeedback.floatingTexts.seal();
        objectUi.objects.seal();
        objectUi.waypoints.seal();
        objectUi.waypointSegments.seal();
        tacticalRadar.events.seal();
        terrainBibs.seal();
        visualAssetDependencies.seal();
        animationEndpointAdmissions.seal();
        entities.seal();
        projectiles.seal();
        trackMarks.seal();
    }
};

} // namespace engine::render
