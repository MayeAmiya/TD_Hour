#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/lifecycle/ObjectHeightDiePlanTypes.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;
constexpr auto trim = container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(const ModuleData& module,
                                                  container::StringView key) noexcept {
    const container::String* result = nullptr;
    for (const auto& [entryKey, value] : module.values) {
        if (asciiEqualIgnoreCase(entryKey, key)) result = &value;
    }
    if (result) return result;
    for (const auto& [entryKey, value] : module.properties) {
        if (asciiEqualIgnoreCase(entryKey, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<math::q32_32> parseFiniteFixed(container::StringView text) noexcept {
    const std::optional<float> value =
        parseContentFloat(text, {
            .source = __FILE__, .block = "Object", .module = "HeightDie",
            .field = "Real"});
    return value
        ? std::optional<math::q32_32>{
              engine::LogicFixedVec3::scalarFromFloat(*value)}
        : std::nullopt;
}

[[nodiscard]] std::optional<uint32_t> parseMilliseconds(container::StringView text) noexcept {
    text = trim(text);
    if (text.empty() || text.front() == '-') return std::nullopt;
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(value);
}

[[nodiscard]] std::optional<bool> parseBoolean(container::StringView value) noexcept {
    value = trim(value);
    if (asciiEqualIgnoreCase(value, "YES") || asciiEqualIgnoreCase(value, "TRUE") || value == "1") {
        return true;
    }
    if (asciiEqualIgnoreCase(value, "NO") || asciiEqualIgnoreCase(value, "FALSE") || value == "0") {
        return false;
    }
    return std::nullopt;
}

void appendDiagnostic(ObjectHeightDiePlan& plan, const ModuleData& module,
                      container::StringView field, container::StringView value) {
    const container::StringView tag = module.moduleTag.empty()
        ? container::StringView{"<untagged>"} : container::StringView{module.moduleTag};
    plan.diagnostics.push_back("HeightDieUpdate (tag '" + container::String(tag) +
        "') has invalid " + container::String(field) + " value '" + container::String(value) + "'");
}

} // namespace

container::SharedPtr<const ObjectHeightDiePlan>
compileObjectHeightDiePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectHeightDiePlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "HeightDieUpdate")) continue;

        ObjectHeightDieRule rule{.authoredOrder = module.authoredOrder};
        bool valid = true;
        const auto readFixed = [&](container::StringView field, math::q32_32& destination) {
            const container::String* value = moduleValueLast(module, field);
            if (!value) return;
            const std::optional<math::q32_32> parsed = parseFiniteFixed(*value);
            if (!parsed) {
                appendDiagnostic(*plan, module, field, *value);
                valid = false;
                return;
            }
            destination = *parsed;
        };
        const auto readBoolean = [&](container::StringView field, bool& destination) {
            const container::String* value = moduleValueLast(module, field);
            if (!value) return;
            const std::optional<bool> parsed = parseBoolean(*value);
            if (!parsed) {
                appendDiagnostic(*plan, module, field, *value);
                valid = false;
                return;
            }
            destination = *parsed;
        };

        readFixed("TargetHeight", rule.targetHeightAboveTerrain);
        readBoolean("TargetHeightIncludesStructures", rule.targetHeightIncludesStructures);
        readBoolean("OnlyWhenMovingDown", rule.onlyWhenMovingDown);
        readFixed("DestroyAttachedParticlesAtHeight", rule.destroyAttachedParticlesAtHeight);
        readBoolean("SnapToGroundOnDeath", rule.snapToGroundOnDeath);
        if (const container::String* value = moduleValueLast(module, "InitialDelay")) {
            const std::optional<uint32_t> parsed = parseMilliseconds(*value);
            if (!parsed) {
                appendDiagnostic(*plan, module, "InitialDelay", *value);
                valid = false;
            } else {
                rule.initialDelayMilliseconds = *parsed;
            }
        }
        if (valid) plan->rules.push_back(std::move(rule));
    }

    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectHeightDieRule& left, const ObjectHeightDieRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan->rules.empty() && plan->diagnostics.empty() ? nullptr : plan;
}

} // namespace game
