#include "game/object/plan/structure/ObjectBridgePlanTypes.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>
#include <tuple>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;

using container::trimAsciiView;

[[nodiscard]] const container::String* moduleValue(
    const ModuleData& module, container::StringView key) noexcept {
    for (const auto& [current, value] : module.values) {
        if (asciiEqualIgnoreCase(current, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] float parseFiniteFloat(container::StringView value,
                                     float fallback = 0.0f) noexcept {
    return parseContentFloatOr(value, {
        .source = __FILE__, .block = "Object", .module = "Bridge",
        .field = "Real", .fallback = fallback});
}

[[nodiscard]] int32_t parseSignedInt(container::StringView value,
                                     int32_t fallback = 0) noexcept {
    const container::StringView trimmed = trimAsciiView(value);
    int32_t parsed = fallback;
    const auto* begin = trimmed.data();
    const auto* end = trimmed.data() + trimmed.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    return result.ec == std::errc{} ? parsed : fallback;
}

[[nodiscard]] uint32_t parseMilliseconds(container::StringView value,
                                         uint32_t fallback = 0) noexcept {
    const int32_t parsed = parseSignedInt(value, static_cast<int32_t>(fallback));
    return parsed <= 0 ? 0u : static_cast<uint32_t>(parsed);
}

[[nodiscard]] math::q32_32 fixedNonNegative(container::StringView value,
                                            float fallback) noexcept {
    return math::q32_32{std::max(0.0f, parseFiniteFloat(value, fallback))};
}

[[nodiscard]] bool parseBoolean(container::StringView value,
                                bool fallback = false) noexcept {
    const container::StringView text = trimAsciiView(value);
    if (asciiEqualIgnoreCase(text, "yes") ||
        asciiEqualIgnoreCase(text, "true") || text == "1") {
        return true;
    }
    if (asciiEqualIgnoreCase(text, "no") ||
        asciiEqualIgnoreCase(text, "false") || text == "0") {
        return false;
    }
    return fallback;
}

[[nodiscard]] container::String tokenAfterLabel(container::StringView value,
                                               container::StringView label) {
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               std::isspace(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        const size_t tokenBegin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        container::StringView token =
            value.substr(tokenBegin, cursor - tokenBegin);
        bool matched = asciiEqualIgnoreCase(token, label);
        size_t inlineValue = container::StringView::npos;
        if (!matched && token.size() > label.size() &&
            token[label.size()] == ':' &&
            asciiEqualIgnoreCase(token.substr(0, label.size()), label)) {
            matched = true;
            inlineValue = label.size() + 1u;
        }
        if (!matched) continue;
        if (inlineValue != container::StringView::npos &&
            inlineValue < token.size()) {
            return container::String{token.substr(inlineValue)};
        }
        while (cursor < value.size() &&
               std::isspace(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        if (cursor < value.size() && value[cursor] == ':') {
            ++cursor;
            while (cursor < value.size() &&
                   std::isspace(static_cast<unsigned char>(value[cursor]))) {
                ++cursor;
            }
        }
        const size_t resultBegin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        return container::String{
            value.substr(resultBegin, cursor - resultBegin)};
    }
    return {};
}

[[nodiscard]] ObjectBridgeTimedResource parseTimedResource(
    container::StringView value, bool ocl, uint32_t authoredOrder) {
    ObjectBridgeTimedResource result;
    result.authoredOrder = authoredOrder;
    result.resource = tokenAfterLabel(value, ocl ? "OCL" : "FX");
    const container::String delay = tokenAfterLabel(value, "Delay");
    if (!delay.empty()) result.delayMilliseconds = parseMilliseconds(delay);
    result.bone = tokenAfterLabel(value, "Bone");
    return result;
}

} // namespace

container::SharedPtr<const ObjectBridgeRailPlan>
compileObjectBridgeRailPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectBridgeRailPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (asciiEqualIgnoreCase(module.moduleClass, "BridgeBehavior")) {
            ObjectBridgeBehaviorRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const container::String* value =
                    moduleValue(module, "LateralScaffoldSpeed")) {
                rule.lateralScaffoldSpeed = fixedNonNegative(*value, 1.0f);
            }
            if (const container::String* value =
                    moduleValue(module, "VerticalScaffoldSpeed")) {
                rule.verticalScaffoldSpeed = fixedNonNegative(*value, 1.0f);
            }
            for (const auto& [key, value] : module.values) {
                if (asciiEqualIgnoreCase(key, "BridgeDieFX")) {
                    rule.dieFx.push_back(parseTimedResource(
                        value, false, module.authoredOrder));
                } else if (asciiEqualIgnoreCase(key, "BridgeDieOCL")) {
                    rule.dieOcl.push_back(parseTimedResource(
                        value, true, module.authoredOrder));
                }
            }
            plan->bridges.push_back(std::move(rule));
        } else if (asciiEqualIgnoreCase(module.moduleClass,
                                        "BridgeScaffoldBehavior")) {
            plan->scaffolds.push_back({.authoredOrder = module.authoredOrder});
        } else if (asciiEqualIgnoreCase(module.moduleClass,
                                        "BridgeTowerBehavior")) {
            plan->towers.push_back({.authoredOrder = module.authoredOrder});
        } else if (asciiEqualIgnoreCase(module.moduleClass,
                                        "RailroadBehavior")) {
            ObjectRailroadRule rule;
            rule.authoredOrder = module.authoredOrder;
            const auto copyValue = [&](container::StringView key,
                                       container::String& destination) {
                if (const container::String* value = moduleValue(module, key)) {
                    destination = container::String{trimAsciiView(*value)};
                }
            };
            copyValue("PathPrefixName", rule.pathPrefixName);
            copyValue("CrashFXTemplateName", rule.crashFxTemplateName);
            copyValue("BigMetalBounceSound", rule.bigMetalBounceSound);
            copyValue("SmallMetalBounceSound", rule.smallMetalBounceSound);
            copyValue("MeatyBounceSound", rule.meatyBounceSound);
            copyValue("RunningSound", rule.runningSound);
            copyValue("ClicketyClackSound", rule.clicketyClackSound);
            copyValue("WhistleSound", rule.whistleSound);
            for (const auto& [key, value] : module.values) {
                if (asciiEqualIgnoreCase(key, "CarriageTemplateName")) {
                    rule.carriageTemplateNames.emplace_back(trimAsciiView(value));
                }
            }
            if (const container::String* value =
                    moduleValue(module, "RunningGarrisonSpeedMax")) {
                rule.runningGarrisonSpeedMax = fixedNonNegative(*value, 1.0f);
            }
            if (const container::String* value =
                    moduleValue(module, "KillSpeedMin")) {
                rule.killSpeedMin = fixedNonNegative(*value, 1.0f);
            }
            if (const container::String* value = moduleValue(module, "SpeedMax")) {
                rule.speedMax = fixedNonNegative(*value, 4.0f);
            }
            if (const container::String* value =
                    moduleValue(module, "Acceleration")) {
                rule.acceleration = fixedNonNegative(*value, 1.01f);
            }
            if (const container::String* value = moduleValue(module, "Braking")) {
                rule.braking = fixedNonNegative(*value, 0.99f);
            }
            if (const container::String* value = moduleValue(module, "Friction")) {
                rule.friction = fixedNonNegative(*value, 0.97f);
            }
            if (const container::String* value =
                    moduleValue(module, "WaitAtStationTime")) {
                rule.waitAtStationMilliseconds =
                    parseMilliseconds(*value, 5000u);
            }
            if (const container::String* value =
                    moduleValue(module, "IsLocomotive")) {
                rule.isLocomotive = parseBoolean(*value);
            }
            plan->railroads.push_back(std::move(rule));
        } else if (asciiEqualIgnoreCase(module.moduleClass,
                                        "RailedTransportContain")) {
            plan->railedTransportContains.push_back(
                {.authoredOrder = module.authoredOrder});
        } else if (asciiEqualIgnoreCase(module.moduleClass,
                                        "RailedTransportDockUpdate")) {
            ObjectRailedTransportDockRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const container::String* value =
                    moduleValue(module, "PullInsideDuration")) {
                rule.pullInsideDurationMilliseconds = parseMilliseconds(*value);
            }
            if (const container::String* value =
                    moduleValue(module, "PushOutsideDuration")) {
                rule.pushOutsideDurationMilliseconds = parseMilliseconds(*value);
            }
            if (const container::String* value =
                    moduleValue(module, "ToleranceDistance")) {
                rule.toleranceDistance = fixedNonNegative(*value, 50.0f);
            }
            plan->railedTransportDocks.push_back(std::move(rule));
        } else if (asciiEqualIgnoreCase(module.moduleClass,
                                        "RailedTransportAIUpdate")) {
            ObjectRailedTransportAiRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const container::String* value =
                    moduleValue(module, "PathPrefixName")) {
                rule.pathPrefixName = container::String{trimAsciiView(*value)};
            }
            plan->railedTransportAi.push_back(std::move(rule));
        }
    }
    if (plan->bridges.empty() && plan->scaffolds.empty() &&
        plan->towers.empty() && plan->railroads.empty() &&
        plan->railedTransportContains.empty() &&
        plan->railedTransportDocks.empty() &&
        plan->railedTransportAi.empty()) {
        return {};
    }
    return plan;
}

} // namespace game
