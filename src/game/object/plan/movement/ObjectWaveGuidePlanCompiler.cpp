#include "game/object/plan/movement/ObjectWaveGuidePlanTypes.h"
#include "core/container/string_utils.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "presentation/render/WaterSurfaceVisualSettings.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::String trim(container::StringView value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return container::String{value};
}

[[nodiscard]] const container::String* valueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto value = module.values.rbegin(); value != module.values.rend();
         ++value) {
        if (equalInsensitive(value->first, key)) return &value->second;
    }
    return nullptr;
}

[[nodiscard]] std::optional<double> parseFinite(
    container::StringView value) noexcept {
    const container::String cleaned = trim(value);
    if (cleaned.empty()) return std::nullopt;
    double output = 0.0;
    const auto [cursor, error] = std::from_chars(
        cleaned.data(), cleaned.data() + cleaned.size(), output);
    if (error != std::errc{} ||
        cursor != cleaned.data() + cleaned.size() ||
        !std::isfinite(output)) {
        return std::nullopt;
    }
    return output;
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    const container::String cleaned = trim(value);
    if (cleaned.empty()) return std::nullopt;
    uint32_t output = 0;
    const auto [cursor, error] = std::from_chars(
        cleaned.data(), cleaned.data() + cleaned.size(), output);
    if (error != std::errc{} || cursor != cleaned.data() + cleaned.size()) {
        return std::nullopt;
    }
    return output;
}

void computeShape(ObjectWaveGuideRule& rule,
                  ObjectWaveGuidePlan& plan,
                  container::StringView moduleTag) {
    constexpr size_t kMaximumShapePoints = 64;
    if (rule.ySize <= math::q32_32{} ||
        rule.linearWaveSpacing <= math::q32_32{}) {
        plan.diagnostics.push_back(
            container::String{moduleTag} +
            ": YSize and LinearWaveSpacing must be positive");
        return;
    }
    const math::q32_32 half = rule.ySize / math::q32_32{int32_t{2}};
    for (math::q32_32 y = -half;
         y < half && rule.localShapePoints.size() < kMaximumShapePoints;
         y += rule.linearWaveSpacing) {
        const math::q32_32 x = rule.waveBendMagnitude == math::q32_32{}
            ? math::q32_32{}
            : -(y * y) / rule.waveBendMagnitude;
        rule.localShapePoints.push_back({x, y, {}});
    }
    if (rule.localShapePoints.empty()) {
        plan.diagnostics.push_back(
            container::String{moduleTag} +
            ": WaveGuide shape produced no sample points");
    }
}

} // namespace

container::SharedPtr<const ObjectWaveGuidePlan>
compileObjectWaveGuidePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectWaveGuidePlan>();
    for (const ModuleData& module : templateData.modules) {
        const container::StringView moduleClass = module.moduleClass.empty()
            ? container::StringView{module.type}
            : container::StringView{module.moduleClass};
        if (!equalInsensitive(moduleClass, "WaveGuideUpdate")) continue;

        ObjectWaveGuideRule rule;
        rule.authoredOrder = module.authoredOrder;
        const container::String tag = !module.moduleTag.empty()
            ? module.moduleTag : module.tag;
        const auto fixed = [&](container::StringView key,
                               math::q32_32& output,
                               bool nonNegative = true) {
            const container::String* value = valueLast(module, key);
            if (!value) return;
            const std::optional<double> parsed = parseFinite(*value);
            if (!parsed || (nonNegative && *parsed < 0.0)) {
                plan->diagnostics.push_back(
                    tag + ": " + container::String{key} +
                    " must be a finite non-negative number");
                return;
            }
            output = math::q32_32{*parsed};
        };
        if (const container::String* value = valueLast(module, "WaveDelay")) {
            if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                rule.waveDelayMilliseconds = *parsed;
            } else {
                plan->diagnostics.push_back(
                    tag + ": WaveDelay must be unsigned milliseconds");
            }
        }
        fixed("YSize", rule.ySize);
        fixed("LinearWaveSpacing", rule.linearWaveSpacing);
        fixed("WaveBendMagnitude", rule.waveBendMagnitude);
        if (const container::String* value = valueLast(module, "WaterVelocity")) {
            const std::optional<double> parsed = parseFinite(*value);
            if (!parsed) {
                plan->diagnostics.push_back(
                    tag + ": WaterVelocity must be a finite number");
            } else {
                rule.waterVelocity = math::q32_32{
                    engine::water_surface::visual_defaults::
                        legacyWaterVelocityPerUpdate(
                            static_cast<float>(*parsed))};
            }
        }
        fixed("PreferredHeight", rule.preferredHeight, false);
        fixed("ShorelineEffectDistance", rule.shorelineEffectDistance);
        fixed("DamageRadius", rule.damageRadius);
        fixed("DamageAmount", rule.damageAmount);
        fixed("ToppleForce", rule.toppleForce);
        if (const container::String* value =
                valueLast(module, "RandomSplashSoundFrequency")) {
            int32_t frequency = 0;
            const container::String cleaned = trim(*value);
            const auto [cursor, error] = std::from_chars(
                cleaned.data(), cleaned.data() + cleaned.size(), frequency);
            if (error == std::errc{} &&
                cursor == cleaned.data() + cleaned.size()) {
                rule.randomSplashSoundFrequency =
                    std::clamp(frequency, 0, 100);
            } else {
                plan->diagnostics.push_back(
                    tag + ": RandomSplashSoundFrequency must be an integer");
            }
        }
        if (const container::String* value =
                valueLast(module, "BridgeParticleAngleFudge")) {
            if (const std::optional<double> parsed = parseFinite(*value)) {
                rule.bridgeParticleAngleFudgeRadians = math::q32_32{
                    *parsed * std::numbers::pi / 180.0};
            } else {
                plan->diagnostics.push_back(
                    tag + ": BridgeParticleAngleFudge must be finite degrees");
            }
        }
        const auto name = [&](container::StringView key,
                              container::String& output) {
            if (const container::String* value = valueLast(module, key)) {
                output = trim(*value);
            }
        };
        name("RandomSplashSound", rule.randomSplashSound);
        name("BridgeParticle", rule.bridgeParticle);
        name("LoopingSound", rule.loopingSound);
        computeShape(rule, *plan, tag);
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectWaveGuideRule& left,
           const ObjectWaveGuideRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}
} // namespace game
