#include "core/container/string_utils.h"
#include "game/object/plan/special/ObjectOclUpdatePlanTypes.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView trim(container::StringView value) noexcept {
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

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* valueLast(const ModuleData& module,
                                           container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found) {
        if (equalInsensitive(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (equalInsensitive(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] bool parseBool(container::StringView value) noexcept {
    value = trim(value);
    return equalInsensitive(value, "yes") || equalInsensitive(value, "true") ||
           equalInsensitive(value, "on") || value == "1";
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    value = trim(value);
    uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] container::Vector<container::StringView> tokens(container::StringView value) {
    container::Vector<container::StringView> result;
    while (!value.empty()) {
        while (!value.empty() &&
               (std::isspace(static_cast<unsigned char>(value.front())) ||
                value.front() == ':' || value.front() == ',')) {
            value.remove_prefix(1);
        }
        if (value.empty()) break;
        size_t length = 0;
        while (length < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[length])) &&
               value[length] != ':' && value[length] != ',') ++length;
        result.push_back(value.substr(0, length));
        value.remove_prefix(length);
    }
    return result;
}

[[nodiscard]] std::optional<ObjectFactionOclReference> parseFactionOcl(
    container::StringView value) {
    const container::Vector<container::StringView> parsed = tokens(value);
    if (parsed.size() < 4 || !equalInsensitive(parsed[0], "Faction") ||
        !equalInsensitive(parsed[2], "OCL")) return std::nullopt;
    return ObjectFactionOclReference{
        .faction = container::String{parsed[1]},
        .objectCreationList = container::String{parsed[3]},
    };
}

void appendDiagnostic(ObjectOclUpdatePlan& plan, const ModuleData& module,
                      container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
        : !module.tag.empty() ? module.tag : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

} // namespace

container::SharedPtr<const ObjectOclUpdatePlan>
compileObjectOclUpdatePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectOclUpdatePlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "OCLUpdate")) continue;
        ObjectOclUpdateParameters parameters;
        parameters.authoredOrder = module.authoredOrder;
        for (const auto& [key, value] : module.values) {
            if (equalInsensitive(key, "OCL")) {
                const container::StringView name = trim(value);
                if (!equalInsensitive(name, "None")) {
                    parameters.objectCreationList = container::String{name};
                }
            } else if (equalInsensitive(key, "FactionOCL")) {
                if (std::optional<ObjectFactionOclReference> reference =
                        parseFactionOcl(value)) {
                    parameters.factionObjectCreationLists.push_back(
                        std::move(*reference));
                } else {
                    appendDiagnostic(*plan, module,
                                     "FactionOCL must use 'Faction: name OCL: name'");
                }
            } else if (equalInsensitive(key, "MinDelay")) {
                if (const auto parsed = parseUnsigned(value)) {
                    parameters.minimumDelayMilliseconds = *parsed;
                } else appendDiagnostic(*plan, module, "MinDelay must be unsigned milliseconds");
            } else if (equalInsensitive(key, "MaxDelay")) {
                if (const auto parsed = parseUnsigned(value)) {
                    parameters.maximumDelayMilliseconds = *parsed;
                } else appendDiagnostic(*plan, module, "MaxDelay must be unsigned milliseconds");
            } else if (equalInsensitive(key, "CreateAtEdge")) {
                parameters.createAtEdge = parseBool(value);
            } else if (equalInsensitive(key, "FactionTriggered")) {
                parameters.factionTriggered = parseBool(value);
            }
        }
        if (parameters.maximumDelayMilliseconds <
            parameters.minimumDelayMilliseconds) {
            parameters.maximumDelayMilliseconds =
                parameters.minimumDelayMilliseconds;
        }
        if (parameters.factionTriggered) {
            if (parameters.factionObjectCreationLists.empty()) {
                appendDiagnostic(*plan, module,
                                 "FactionTriggered requires at least one FactionOCL");
            }
        } else if (parameters.objectCreationList.empty()) {
            appendDiagnostic(*plan, module, "OCL is required");
        }
        plan->rules.push_back(std::move(parameters));
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectOclUpdateParameters& left,
           const ObjectOclUpdateParameters& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
