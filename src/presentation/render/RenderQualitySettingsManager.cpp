#include "RenderQualitySettingsManager.h"

#include <memory>

namespace engine {
namespace {

bool sameFeatureSettings(
    const RenderFeatureQualitySettings& left,
    const RenderFeatureQualitySettings& right) noexcept {
#define SAME_FEATURE(field) left.field == right.field
    return SAME_FEATURE(staticLod) &&
        SAME_FEATURE(dynamicLod) &&
        SAME_FEATURE(terrainLod) &&
        SAME_FEATURE(maximumParticles) &&
        SAME_FEATURE(textureReductionFactor) &&
        SAME_FEATURE(useShadowVolumes) &&
        SAME_FEATURE(useShadowDecals) &&
        SAME_FEATURE(useCloudMap) &&
        SAME_FEATURE(useLightMap) &&
        SAME_FEATURE(showSoftWaterEdge) &&
        SAME_FEATURE(showTrees) &&
        SAME_FEATURE(useTreeSway) &&
        SAME_FEATURE(useBuildupScaffolds) &&
        SAME_FEATURE(extraAnimations) &&
        SAME_FEATURE(useDrawModuleLod) &&
        SAME_FEATURE(useHeatEffects) &&
        SAME_FEATURE(behindBuildingMarkers) &&
        SAME_FEATURE(dynamicLodEnabled) &&
        SAME_FEATURE(particleSimulationBackend);
#undef SAME_FEATURE
}

bool sameDisplaySettings(
    const RenderDisplaySettings& left,
    const RenderDisplaySettings& right) noexcept {
#define SAME_DISPLAY(field) left.field == right.field
    return SAME_DISPLAY(width) &&
        SAME_DISPLAY(height) &&
        SAME_DISPLAY(refreshRateHz) &&
        SAME_DISPLAY(textureFilter) &&
        SAME_DISPLAY(anisotropyLevel) &&
        SAME_DISPLAY(gamma) &&
        SAME_DISPLAY(displayGamma) &&
        SAME_DISPLAY(legacyAntiAliasing) &&
        SAME_DISPLAY(displayMode) &&
        SAME_DISPLAY(antiAliasingMode) &&
        SAME_DISPLAY(fxaaSubpixel) &&
        SAME_DISPLAY(fxaaEdgeThreshold) &&
        SAME_DISPLAY(fxaaEdgeThresholdMin) &&
        SAME_DISPLAY(verticalSync) &&
        SAME_DISPLAY(fpsLimitEnabled);
#undef SAME_DISPLAY
}

void mergeDisplayOverrides(
    RenderDisplayOverrides& destination,
    const RenderDisplayOverrides& patch) {
#define MERGE_DISPLAY(field) \
    if (patch.field) destination.field = patch.field
    MERGE_DISPLAY(width);
    MERGE_DISPLAY(height);
    MERGE_DISPLAY(refreshRateHz);
    MERGE_DISPLAY(textureFilter);
    MERGE_DISPLAY(anisotropyLevel);
    MERGE_DISPLAY(gamma);
    MERGE_DISPLAY(displayGamma);
    MERGE_DISPLAY(legacyAntiAliasing);
    MERGE_DISPLAY(displayMode);
    MERGE_DISPLAY(antiAliasingMode);
    MERGE_DISPLAY(fxaaSubpixel);
    MERGE_DISPLAY(fxaaEdgeThreshold);
    MERGE_DISPLAY(fxaaEdgeThresholdMin);
    MERGE_DISPLAY(verticalSync);
    MERGE_DISPLAY(fpsLimitEnabled);
#undef MERGE_DISPLAY
}

} // namespace

RenderQualitySettingsManager::RenderQualitySettingsManager() {
    publish();
}

void RenderQualitySettingsManager::configure(
    const RenderFeaturePresetSet& featurePresets,
    const RenderDisplayPresetSet& displayPresets,
    const RenderFeatureQualitySettings& featureBase,
    const RenderDisplaySettings& displayBase,
    const RenderFeatureQualityOverrides& featureOverrides,
    const RenderDisplayOverrides& displayOverrides) {
    m_featurePresets = featurePresets;
    m_displayPresets = displayPresets;
    m_featureBase = featureBase;
    m_displayBase = displayBase;
    m_preferenceFeatureOverrides = featureOverrides;
    m_preferenceDisplayOverrides = displayOverrides;
    publish();
}

void RenderQualitySettingsManager::setSources(
    const RenderFeaturePresetSet& featurePresets,
    const RenderDisplayPresetSet& displayPresets,
    const RenderFeatureQualitySettings& featureBase,
    const RenderDisplaySettings& displayBase) {
    m_featurePresets = featurePresets;
    m_displayPresets = displayPresets;
    m_featureBase = featureBase;
    m_displayBase = displayBase;
    publish();
}

void RenderQualitySettingsManager::setOverrides(
    const RenderFeatureQualityOverrides& featureOverrides,
    const RenderDisplayOverrides& displayOverrides) {
    setOverrides(
        RenderQualityOverrideLayer::Host,
        featureOverrides, displayOverrides);
}

void RenderQualitySettingsManager::setOverrides(
    RenderQualityOverrideLayer layer,
    const RenderFeatureQualityOverrides& featureOverrides,
    const RenderDisplayOverrides& displayOverrides) {
    if (layer == RenderQualityOverrideLayer::Runtime) {
        m_runtimeFeatureOverrides = featureOverrides;
        m_runtimeDisplayOverrides = displayOverrides;
    } else {
        m_hostFeatureOverrides = featureOverrides;
        m_hostDisplayOverrides = displayOverrides;
    }
    publish();
}

void RenderQualitySettingsManager::clearOverrides(
    RenderQualityOverrideLayer layer) {
    setOverrides(layer, {}, {});
}

void RenderQualitySettingsManager::patchDisplayOverrides(
    RenderQualityOverrideLayer layer,
    const RenderDisplayOverrides& displayOverrides) {
    RenderDisplayOverrides& destination =
        layer == RenderQualityOverrideLayer::Runtime
        ? m_runtimeDisplayOverrides
        : m_hostDisplayOverrides;
    mergeDisplayOverrides(destination, displayOverrides);
    publish();
}

void RenderQualitySettingsManager::setCapabilities(
    const RenderDisplayCapabilities& capabilities) {
    m_capabilities = capabilities;
    publish();
}

void RenderQualitySettingsManager::publish() {
    const auto previousSnapshot = snapshot();
    RenderFeatureQualitySettings featureBase = m_featureBase;
    RenderDisplaySettings displayBase = m_displayBase;

    // Legacy StaticGameLOD remains parseable, but it no longer selects a
    // degrading runtime profile.  Always start from the full-detail profile;
    // sparse non-LOD feature/display preferences are still resolved below.
    constexpr size_t fixedProfile = static_cast<size_t>(
        render_lod_policy::kStaticProfile);
    static_assert(fixedProfile < static_cast<size_t>(RenderStaticLod::Count));
    featureBase = m_featurePresets.profiles[fixedProfile];

    const uint64_t nextRevision = previousSnapshot
        ? previousSnapshot->revision + 1u : 1u;
    const RenderDisplaySettings* previousDisplay = previousSnapshot
        ? &previousSnapshot->display.effective : nullptr;
    ResolvedRenderFeatureSnapshot feature = resolveRenderFeatureQuality(
        featureBase, m_preferenceFeatureOverrides, nextRevision);
    feature = resolveRenderFeatureQuality(
        feature.requested, m_hostFeatureOverrides, nextRevision);
    feature = resolveRenderFeatureQuality(
        feature.requested, m_runtimeFeatureOverrides, nextRevision);
    const ResolvedRenderDisplaySnapshot preferenceDisplay =
        resolveRenderDisplaySettings(
            displayBase, m_preferenceDisplayOverrides, m_capabilities,
            nullptr, nextRevision);
    const ResolvedRenderDisplaySnapshot hostDisplay =
        resolveRenderDisplaySettings(
            preferenceDisplay.requested, m_hostDisplayOverrides,
            m_capabilities, nullptr, nextRevision);
    ResolvedRenderDisplaySnapshot display = resolveRenderDisplaySettings(
        hostDisplay.requested, m_runtimeDisplayOverrides,
        m_capabilities, previousDisplay, nextRevision);

    if (previousSnapshot &&
        sameFeatureSettings(previousSnapshot->feature.requested,
                            feature.requested) &&
        sameDisplaySettings(previousSnapshot->display.requested,
                            display.requested) &&
        sameDisplaySettings(previousSnapshot->display.effective,
                            display.effective) &&
        previousSnapshot->display.fallbackMask == display.fallbackMask) {
        return;
    }

    m_snapshot.store(
        std::make_shared<const RenderQualitySettingsSnapshot>(
            RenderQualitySettingsSnapshot{
            .feature = feature,
            .display = display,
            .revision = nextRevision,
            }),
        std::memory_order_release);
}

} // namespace engine
