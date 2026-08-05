#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "CombatProfile.h"
#include "ObjectModuleCatalog.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "VFS.h"
#include "debug/debug.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include "ThingRecipeDetail.h"

namespace game {
using namespace detail;

std::optional<ModelAnimationCandidate> parseModelAnimationDirective(
    container::StringView authored, bool idle) {
    return parseModelAnimationCandidate(authored, idle);
}

template <typename Rule>
ModelAnimationSelection selectModelAnimationImpl(
    const Rule& rule,
    uint64_t objectId,
    const ModelConditionMask& conditions,
    uint64_t stateGeneration) noexcept {
    const auto mix = [](uint64_t value) noexcept {
        value += 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31u);
    };
    uint64_t seed = mix(objectId);
    if (stateGeneration != 0) {
        seed = mix(seed ^ mix(stateGeneration));
    }
    for (uint64_t word : conditions.words) seed = mix(seed ^ word);
    for (unsigned char character : rule.model) {
        seed = mix(seed ^ static_cast<uint64_t>(character));
    }

    ModelAnimationSelection result;
    if (!rule.animationCandidates.empty()) {
        uint64_t totalWeight = 0;
        for (const ModelAnimationCandidate& candidate : rule.animationCandidates) {
            totalWeight = std::min<uint64_t>(
                std::numeric_limits<uint64_t>::max() - candidate.selectionWeight,
                totalWeight) + candidate.selectionWeight;
        }
        uint64_t ticket = totalWeight == 0 ? 0 : seed % totalWeight;
        result.candidateIndex = rule.animationCandidates.size() - 1u;
        for (size_t index = 0; index < rule.animationCandidates.size(); ++index) {
            const uint32_t weight = rule.animationCandidates[index].selectionWeight;
            if (ticket < weight) {
                result.candidateIndex = index;
                break;
            }
            ticket -= weight;
        }
    }

    const float minimum = rule.animationSpeedFactorMinimum;
    const float maximum = rule.animationSpeedFactorMaximum;
    if (std::isfinite(minimum) && std::isfinite(maximum)) {
        if (maximum <= minimum) {
            result.speedFactor = minimum;
        } else {
            const uint64_t randomBits = mix(seed ^ 0xA0761D6478BD642Full);
            const double unit = static_cast<double>(randomBits >> 11u) *
                (1.0 / 9007199254740992.0);
            result.speedFactor = minimum +
                (maximum - minimum) * static_cast<float>(unit);
        }
    }
    return result;
}

ModelAnimationSelection selectModelAnimation(
    const ModelConditionVisualRule& rule,
    uint64_t objectId,
    const ModelConditionMask& conditions,
    uint64_t stateGeneration) noexcept {
    return selectModelAnimationImpl(
        rule, objectId, conditions, stateGeneration);
}

ModelAnimationSelection selectModelAnimation(
    const ModelConditionTransitionRule& rule,
    uint64_t objectId,
    const ModelConditionMask& conditions,
    uint64_t stateGeneration) noexcept {
    return selectModelAnimationImpl(
        rule, objectId, conditions, stateGeneration);
}

namespace {
size_t selectModelConditionVisualRuleIndexImpl(
    container::Span<const ModelConditionVisualRule> visuals,
    ModelConditionMask ignoredConditions,
    ModelConditionMask conditions) noexcept {
    conditions.clear(ignoredConditions);
    size_t bestIndex = std::numeric_limits<size_t>::max();
    uint32_t bestYes = 0;
    uint32_t bestExtraneous = UINT32_MAX;
    for (size_t ruleIndex = 0; ruleIndex < visuals.size(); ++ruleIndex) {
        const ModelConditionVisualRule& rule = visuals[ruleIndex];
        for (const ModelConditionMask& required : rule.acceptedConditions) {
            const uint32_t yes = conditions.intersectionCount(required);
            const uint32_t extraneous = required.extraneousCountAgainst(conditions);
            if (yes > bestYes ||
                (yes >= bestYes && extraneous < bestExtraneous)) {
                bestIndex = ruleIndex;
                bestYes = yes;
                bestExtraneous = extraneous;
            }
        }
    }
    return bestIndex;
}
} // namespace

size_t selectModelConditionVisualRuleIndex(
    const ThingTemplate& templateData,
    ModelConditionMask conditions) noexcept {
    return selectModelConditionVisualRuleIndexImpl(
        templateData.modelConditionVisuals,
        templateData.ignoredModelConditions, conditions);
}

size_t selectModelConditionVisualRuleIndex(
    const ModelDrawVisualChannel& channel,
    ModelConditionMask conditions) noexcept {
    return selectModelConditionVisualRuleIndexImpl(
        channel.conditionVisuals, channel.ignoredConditions, conditions);
}


} // namespace game
