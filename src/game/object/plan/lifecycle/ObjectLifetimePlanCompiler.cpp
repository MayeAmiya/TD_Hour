#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/plan/lifecycle/ObjectLifetimePlanTypes.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <system_error>
#include <utility>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;
constexpr auto trim = container::trimAsciiView;

// FieldParse writes the last repeated scalar field into ModuleData. Preserve
// that behavior for production recipe values while still supporting compact
// hand-built probe data that only fills the unordered properties map.
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

[[nodiscard]] std::optional<uint32_t> parseMilliseconds(container::StringView text) noexcept {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    if (parsed > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] uint64_t stableRuleKey(const ModuleData& module) noexcept {
    // FNV-1a over canonical module identity. This is a PRF salt, not a save
    // format or exposed hash, so a dense stable local implementation is enough.
    uint64_t hash = 14695981039346656037ull;
    const auto append = [&hash](container::StringView value) {
        for (const char raw : value) {
            const char lower = raw >= 'A' && raw <= 'Z'
                ? static_cast<char>(raw + ('a' - 'A')) : raw;
            hash ^= static_cast<uint8_t>(lower);
            hash *= 1099511628211ull;
        }
        hash ^= 0xffu;
        hash *= 1099511628211ull;
    };
    append(moduleClass(module));
    append(module.moduleTag);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<uint8_t>(module.authoredOrder >> shift);
        hash *= 1099511628211ull;
    }
    return hash;
}

void appendInvalidDurationDiagnostic(ObjectLifetimePlan& plan, const ModuleData& module,
                                     container::StringView field, container::StringView value) {
    const container::StringView tag = module.moduleTag.empty()
        ? container::StringView{"<untagged>"}
        : container::StringView{module.moduleTag};
    plan.diagnostics.push_back("lifetime module '" + container::String(moduleClass(module)) +
        "' (tag '" + container::String(tag) + "') has invalid unsigned-millisecond " +
        container::String(field) + " value '" + container::String(value) + "'");
}

} // namespace

container::SharedPtr<const ObjectLifetimePlan>
compileObjectLifetimePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectLifetimePlan>();
    for (const ModuleData& module : templateData.modules) {
        const container::StringView className = moduleClass(module);
        ObjectLifetimeAction action{};
        if (asciiEqualIgnoreCase(className, "LifetimeUpdate")) {
            action = ObjectLifetimeAction::Kill;
        } else if (asciiEqualIgnoreCase(className, "DeletionUpdate")) {
            action = ObjectLifetimeAction::Destroy;
        } else {
            continue;
        }

        ObjectLifetimeRule rule{
            .authoredOrder = module.authoredOrder,
            .stableRuleKey = stableRuleKey(module),
            .action = action,
        };
        bool ruleValid = true;
        if (const container::String* value = moduleValueLast(module, "MinLifetime")) {
            const std::optional<uint32_t> parsed = parseMilliseconds(*value);
            if (parsed) {
                rule.minimumLifetimeMilliseconds = *parsed;
            } else {
                appendInvalidDurationDiagnostic(*plan, module, "MinLifetime", *value);
                ruleValid = false;
            }
        }
        if (const container::String* value = moduleValueLast(module, "MaxLifetime")) {
            const std::optional<uint32_t> parsed = parseMilliseconds(*value);
            if (parsed) {
                rule.maximumLifetimeMilliseconds = *parsed;
            } else {
                appendInvalidDurationDiagnostic(*plan, module, "MaxLifetime", *value);
                ruleValid = false;
            }
        }
        if (ruleValid) plan->rules.push_back(std::move(rule));
    }
    // Final Recipe application normally preserves source order, but module
    // replacement/copying must not make runtime timing depend on that
    // implementation detail. `authoredOrder` is the immutable canonical key;
    // stable_sort keeps a deterministic final-recipe tie order for malformed
    // content which reused the same order value.
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectLifetimeRule& left, const ObjectLifetimeRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan->rules.empty() && plan->diagnostics.empty() ? nullptr : plan;
}

} // namespace game
