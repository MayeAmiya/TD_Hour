#include "LocalPlacementPreviewPresentation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine {
namespace {

struct PlacementCullingBounds final {
    render::RenderVector centerOffset{};
    float radius = 0.0f;
};

[[nodiscard]] PlacementCullingBounds placementCullingBounds(
    const game::ObjectGeometryTemplate& geometry,
    float authoredScale) noexcept {
    const auto finiteNonnegative = [](float value) noexcept {
        return std::isfinite(value) ? std::max(0.0f, value) : 0.0f;
    };
    const float scale = finiteNonnegative(authoredScale);
    const float major = finiteNonnegative(
        geometry.majorRadiusFixed.to_float()) * scale;
    const float minor = finiteNonnegative(
        geometry.minorRadiusFixed.to_float()) * scale;
    const float height = finiteNonnegative(
        geometry.heightFixed.to_float()) * scale;
    const float authoredSphere = finiteNonnegative(
        geometry.boundingSphereRadiusFixed.to_float()) * scale;

    PlacementCullingBounds result;
    switch (geometry.type) {
    case game::ObjectGeometryType::Sphere:
        result.radius = std::max(major, authoredSphere);
        break;
    case game::ObjectGeometryType::Cylinder: {
        const float halfHeight = height * 0.5f;
        result.centerOffset = {0.0f, 0.0f, halfHeight};
        result.radius = std::max(
            std::hypot(major, halfHeight), authoredSphere);
        break;
    }
    case game::ObjectGeometryType::Box: {
        const float halfHeight = height * 0.5f;
        result.centerOffset = {0.0f, 0.0f, halfHeight};
        result.radius = std::max(
            std::hypot(std::hypot(major, minor), halfHeight),
            authoredSphere);
        break;
    }
    }
    return result;
}

[[nodiscard]] render::RenderAnimationMode toRenderAnimationMode(
    game::ModelAnimationMode mode) noexcept {
    switch (mode) {
    case game::ModelAnimationMode::Manual:
        return render::RenderAnimationMode::Manual;
    case game::ModelAnimationMode::Loop:
        return render::RenderAnimationMode::Loop;
    case game::ModelAnimationMode::Once:
        return render::RenderAnimationMode::Once;
    case game::ModelAnimationMode::LoopPingPong:
        return render::RenderAnimationMode::LoopPingPong;
    case game::ModelAnimationMode::LoopBackwards:
        return render::RenderAnimationMode::LoopBackwards;
    case game::ModelAnimationMode::OnceBackwards:
        return render::RenderAnimationMode::OnceBackwards;
    }
    return render::RenderAnimationMode::Once;
}

[[nodiscard]] render::RenderEntityId previewInstanceIdentity(
    render::RenderEntityId previewIdentity, size_t channelIndex) noexcept {
    // 11 marks the local-placement domain; client terrain uses 10. The state
    // allocator owns the low 48-bit ordinal and the remaining 14 bits retain
    // the authored Draw-channel identity without colliding with live objects.
    constexpr uint64_t kDomain = 0xc000000000000000ull;
    constexpr uint64_t kOrdinalMask = 0x0000ffffffffffffull;
    constexpr size_t kMaximumChannel = 0x3fffu;
    if (channelIndex > kMaximumChannel) return 0;
    return kDomain |
        (static_cast<uint64_t>(channelIndex) << 48u) |
        (previewIdentity & kOrdinalMask);
}

[[nodiscard]] game::ModelConditionMask placementModelConditions(
    const RenderGameDataSettings& settings,
    uint8_t terrainTimeOfDay) noexcept {
    game::ModelConditionMask conditions;
    bool night = settings.visual.defaultTimeOfDay == RenderTimeOfDay::Night;
    if (terrainTimeOfDay != 0) night = terrainTimeOfDay == 4;
    if (settings.visual.forceModelsToFollowTimeOfDay && night) {
        conditions.set(game::ModelConditionFlag::Night);
    }
    if (settings.visual.forceModelsToFollowWeather &&
        settings.visual.defaultWeather == RenderWeather::Snowy) {
        conditions.set(game::ModelConditionFlag::Snow);
    }
    return conditions;
}

[[nodiscard]] render::RenderVector sanitizedColor(
    render::RenderVector value) noexcept {
    if (!std::isfinite(value.x()) || !std::isfinite(value.y()) ||
        !std::isfinite(value.z())) {
        return {1.0f, 1.0f, 1.0f};
    }
    return {
        std::clamp(value.x(), 0.0f, 1.0f),
        std::clamp(value.y(), 0.0f, 1.0f),
        std::clamp(value.z(), 0.0f, 1.0f),
    };
}

} // namespace

