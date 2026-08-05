#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/economy/ObjectProductionPlanTypes.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/contracts/ObjectDisabledTypes.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/plan/economy/ObjectUpgradePlanTypes.h"
#include "game/player/FactionTemplate.h"
#include "game/player/PlayerRegistry.h"
#include "game/command/CommandBarOverrides.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;

using container::trimAsciiView;

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

[[nodiscard]] std::optional<uint32_t> parseUnsigned(container::StringView text) noexcept {
    text = trimAsciiView(text);
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
    value = trimAsciiView(value);
    if (asciiEqualIgnoreCase(value, "YES") || asciiEqualIgnoreCase(value, "TRUE") || value == "1") {
        return true;
    }
    if (asciiEqualIgnoreCase(value, "NO") || asciiEqualIgnoreCase(value, "FALSE") || value == "0") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] container::Vector<container::StringView> splitWhitespace(container::StringView text) {
    container::Vector<container::StringView> result;
    while (true) {
        text = trimAsciiView(text);
        if (text.empty()) break;
        size_t end = 0;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        result.push_back(text.substr(0, end));
        text.remove_prefix(end);
    }
    return result;
}

[[nodiscard]] std::optional<container::Array<float, 3>> parseCoord3(container::StringView text) noexcept {
    container::Array<float, 3> result{};
    constexpr container::Array<char, 3> labels{'X', 'Y', 'Z'};
    text = trimAsciiView(text);
    for (size_t index = 0; index < result.size(); ++index) {
        while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) ||
                                 text.front() == ',')) {
            text.remove_prefix(1);
        }
        if (text.empty()) return std::nullopt;

        // ZH coordinates are normally authored as `X: ... Y: ... Z: ...`.
        // Retain the bare triplet form for programmatic/tool-authored data.
        if (std::isalpha(static_cast<unsigned char>(text.front()))) {
            if (std::toupper(static_cast<unsigned char>(text.front())) != labels[index]) {
                return std::nullopt;
            }
            text.remove_prefix(1);
            text = trimAsciiView(text);
            if (text.empty() || text.front() != ':') return std::nullopt;
            text.remove_prefix(1);
            text = trimAsciiView(text);
            if (text.empty()) return std::nullopt;
        }

        size_t tokenEnd = 0;
        while (tokenEnd < text.size() &&
               !std::isspace(static_cast<unsigned char>(text[tokenEnd])) &&
               text[tokenEnd] != ',') {
            ++tokenEnd;
        }
        const container::StringView token = text.substr(0, tokenEnd);
        const std::optional<float> value = game::parseContentFloat(token, {
            .source = __FILE__,
            .block = "Object",
            .module = "ProductionExit",
            .field = "Coord3",
            .fallback = 0.0f,
        });
        if (!value) return std::nullopt;
        result[index] = *value;
        text.remove_prefix(tokenEnd);
    }
    text = trimAsciiView(text);
    if (!text.empty() && text.front() == ',') text.remove_prefix(1);
    return trimAsciiView(text).empty() ? std::optional<container::Array<float, 3>>{result} : std::nullopt;
}

[[nodiscard]] std::optional<ObjectProductionQuantityModifier>
parseQuantityModifier(container::StringView text) {
    const container::Vector<container::StringView> tokens = splitWhitespace(text);
    if (tokens.empty() || tokens.size() > 2) return std::nullopt;
    ObjectProductionQuantityModifier result{.templateName = container::String(tokens.front())};
    if (result.templateName.empty()) return std::nullopt;
    if (tokens.size() == 2) {
        const std::optional<uint32_t> count = parseUnsigned(tokens[1]);
        if (!count || *count == 0) return std::nullopt;
        result.quantity = *count;
    }
    return result;
}

[[nodiscard]] container::String moduleTag(const ModuleData& module) {
    return module.moduleTag.empty() ? container::String{"<untagged>"} : module.moduleTag;
}

void appendDiagnostic(container::Vector<container::String>& diagnostics, const ModuleData& module,
                      container::StringView message) {
    diagnostics.push_back("module '" + moduleTag(module) + "': " + container::String(message));
}

} // namespace

