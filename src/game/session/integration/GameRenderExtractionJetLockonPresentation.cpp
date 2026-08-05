#include "game/session/integration/GameRenderExtractionJetLockonPresentation.h"

#include "core/container/string_utils.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/session/integration/GameRenderExtractionDetail.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine {
namespace {

inline constexpr uint32_t kJetLockonPresentationChannelBase = 0x71000000u;

[[nodiscard]] uint64_t lockonDurationTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0 || framesPerSecond == 0) return 0;
    const uint64_t scaled = static_cast<uint64_t>(milliseconds) *
        static_cast<uint64_t>(framesPerSecond);
    return std::max<uint64_t>(1u, (scaled + 999u) / 1000u);
}

[[nodiscard]] render::RenderQuaternion facingJet(
    math::q32_32 angle) noexcept {
    const math::q32_32 yaw = angle +
        math::q32_32::from_raw(13'493'037'705ll);
    const math::q32_32_sincos half = math::fixed_sincos(
        yaw / math::q32_32{int32_t{2}});
    return {0.0f, 0.0f, half.sine.to_float(), half.cosine.to_float()};
}

[[nodiscard]] bool blinkVisible(
    const game::ObjectJetAiRule& rule,
    uint64_t elapsed, uint64_t duration) noexcept {
    if (!rule.lockonBlinky) return true;
    if (duration == 0) return false;
    const double value = static_cast<double>(elapsed);
    const double previousSum = 0.5 * (value - 1.0) * value;
    const double currentSum = previousSum + value;
    const double factor = static_cast<double>(
        rule.lockonFrequencyFixed.to_float()) /
        static_cast<double>(duration);
    const bool previousPhase =
        (static_cast<int64_t>(factor * previousSum) & 1ll) != 0;
    const bool currentPhase =
        (static_cast<int64_t>(factor * currentSum) & 1ll) != 0;
    return previousPhase && !currentPhase;
}

} // namespace

bool hasActiveJetLockonPresentation(
    const ecs::registry& registry,
    ecs::entity jetEntity,
    uint64_t cachedFrame,
    uint64_t simulationFrame) noexcept {
    const ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, jetEntity);
    if (!component || !component->plan ||
        simulationFrame <= cachedFrame) return false;
    const size_t count = std::min(component->jetAi.size(),
                                  component->plan->jetAi.size());
    for (size_t index = 0; index < count; ++index) {
        const ObjectJetAiRuntime& runtime = component->jetAi[index];
        const game::ObjectJetAiRule& rule = component->plan->jetAi[index];
        if (!runtime.lockonTargeters.empty() &&
            runtime.lockonReadyTick != 0 &&
            simulationFrame < runtime.lockonReadyTick &&
            !rule.lockonCursor.empty())
            return true;
    }
    return false;
}

void appendJetLockonPresentation(
    const ecs::registry& registry,
    const GameContentSnapshot& content,
    ecs::entity jetEntity,
    render::LocalVisibilityRenderCellState visibilityState,
    bool hiddenByLocalVisibility,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond,
    render::WorldRenderSnapshot& snapshot) {
    const ObjectIdentityComponent* identity =
        ecs::try_get<ObjectIdentityComponent>(registry, jetEntity);
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, jetEntity);
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, jetEntity);
    const ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, jetEntity);
    if (!identity || !identity->id || !transform || !component ||
        !component->plan) return;

    const LogicFixedVec3 jetPosition = readAuthoritativeObjectPosition(
        registry, jetEntity, *transform);
    const size_t count = std::min(component->jetAi.size(),
                                  component->plan->jetAi.size());
    for (size_t moduleIndex = 0; moduleIndex < count; ++moduleIndex) {
        const ObjectJetAiRuntime& runtime = component->jetAi[moduleIndex];
        const game::ObjectJetAiRule& rule =
            component->plan->jetAi[moduleIndex];
        if (runtime.lockonTargeters.empty() ||
            runtime.lockonReadyTick == 0 ||
            simulationFrame >= runtime.lockonReadyTick ||
            rule.lockonCursor.empty()) continue;
        const uint64_t duration = lockonDurationTicks(
            rule.lockonMilliseconds,
            std::max<uint32_t>(1u, logicFramesPerSecond));
        if (duration == 0) continue;
        const uint64_t remaining = runtime.lockonReadyTick - simulationFrame;
        const uint64_t elapsed = duration > remaining
            ? duration - remaining : 0;
        if (!blinkVisible(rule, elapsed, duration)) continue;

        const auto archetype = content.findObjectArchetype(rule.lockonCursor);
        if (!archetype) continue;
        const game::ThingTemplate& recipe = archetype->templateData;
        const math::q32_32 fraction = math::q32_32::from_fraction(
            static_cast<int64_t>(std::min(remaining, duration)),
            static_cast<int64_t>(duration));
        const math::q32_32 finalDistance = geometry
            ? math::q32_32::max(
                  math::q32_32{}, geometry->boundingCircleRadiusFixed)
            : math::q32_32{};
        const math::q32_32 distance = finalDistance +
            (rule.lockonInitialDistanceFixed - finalDistance) * fraction;
        const math::q32_32 angle = rule.lockonAngleSpinFixed * fraction;
        const math::q32_32_sincos rotation = math::fixed_sincos(angle);
        const LogicFixedVec3 cursorPosition{
            jetPosition.x + rotation.cosine * distance,
            jetPosition.y + rotation.sine * distance,
            jetPosition.z,
        };
        for (size_t templateChannel = 0;
             templateChannel < recipe.drawVisualChannels.size();
             ++templateChannel) {
            const game::ModelDrawVisualChannel& channel =
                recipe.drawVisualChannels[templateChannel];
            const size_t ruleIndex =
                game::selectModelConditionVisualRuleIndex(channel, {});
            if (ruleIndex >= channel.conditionVisuals.size()) continue;
            const game::ModelConditionVisualRule& visual =
                channel.conditionVisuals[ruleIndex];
            if (visual.model.empty() ||
                container::asciiEqualIgnoreCase(visual.model, "None"))
                continue;
            const uint64_t ordinal = moduleIndex *
                recipe.drawVisualChannels.size() + templateChannel;
            if (ordinal > static_cast<uint64_t>(
                    0x7fffffffu - kJetLockonPresentationChannelBase))
                continue;
            const uint32_t channelIndex =
                kJetLockonPresentationChannelBase +
                static_cast<uint32_t>(ordinal);
            render::RenderEntitySnapshot output;
            output.id = render_extraction_detail::renderInstanceId(
                identity->id.value, channelIndex);
            output.objectId = identity->id.value;
            output.channelIndex = channelIndex;
            output.modelAsset = visual.model;
            output.transform.position = {
                cursorPosition.x.to_float(),
                cursorPosition.y.to_float(),
                cursorPosition.z.to_float(),
            };
            output.transform.orientation = facingJet(angle);
            const float scale = recipe.assetScale.to_float();
            output.transform.scale = {scale, scale, scale};
            output.boundingRadius = 4.0f * std::max(1.0f, scale);
            output.visual.animationState = visual.animation;
            output.visual.animationMode =
                render_extraction_detail::toRenderAnimationMode(
                    visual.animationMode);
            output.visual.animationTimeSeconds =
                static_cast<float>(elapsed) /
                static_cast<float>(std::max<uint32_t>(
                    1u, logicFramesPerSecond));
            output.visual.animationStateEnterTick =
                runtime.lockonReadyTick > duration
                    ? runtime.lockonReadyTick - duration : 0;
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
            snapshot.entities.push_back(std::move(output));
        }
    }
}

} // namespace engine
