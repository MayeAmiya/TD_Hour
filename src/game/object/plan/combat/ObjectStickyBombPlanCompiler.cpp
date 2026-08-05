#include "game/object/plan/combat/ObjectStickyBombPlanTypes.h"
#include "core/container/string_utils.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty()
        ? container::StringView{module.moduleClass}
        : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it) {
        if (equalInsensitive(it->first, key)) return &it->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (equalInsensitive(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView text) noexcept {
    double parsed = 0.0;
    const auto [cursor, error] = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    constexpr double maximumExclusive =
        static_cast<double>(std::numeric_limits<int32_t>::max()) + 1.0;
    constexpr double minimumInclusive =
        static_cast<double>(std::numeric_limits<int32_t>::min());
    if (error != std::errc{} || cursor != text.data() + text.size() ||
        !std::isfinite(parsed) || parsed < minimumInclusive ||
        parsed >= maximumExclusive) {
        return std::nullopt;
    }
    return math::q32_32{parsed};
}

[[nodiscard]] container::String unitSound(
    const ThingTemplate& templateData, container::StringView semanticName) {
    for (const auto& [candidate, eventName] :
         templateData.unitSpecificSounds) {
        if (equalInsensitive(candidate, semanticName)) return eventName;
    }
    return {};
}

} // namespace

container::SharedPtr<const ObjectStickyBombPlan>
compileObjectStickyBombPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectStickyBombPlan>();
    plan->createdSound = unitSound(templateData, "StickyBombCreated");
    plan->pingSound = unitSound(templateData, "UnitBombPing");
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "StickyBombUpdate")) {
            continue;
        }
        ObjectStickyBombRule rule{.authoredOrder = module.authoredOrder};
        if (const container::String* value =
                moduleValueLast(module, "AttachToTargetBone")) {
            rule.attachToTargetBone = *value;
        }
        if (const container::String* value =
                moduleValueLast(module, "OffsetZ")) {
            if (const auto parsed = parseFixed(*value)) {
                rule.offsetZ = *parsed;
            } else {
                plan->diagnostics.push_back(
                    "OffsetZ must be a finite Q32.32 world distance");
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "GeometryBasedDamageWeapon")) {
            rule.geometryBasedDamageWeapon = *value;
        }
        if (const container::String* value =
                moduleValueLast(module, "GeometryBasedDamageFX")) {
            rule.geometryBasedDamageFx = *value;
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return {};
    std::stable_sort(
        plan->rules.begin(), plan->rules.end(),
        [](const ObjectStickyBombRule& left,
           const ObjectStickyBombRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