container::SharedPtr<const ObjectProductionPlan>
compileObjectProductionPlan(const ThingTemplate& templateData) {
    container::SharedPtr<ObjectProductionPlan> plan;
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "ProductionUpdate")) continue;
        // RefCode resolves ProductionUpdateInterface by authored order and
        // returns the first providing module. Later hosts are unreachable but
        // valid data, so they must not invalidate the whole object recipe.
        if (plan) continue;

        plan = std::make_shared<ObjectProductionPlan>();
        plan->authoredOrder = module.authoredOrder;
        const auto readUnsigned = [&](container::StringView key, uint32_t& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            const std::optional<uint32_t> parsed = parseUnsigned(*value);
            if (!parsed) {
                appendDiagnostic(plan->diagnostics, module,
                                 container::String(key) + " must be an unsigned integer (got '" + *value + "')");
            } else {
                destination = *parsed;
            }
        };
        readUnsigned("MaxQueueEntries", plan->maxQueueEntries);
        readUnsigned("NumDoorAnimations", plan->numberOfDoorAnimations);
        readUnsigned("DoorOpeningTime", plan->doorOpeningMilliseconds);
        readUnsigned("DoorWaitOpenTime", plan->doorWaitOpenMilliseconds);
        readUnsigned("DoorCloseTime", plan->doorClosingMilliseconds);
        readUnsigned("ConstructionCompleteDuration", plan->constructionCompleteMilliseconds);
        if (const container::String* value =
                moduleValueLast(module, "DisabledTypesToProcess")) {
            plan->disabledTypesToProcess = 0;
            for (const container::StringView token : splitWhitespace(*value)) {
                if (asciiEqualIgnoreCase(token, "DISABLEDMASK_ALL") ||
                    asciiEqualIgnoreCase(token, "DISABLED_ALL") ||
                    asciiEqualIgnoreCase(token, "DISABLED_ANY")) {
                    plan->disabledTypesToProcess =
                        engine::objectDisabledKnownMask();
                    continue;
                }
                if (asciiEqualIgnoreCase(token, "DISABLEDMASK_NONE") ||
                    asciiEqualIgnoreCase(token, "DISABLED_NONE")) {
                    continue;
                }
                const std::optional<engine::ObjectDisabledReason> reason =
                    engine::objectDisabledReasonFromLegacyToken(token);
                if (!reason) {
                    appendDiagnostic(
                        plan->diagnostics, module,
                        "unknown DisabledTypesToProcess token '" +
                            container::String(token) + "'");
                    continue;
                }
                plan->disabledTypesToProcess |=
                    engine::objectDisabledBit(*reason);
            }
        }
        if (plan->maxQueueEntries == 0) {
            appendDiagnostic(plan->diagnostics, module, "MaxQueueEntries must be greater than zero");
        }
        if (plan->numberOfDoorAnimations > 4) {
            appendDiagnostic(
                plan->diagnostics, module,
                "NumDoorAnimations exceeds RefCode DOOR_COUNT_MAX; runtime uses the first four doors");
        }

        for (const auto& [key, value] : module.values) {
            if (!asciiEqualIgnoreCase(key, "QuantityModifier")) continue;
            const std::optional<ObjectProductionQuantityModifier> parsed =
                parseQuantityModifier(value);
            if (!parsed) {
                appendDiagnostic(plan->diagnostics, module,
                                 "invalid QuantityModifier value '" + value + "'");
            } else {
                plan->quantityModifiers.push_back(*parsed);
            }
        }
    }
    return plan;
}