void appendLocalPlacementPreviewEntities(
    const selection::LocalPlacementPreviewSnapshot& placement,
    const game::ThingTemplate& product,
    render::RenderVector authorPlayerColor,
    const RenderGameDataSettings& settings,
    const RenderFeatureQualitySettings& featureQuality,
    container::Vector<render::RenderEntitySnapshot>& output,
    uint8_t terrainTimeOfDay) {
    if (!placement.previewIdentity || !placement.hasPose ||
        placement.objectType.empty() || product.name.empty() ||
        !std::isfinite(placement.position.x()) ||
        !std::isfinite(placement.position.y()) ||
        !std::isfinite(placement.position.z()) ||
        !std::isfinite(placement.yawRadians)) {
        return;
    }

    const game::ModelConditionMask conditions =
        placementModelConditions(settings, terrainTimeOfDay);
    const float authoredScale = std::isfinite(product.assetScale.to_float())
        ? std::max(0.0f, product.assetScale.to_float()) : 1.0f;
    const float configuredOpacity =
        settings.visual.objectFeedback.objectPlacementOpacity;
    const float opacity = std::isfinite(configuredOpacity)
        ? std::clamp(configuredOpacity, 0.0f, 1.0f) : 0.45f;
    const render::RenderVector playerColor =
        sanitizedColor(authorPlayerColor);
    const PlacementCullingBounds culling =
        placementCullingBounds(product.geometry, authoredScale);
    const size_t channelCount = product.drawVisualChannels.empty()
        ? size_t{1} : product.drawVisualChannels.size();
    output.reserve(output.size() + channelCount);

    for (size_t channelIndex = 0; channelIndex < channelCount;
         ++channelIndex) {
        const game::ModelDrawVisualChannel* channel =
            channelIndex < product.drawVisualChannels.size()
            ? &product.drawVisualChannels[channelIndex] : nullptr;
        if (channel && !allowsDrawModuleAtStaticLod(
                featureQuality.staticLod,
                static_cast<uint8_t>(channel->minimumLod),
                featureQuality.useDrawModuleLod)) {
            continue;
        }
        // A dependency model attached to a containing Object has no valid
        // container in a local, non-authoritative placement preview.
        if (channel && !channel->attachToBoneInContainer.empty()) continue;

        const container::Vector<game::ModelConditionVisualRule>* rules =
            channel ? &channel->conditionVisuals
                    : &product.modelConditionVisuals;
        size_t selectedRule = channel
            ? game::selectModelConditionVisualRuleIndex(*channel, conditions)
            : game::selectModelConditionVisualRuleIndex(product, conditions);
        const game::ModelConditionVisualRule* rule =
            selectedRule < rules->size() ? &(*rules)[selectedRule] : nullptr;
        // Model=None is an explicit, drawable-but-not-visible state. Falling
        // back to the channel default here incorrectly exposes doors,
        // scaffolds and construction accessories on placement icons.
        container::String model = rule
            ? rule->model
            : channel ? channel->defaultModel : product.defaultW3dModel;
        if (model.empty()) continue;

        render::RenderEntitySnapshot preview;
        preview.id = previewInstanceIdentity(
            placement.previewIdentity, channelIndex);
        if (!preview.id) continue;
        preview.objectId = placement.previewIdentity;
        preview.channelIndex = static_cast<uint32_t>(channelIndex);
        preview.modelAsset = std::move(model);
        preview.transform.position = placement.position;
        preview.transform.orientation = math::quat::from_axis_angle(
            {0.0f, 0.0f, 1.0f}, placement.yawRadians);
        preview.transform.scale = {
            authoredScale, authoredScale, authoredScale};
        preview.boundingRadius = culling.radius;
        preview.cullingCenterOffset = culling.centerOffset;
        // This is renderer-local cursor state. RefCode updates it directly in
        // InGameUI::handleBuildPlacements; blending it as a simulation object
        // makes the mesh lag behind its terrain bib and mouse position.
        preview.interpolationDisabled = true;
        preview.visual.modelConditionFlags = conditions.words;
        preview.visual.objectOpacity = opacity;
        preview.visual.animationSampleTick = placement.animationStartTick;
        preview.visual.animationStateEnterTick = placement.animationStartTick;
        preview.visual.receivesDynamicLights =
            !channel || channel->receivesDynamicLights;
        preview.animationCompletionFeedbackEnabled = false;
        // Unlike a container dependency, this bone belongs to a sibling Draw
        // channel of the same preview identity and is published in this very
        // batch, so the cursor model composes the same way the live object does.
        if (channel && !channel->attachToBoneInAnotherModule.empty()) {
            preview.attachToBoneInAnotherModule =
                channel->attachToBoneInAnotherModule;
        }
        if (placement.legality ==
                selection::LocalPlacementLegality::Illegal ||
            placement.feedback ==
                selection::LocalPlacementPreviewFeedback::Rejected) {
            // Drawable::colorTint(IllegalBuildColor), where the original
            // constant is exactly {1, 0, 0}.
            preview.visual.scriptFlashTint = {1.0f, 0.0f, 0.0f};
        } else if (placement.feedback ==
                   selection::LocalPlacementPreviewFeedback::Queued) {
            preview.visual.scriptFlashTint = {1.0f, 0.78f, 0.08f};
        }

        if (rule) {
            if (rule->allowsModelColorChange) {
                preview.visual.hasScriptIndicatorColor = true;
                preview.visual.scriptIndicatorColor = playerColor;
            }
            const game::ModelAnimationSelection animation =
                game::selectModelAnimation(
                    *rule, preview.id, conditions, 0);
            preview.visual.animationState =
                animation.candidateIndex < rule->animationCandidates.size()
                ? rule->animationCandidates[animation.candidateIndex].resource
                : rule->animation;
            preview.visual.animationRate = std::max(
                0.0f, animation.speedFactor);
            preview.visual.animationMode =
                toRenderAnimationMode(rule->animationMode);
            preview.visual.subObjectVisibility.reserve(
                rule->subObjectVisibility.size());
            for (const game::ModelSubObjectVisibility& visibility :
                 rule->subObjectVisibility) {
                preview.visual.subObjectVisibility.push_back({
                    .name = visibility.name,
                    .visible = visibility.visible,
                });
            }
            // DRAWABLE_STATUS_NO_STATE_PARTICLES is intentional: do not copy
            // rule->particleSystemBones into this local cursor snapshot.
        }

        if (settings.visual.objectFeedback.objectPlacementShadows) {
            preview.shadow = {
                .typeMask = render::filterRenderShadowTypeMask(
                    product.shadow.typeMask,
                    featureQuality.useShadowVolumes,
                    featureQuality.useShadowDecals),
                .textureName = product.shadow.texture,
                .sizeX = product.shadow.sizeX,
                .sizeY = product.shadow.sizeY,
                .offsetX = product.shadow.offsetX,
                .offsetY = product.shadow.offsetY,
            };
        }
        output.push_back(std::move(preview));
    }
}

} // namespace engine
