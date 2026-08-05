#include "game/session/integration/GameRenderExtractionGarrisonPresentation.h"

#include "game/content/runtime/GameContentSnapshot.h"
#include "core/container/string_utils.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/session/integration/GameRenderExtractionDetail.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>

namespace engine {
namespace {

inline constexpr uint32_t kGarrisonPresentationChannelBase = 0x70000000u;

[[nodiscard]] uint64_t saturatingAdd(
    uint64_t value, uint64_t increment) noexcept {
    return increment > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max()
        : value + increment;
}

[[nodiscard]] uint64_t muzzleFlashStart(
    const ObjectGarrisonFirePointAssignment& assignment) noexcept {
    return assignment.lastEffectFireSequence == 0
        ? 0 : saturatingAdd(assignment.lastEffectFireTick, 1u);
}

[[nodiscard]] uint64_t muzzleFlashEnd(
    const ObjectGarrisonFirePointAssignment& assignment,
    uint32_t logicFramesPerSecond) noexcept {
    const uint64_t start = muzzleFlashStart(assignment);
    if (start == 0) return 0;
    // RefCode: MUZZLE_FLASH_LIFETIME = LOGICFRAMES_PER_SECOND / 7 and the
    // condition is cleared only when elapsed > lifetime.
    const uint64_t lifetime = std::max<uint32_t>(
        1u, std::max<uint32_t>(1u, logicFramesPerSecond) / 7u);
    return saturatingAdd(start, lifetime);
}

[[nodiscard]] bool crosses(
    uint64_t cachedFrame, uint64_t simulationFrame,
    uint64_t boundary) noexcept {
    return boundary != 0 && cachedFrame < boundary &&
        simulationFrame >= boundary;
}

[[nodiscard]] render::RenderQuaternion aimOrientation(
    const LogicFixedVec3& source,
    const LogicFixedVec3& target) noexcept {
    const math::q32_32 yaw = math::fixed_atan2(
        target.y - source.y, target.x - source.x);
    const math::q32_32_sincos half = math::fixed_sincos(
        yaw / math::q32_32{int32_t{2}});
    return {
        0.0f,
        0.0f,
        half.sine.to_float(),
        half.cosine.to_float(),
    };
}

} // namespace

bool hasDueGarrisonPresentationBoundary(
    const ecs::registry& registry,
    ecs::entity hostEntity,
    uint64_t cachedFrame,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond) noexcept {
    const ObjectGarrisonFirePointComponent* points =
        ecs::try_get<ObjectGarrisonFirePointComponent>(
            registry, hostEntity);
    if (!points) return false;
    for (const ObjectGarrisonFirePointAssignment& assignment :
         points->assignments) {
        if (assignment.suppressMuzzleFlash) continue;
        const uint64_t start = muzzleFlashStart(assignment);
        const uint64_t end = muzzleFlashEnd(
            assignment, logicFramesPerSecond);
        if (crosses(cachedFrame, simulationFrame, start) ||
            (end != std::numeric_limits<uint64_t>::max() &&
             crosses(cachedFrame, simulationFrame, end + 1u))) {
            return true;
        }
    }
    return false;
}

void appendGarrisonGunPresentation(
    const ecs::registry& registry,
    const GameContentSnapshot& content,
    ecs::entity hostEntity,
    ObjectId host,
    render::LocalVisibilityRenderCellState visibilityState,
    bool hiddenByLocalVisibility,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond,
    render::WorldRenderSnapshot& snapshot) {
    const ObjectGarrisonFirePointComponent* points =
        ecs::try_get<ObjectGarrisonFirePointComponent>(
            registry, hostEntity);
    if (!host || !points || points->assignments.empty()) return;

    const auto archetype = content.findObjectArchetype("GarrisonGun");
    if (!archetype) return;
    const game::ThingTemplate& recipe = archetype->templateData;
    if (recipe.drawVisualChannels.empty()) return;

    const uint32_t framesPerSecond = std::max<uint32_t>(
        1u, logicFramesPerSecond);
    for (const ObjectGarrisonFirePointAssignment& assignment :
         points->assignments) {
        const uint64_t flashStart = muzzleFlashStart(assignment);
        const uint64_t flashEnd = muzzleFlashEnd(
            assignment, framesPerSecond);
        const bool firing = !assignment.suppressMuzzleFlash &&
            flashStart != 0 && simulationFrame >= flashStart &&
            simulationFrame <= flashEnd;
        const game::ModelConditionMask conditions = firing
            ? game::modelConditionMaskOf(
                  game::ModelConditionFlag::FiringA)
            : game::ModelConditionMask{};

        for (size_t templateChannel = 0;
             templateChannel < recipe.drawVisualChannels.size();
             ++templateChannel) {
            const game::ModelDrawVisualChannel& channel =
                recipe.drawVisualChannels[templateChannel];
            const size_t ruleIndex =
                game::selectModelConditionVisualRuleIndex(
                    channel, conditions);
            if (ruleIndex >= channel.conditionVisuals.size()) continue;
            const game::ModelConditionVisualRule& rule =
                channel.conditionVisuals[ruleIndex];
            if (rule.model.empty() ||
                container::asciiEqualIgnoreCase(rule.model, "None")) {
                continue;
            }

            const uint64_t channelOrdinal =
                static_cast<uint64_t>(assignment.pointIndex) *
                    recipe.drawVisualChannels.size() +
                templateChannel;
            if (channelOrdinal >
                static_cast<uint64_t>(0x7fffffffu -
                    kGarrisonPresentationChannelBase)) {
                continue;
            }
            const uint32_t channelIndex =
                kGarrisonPresentationChannelBase +
                static_cast<uint32_t>(channelOrdinal);
            render::RenderEntitySnapshot output;
            output.id = render_extraction_detail::renderInstanceId(
                host.value, channelIndex);
            output.objectId = host.value;
            output.channelIndex = channelIndex;
            output.modelAsset = rule.model;
            output.transform.position = {
                assignment.pointPosition.x.to_float(),
                assignment.pointPosition.y.to_float(),
                assignment.pointPosition.z.to_float(),
            };
            output.transform.orientation = aimOrientation(
                assignment.pointPosition, assignment.targetPosition);
            const float scale = recipe.assetScale.to_float();
            output.transform.scale = {scale, scale, scale};
            output.boundingRadius = 4.0f * std::max(1.0f, scale);
            output.visual.modelConditionFlags = conditions.words;
            const game::ModelAnimationSelection animation =
                game::selectModelAnimation(
                    rule, output.id, conditions,
                    assignment.lastEffectFireSequence);
            if (animation.candidateIndex <
                rule.animationCandidates.size()) {
                output.visual.animationState =
                    rule.animationCandidates[
                        animation.candidateIndex].resource;
                output.visual.animationRate = animation.speedFactor;
            } else {
                output.visual.animationState = rule.animation;
            }
            output.visual.animationMode =
                render_extraction_detail::toRenderAnimationMode(
                    rule.animationMode);
            output.visual.animationTimeSeconds = firing
                ? static_cast<float>(simulationFrame - flashStart) /
                      static_cast<float>(framesPerSecond)
                : 0.0f;
            output.visual.animationStateEnterTick = firing
                ? flashStart : 0;
            output.visual.animationSampleTick = simulationFrame;
            output.localVisibilityState = visibilityState;
            output.hiddenByLocalVisibility = hiddenByLocalVisibility;
            output.visual.receivesLocalVisibility =
                visibilityState !=
                render::LocalVisibilityRenderCellState::Visible;
            output.localVisibilityMemoryPolicy = visibilityState ==
                    render::LocalVisibilityRenderCellState::Shrouded
                ? render::RenderLocalVisibilityMemoryPolicy::HardHidden
                : render::RenderLocalVisibilityMemoryPolicy::StaticGhost;
            output.animationCompletionFeedbackEnabled = false;
            output.interpolationDisabled =
                simulationFrame == flashStart;
            snapshot.entities.push_back(std::move(output));
        }
    }
}

} // namespace engine
