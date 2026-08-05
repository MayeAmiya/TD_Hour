#include "game/object/plan/structure/ObjectMissileLauncherBuildingPlanTypes.h"
#include "core/container/string_utils.h"
#include "debug/debug.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto iterator = module.values.rbegin();
         iterator != module.values.rend(); ++iterator) {
        if (equalInsensitive(iterator->first, key)) return &iterator->second;
    }
    return nullptr;
}

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

[[nodiscard]] std::optional<uint32_t> parseMilliseconds(
    container::StringView value) noexcept {
    const container::String cleaned = trim(value);
    if (cleaned.empty()) return std::nullopt;
    uint32_t parsed = 0;
    const char* begin = cleaned.data();
    const char* end = begin + cleaned.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || cursor != end) return std::nullopt;
    return parsed;
}

} // namespace

container::SharedPtr<const ObjectMissileLauncherBuildingPlan>
compileObjectMissileLauncherBuildingPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectMissileLauncherBuildingPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(
                module.moduleClass, "MissileLauncherBuildingUpdate")) {
            continue;
        }
        ObjectMissileLauncherBuildingRule rule;
        rule.authoredOrder = module.authoredOrder;
        if (const container::String* value =
                moduleValueLast(module, "SpecialPowerTemplate")) {
            rule.specialPowerTemplate = trim(*value);
        }
        const container::String tag = !module.moduleTag.empty()
            ? module.moduleTag : module.tag;
        if (rule.specialPowerTemplate.empty()) {
            plan->diagnostics.push_back(
                tag + ": SpecialPowerTemplate is required");
        }
        const auto duration = [&](container::StringView key,
                                  uint32_t& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            if (const std::optional<uint32_t> parsed =
                    parseMilliseconds(*value)) {
                destination = *parsed;
            } else {
                plan->diagnostics.push_back(
                    tag + ": " + container::String{key} +
                    " must be unsigned milliseconds");
            }
        };
        duration("DoorOpenTime", rule.doorOpenMilliseconds);
        duration("DoorWaitOpenTime", rule.doorWaitOpenMilliseconds);
        duration("DoorCloseTime", rule.doorCloseMilliseconds);
        const auto name = [&](container::StringView key,
                              container::String& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                destination = trim(*value);
            }
        };
        name("DoorOpeningFX", rule.doorOpeningFx);
        name("DoorOpenFX", rule.doorOpenFx);
        name("DoorWaitingToCloseFX", rule.doorWaitingToCloseFx);
        name("DoorClosingFX", rule.doorClosingFx);
        name("DoorClosedFX", rule.doorClosedFx);
        name("DoorOpenIdleAudio", rule.doorOpenIdleAudio);
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(
        plan->rules.begin(), plan->rules.end(),
        [](const ObjectMissileLauncherBuildingRule& left,
           const ObjectMissileLauncherBuildingRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