container::SharedPtr<const ObjectProductionExitPlan>
compileObjectProductionExitPlan(const ThingTemplate& templateData) {
    container::SharedPtr<ObjectProductionExitPlan> plan;
    for (const ModuleData& module : templateData.modules) {
        const container::StringView klass = moduleClass(module);
        std::optional<ObjectProductionExitKind> kind;
        if (asciiEqualIgnoreCase(klass, "DefaultProductionExitUpdate")) {
            kind = ObjectProductionExitKind::Default;
        } else if (asciiEqualIgnoreCase(klass, "QueueProductionExitUpdate")) {
            kind = ObjectProductionExitKind::Queue;
        } else if (asciiEqualIgnoreCase(klass, "SpawnPointProductionExitUpdate")) {
            kind = ObjectProductionExitKind::SpawnPoint;
        } else if (asciiEqualIgnoreCase(klass, "SupplyCenterProductionExitUpdate")) {
            kind = ObjectProductionExitKind::SupplyCenter;
        } else if (asciiEqualIgnoreCase(klass, "ParkingPlaceBehavior")) {
            kind = ObjectProductionExitKind::AirfieldParking;
        } else if (asciiEqualIgnoreCase(klass, "FlightDeckBehavior")) {
            kind = ObjectProductionExitKind::FlightDeck;
        } else {
            continue;
        }
        if (plan) {
            appendDiagnostic(plan->diagnostics, module,
                             "additional production ExitInterface is unreachable; legacy Object lookup uses the first host");
            continue;
        }

        plan = std::make_shared<ObjectProductionExitPlan>();
        plan->authoredOrder = module.authoredOrder;
        plan->kind = *kind;
        const auto readCoord = [&](container::StringView key, math::q32_32& x,
                                   math::q32_32& y, math::q32_32& z) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            const std::optional<container::Array<float, 3>> parsed = parseCoord3(*value);
            if (!parsed) {
                appendDiagnostic(plan->diagnostics, module,
                                 container::String(key) + " must be three finite coordinates (got '" + *value + "')");
                return;
            }
            x = engine::LogicFixedVec3::scalarFromFloat((*parsed)[0]);
            y = engine::LogicFixedVec3::scalarFromFloat((*parsed)[1]);
            z = engine::LogicFixedVec3::scalarFromFloat((*parsed)[2]);
        };
        readCoord("UnitCreatePoint", plan->unitCreatePointX, plan->unitCreatePointY,
                  plan->unitCreatePointZ);
        readCoord("NaturalRallyPoint", plan->naturalRallyPointX, plan->naturalRallyPointY,
                  plan->naturalRallyPointZ);
        const auto readUnsigned = [&](container::StringView key,
                                      uint32_t& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            const std::optional<uint32_t> parsed = parseUnsigned(*value);
            if (!parsed) {
                appendDiagnostic(plan->diagnostics, module,
                    container::String(key) +
                    " must be an unsigned duration/count (got '" + *value + "')");
            } else {
                destination = *parsed;
            }
        };
        readUnsigned("ExitDelay", plan->exitDelayMilliseconds);
        readUnsigned("InitialBurst", plan->initialBurst);
        readUnsigned("GrantTemporaryStealth",
                     plan->grantTemporaryStealthMilliseconds);
        if (const container::String* value =
                moduleValueLast(module, "AllowAirborneCreation")) {
            const std::optional<bool> parsed = parseBoolean(*value);
            if (!parsed) {
                appendDiagnostic(plan->diagnostics, module,
                    "AllowAirborneCreation must be boolean (got '" + *value + "')");
            } else {
                plan->allowAirborneCreation = *parsed;
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "UseSpawnRallyPoint")) {
            const std::optional<bool> parsed = parseBoolean(*value);
            if (!parsed) {
                appendDiagnostic(plan->diagnostics, module,
                    "UseSpawnRallyPoint must be boolean (got '" + *value + "')");
            } else {
                plan->useSpawnRallyPoint = *parsed;
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "SpawnPointBoneName")) {
            plan->spawnPointBoneName = container::String(trimAsciiView(*value));
        }
        if (*kind == ObjectProductionExitKind::SpawnPoint &&
            plan->spawnPointBoneName.empty()) {
            appendDiagnostic(plan->diagnostics, module,
                             "SpawnPointBoneName must not be empty");
        }
    }
    return plan;
}

} // namespace game
