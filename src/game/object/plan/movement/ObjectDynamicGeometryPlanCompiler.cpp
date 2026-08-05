#include "game/object/plan/movement/ObjectDynamicGeometryPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView trim(
    container::StringView value) noexcept {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] container::StringView numericText(
    container::StringView value) noexcept {
    const size_t semicolon = value.find(';');
    const size_t slash = value.find("//");
    size_t comment = container::StringView::npos;
    if (semicolon != container::StringView::npos) comment = semicolon;
    if (slash != container::StringView::npos) {
        comment = comment == container::StringView::npos
            ? slash : std::min(comment, slash);
    }
    return trim(comment == container::StringView::npos
        ? value : value.substr(0, comment));
}

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend();
         ++found) {
        if (equalInsensitive(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (equalInsensitive(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<uint32_t> parseUnsignedMilliseconds(
    container::StringView value) noexcept {
    value = numericText(value);
    if (value.empty() || value.front() == '-') return std::nullopt;
    uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value, math::q32_32 fallback = {}) noexcept {
    value = numericText(value);
    const game::ContentFloatContext context{
        .source = __FILE__, .block = "Object",
        .module = "DynamicGeometryInfoUpdate", .field = "FixedReal",
        .fallback = fallback.to_float()};
    const std::optional<float> parsed =
        game::parseContentFloat(value, context);
    if (!parsed) return fallback;
    constexpr float kMinimum = -2147483648.0f;
    constexpr float kMaximumExclusive = 2147483648.0f;
    if (*parsed < kMinimum || *parsed >= kMaximumExclusive) {
        game::warnContentFloatFallback(
            value, context,
            "finite numeric prefix is outside the Q32.32 field range; retained the prior/default value");
        return fallback;
    }
    return math::q32_32{*parsed};
}

[[nodiscard]] std::optional<bool> parseBoolean(
    container::StringView value) noexcept {
    value = numericText(value);
    if (equalInsensitive(value, "YES") ||
        equalInsensitive(value, "TRUE") || value == "1") return true;
    if (equalInsensitive(value, "NO") ||
        equalInsensitive(value, "FALSE") || value == "0") return false;
    return std::nullopt;
}

[[nodiscard]] container::String parseOptionalName(
    container::StringView value) {
    value = trim(value);
    if (equalInsensitive(value, "NONE")) return {};
    return container::String{value};
}

void appendDiagnostic(ObjectDynamicGeometryPlan& plan,
                      const ModuleData& module,
                      container::StringView field,
                      container::StringView value) {
    const container::StringView tag = !module.moduleTag.empty()
        ? container::StringView{module.moduleTag}
        : !module.tag.empty() ? container::StringView{module.tag}
                              : moduleClass(module);
    plan.diagnostics.push_back(
        container::String{moduleClass(module)} + " (tag '" +
        container::String{tag} + "') has invalid " +
        container::String{field} + " value '" +
        container::String{value} + "'");
}

} // namespace

container::SharedPtr<const ObjectDynamicGeometryPlan>
compileObjectDynamicGeometryPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectDynamicGeometryPlan>();
    for (const ModuleData& module : templateData.modules) {
        const container::StringView className = moduleClass(module);
        const bool firestorm = equalInsensitive(
            className, "FirestormDynamicGeometryInfoUpdate");
        if (!firestorm && !equalInsensitive(
                className, "DynamicGeometryInfoUpdate")) {
            continue;
        }

        ObjectDynamicGeometryRule rule{
            .authoredOrder = module.authoredOrder,
            .kind = firestorm ? ObjectDynamicGeometryKind::Firestorm
                              : ObjectDynamicGeometryKind::Basic,
        };
        bool valid = true;
        const auto readFixed = [&](container::StringView field,
                                   math::q32_32& destination) {
            const container::String* value = moduleValueLast(module, field);
            if (!value) return;
            const std::optional<math::q32_32> parsed =
                parseFixed(*value, destination);
            if (!parsed) {
                appendDiagnostic(*plan, module, field, *value);
                valid = false;
            } else {
                destination = *parsed;
            }
        };
        const auto readUnsigned = [&](container::StringView field,
                                      uint32_t& destination) {
            const container::String* value = moduleValueLast(module, field);
            if (!value) return false;
            const std::optional<uint32_t> parsed =
                parseUnsignedMilliseconds(*value);
            if (!parsed) {
                appendDiagnostic(*plan, module, field, *value);
                valid = false;
            } else {
                destination = *parsed;
            }
            return true;
        };

        static_cast<void>(readUnsigned(
            "InitialDelay", rule.initialDelayMilliseconds));
        readFixed("InitialHeight", rule.initialHeight);
        readFixed("InitialMajorRadius", rule.initialMajorRadius);
        readFixed("InitialMinorRadius", rule.initialMinorRadius);
        readFixed("FinalHeight", rule.finalHeight);
        readFixed("FinalMajorRadius", rule.finalMajorRadius);
        readFixed("FinalMinorRadius", rule.finalMinorRadius);
        rule.hasAuthoredTransitionTime = readUnsigned(
            "TransitionTime", rule.transitionMilliseconds);
        if (const container::String* value = moduleValueLast(
                module, "ReverseAtTransitionTime")) {
            const std::optional<bool> parsed = parseBoolean(*value);
            if (!parsed) {
                appendDiagnostic(*plan, module,
                                 "ReverseAtTransitionTime", *value);
                valid = false;
            } else {
                rule.reverseAtTransitionTime = *parsed;
            }
        }

        if (firestorm) {
            for (size_t index = 0;
                 index < rule.firestorm.particleSystems.size(); ++index) {
                const container::String field =
                    "ParticleSystem" + std::to_string(index + 1);
                if (const container::String* value =
                        moduleValueLast(module, field)) {
                    rule.firestorm.particleSystems[index] =
                        parseOptionalName(*value);
                }
            }
            if (const container::String* value =
                    moduleValueLast(module, "FXList")) {
                rule.firestorm.fxList = parseOptionalName(*value);
            }
            readFixed("ParticleOffsetZ", rule.firestorm.particleOffsetZ);
            readFixed("ScorchSize", rule.firestorm.scorchSize);
            readFixed("DelayBetweenDamageFrames",
                      rule.firestorm.damageIntervalMilliseconds);
            readFixed("DamageAmount", rule.firestorm.damageAmount);
            readFixed("MaxHeightForDamage",
                      rule.firestorm.maximumHeightForDamage);
            if (rule.firestorm.damageIntervalMilliseconds < math::q32_32{}) {
                const container::String* value = moduleValueLast(
                    module, "DelayBetweenDamageFrames");
                appendDiagnostic(*plan, module,
                                 "DelayBetweenDamageFrames",
                                 value ? container::StringView{*value}
                                       : container::StringView{"<negative>"});
                valid = false;
            }
        }

        if (valid) plan->rules.push_back(std::move(rule));
    }

    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectDynamicGeometryRule& left,
           const ObjectDynamicGeometryRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan->rules.empty() && plan->diagnostics.empty() ? nullptr : plan;
}

} // namespace game
