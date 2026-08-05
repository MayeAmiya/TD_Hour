#include "game/object/ai/definition/ObjectAIBehaviorPlan.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace game {
namespace {

[[nodiscard]] const container::String* valueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend();
         ++found) {
        if (container::asciiEqualIgnoreCase(found->first, key))
            return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (container::asciiEqualIgnoreCase(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] bool parseBool(container::StringView value,
                             bool fallback) noexcept {
    value = container::trimAsciiView(value);
    if (container::asciiEqualIgnoreCase(value, "YES") ||
        container::asciiEqualIgnoreCase(value, "TRUE") || value == "1")
        return true;
    if (container::asciiEqualIgnoreCase(value, "NO") ||
        container::asciiEqualIgnoreCase(value, "FALSE") || value == "0")
        return false;
    return fallback;
}

[[nodiscard]] uint32_t parseDurationMilliseconds(
    container::StringView value, uint32_t fallback) noexcept {
    const std::optional<float> parsed = parseContentFloat(value, {
        .source = __FILE__, .block = "Object", .module = "AIUpdate",
        .field = "MoodAttackCheckRate",
        .fallback = static_cast<float>(fallback),
    });
    if (!parsed || *parsed < 0.0f) return fallback;
    if (*parsed >= static_cast<float>(std::numeric_limits<uint32_t>::max()))
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(std::ceil(*parsed));
}

[[nodiscard]] uint32_t autoAcquireFlag(
    container::StringView token) noexcept {
    if (container::asciiEqualIgnoreCase(token, "YES"))
        return ObjectAIAutoAcquireYes;
    if (container::asciiEqualIgnoreCase(token, "STEALTHED"))
        return ObjectAIAutoAcquireWhileStealthed;
    if (container::asciiEqualIgnoreCase(token, "NO"))
        return ObjectAIAutoAcquireNo;
    if (container::asciiEqualIgnoreCase(token, "NOTWHILEATTACKING"))
        return ObjectAIAutoAcquireNotWhileAttacking;
    if (container::asciiEqualIgnoreCase(token, "ATTACK_BUILDINGS"))
        return ObjectAIAutoAcquireAttackBuildings;
    return 0;
}

void applyAutoAcquireMask(
    container::StringView value, uint32_t& result) noexcept {
    bool replacementStarted = false;
    while (!value.empty()) {
        const size_t begin = value.find_first_not_of(" \t\r\n,");
        if (begin == container::StringView::npos) break;
        value.remove_prefix(begin);
        const size_t end = value.find_first_of(" \t\r\n,");
        container::StringView token = value.substr(0, end);
        char operation = 0;
        if (!token.empty() && (token.front() == '+' ||
                              token.front() == '-')) {
            operation = token.front();
            token.remove_prefix(1);
        }
        const uint32_t flag = autoAcquireFlag(token);
        if (flag != 0) {
            if (operation == '+') {
                result |= flag;
            } else if (operation == '-') {
                result &= ~flag;
            } else {
                if (!replacementStarted) {
                    result = 0;
                    replacementStarted = true;
                }
                result |= flag;
            }
        }
        if (end == container::StringView::npos) break;
        value.remove_prefix(end);
    }
}

} // namespace

container::SharedPtr<const ObjectAIBehaviorPlan>
compileObjectAIBehaviorPlan(const ThingTemplate& templateData) {
    const ModuleData* aiModule = nullptr;
    for (const ModuleData& module : templateData.modules) {
        if (!module.isAiModule) continue;
        // Object recipe inheritance has already resolved replacement and
        // authored occurrence. More than one AI module is invalid elsewhere;
        // retain the last value here so diagnostics do not create a second
        // runtime interpretation.
        aiModule = &module;
    }
    if (!aiModule) return nullptr;

    auto plan = std::make_shared<ObjectAIBehaviorPlan>();
    bool foundAutoAcquire = false;
    for (const auto& [key, value] : aiModule->values) {
        if (!container::asciiEqualIgnoreCase(
                key, "AutoAcquireEnemiesWhenIdle")) {
            continue;
        }
        foundAutoAcquire = true;
        applyAutoAcquireMask(
            value, plan->autoAcquireEnemiesWhenIdle);
    }
    if (!foundAutoAcquire) {
        for (const auto& [key, value] : aiModule->properties) {
            if (!container::asciiEqualIgnoreCase(
                    key, "AutoAcquireEnemiesWhenIdle")) {
                continue;
            }
            applyAutoAcquireMask(
                value, plan->autoAcquireEnemiesWhenIdle);
            break;
        }
    }
    if (const container::String* value = valueLast(
            *aiModule, "MoodAttackCheckRate")) {
        plan->moodAttackCheckMilliseconds = parseDurationMilliseconds(
            *value, plan->moodAttackCheckMilliseconds);
    }
    if (const container::String* value = valueLast(
            *aiModule, "ForbidPlayerCommands")) {
        plan->forbidPlayerCommands = parseBool(
            *value, plan->forbidPlayerCommands);
    }
    return plan;
}

} // namespace game
