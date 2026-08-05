#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/combat/ObjectBoneFxPlanTypes.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView trim(container::StringView value) noexcept {
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == container::StringView::npos) return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto iterator = module.values.rbegin();
         iterator != module.values.rend(); ++iterator) {
        if (asciiEqualIgnoreCase(iterator->first, key)) return &iterator->second;
    }
    return nullptr;
}

[[nodiscard]] container::Vector<container::StringView> splitTokens(
    container::StringView value) {
    container::Vector<container::StringView> tokens;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t\r\n:", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t\r\n:", cursor);
        tokens.push_back(value.substr(
            cursor, end == container::StringView::npos
                        ? value.size() - cursor : end - cursor));
        if (end == container::StringView::npos) break;
        cursor = end + 1;
    }
    return tokens;
}

[[nodiscard]] std::optional<float> parseFloat(
    container::StringView value) noexcept {
    return parseContentFloat(value, {
        .source = __FILE__, .block = "Object", .module = "BoneFXUpdate",
        .field = "Real"});
}

[[nodiscard]] std::optional<bool> parseBool(
    container::StringView value) noexcept {
    if (asciiEqualIgnoreCase(value, "YES") ||
        asciiEqualIgnoreCase(value, "TRUE") || value == "1") return true;
    if (asciiEqualIgnoreCase(value, "NO") ||
        asciiEqualIgnoreCase(value, "FALSE") || value == "0") return false;
    return std::nullopt;
}

[[nodiscard]] constexpr uint64_t allDamageTypes() noexcept {
    constexpr uint8_t count = static_cast<uint8_t>(DamageType::COUNT);
    if constexpr (count >= 64) return std::numeric_limits<uint64_t>::max();
    return (uint64_t{1} << count) - 1u;
}

[[nodiscard]] std::optional<uint64_t> parseDamageTypeMask(
    container::StringView value) {
    uint64_t mask = allDamageTypes();
    bool sawToken = false;
    for (container::StringView token : splitTokens(value)) {
        if (asciiEqualIgnoreCase(token, "ALL")) {
            mask = allDamageTypes();
            sawToken = true;
            continue;
        }
        if (asciiEqualIgnoreCase(token, "NONE")) {
            mask = 0;
            sawToken = true;
            continue;
        }
        if (token.empty() ||
            (token.front() != '+' && token.front() != '-')) {
            return std::nullopt;
        }
        const bool remove = token.front() == '-';
        token.remove_prefix(1);
        const std::optional<DamageType> parsed = tryParseDamageType(token);
        if (!parsed) return std::nullopt;
        const uint8_t index = static_cast<uint8_t>(*parsed);
        if (index >= 64) return std::nullopt;
        const uint64_t bit = uint64_t{1} << index;
        if (remove) mask &= ~bit;
        else mask |= bit;
        sawToken = true;
    }
    return sawToken ? std::optional<uint64_t>{mask} : std::nullopt;
}

[[nodiscard]] container::String diagnosticPrefix(const ModuleData& module) {
    if (!module.moduleTag.empty()) return module.moduleTag;
    if (!module.tag.empty()) return module.tag;
    return container::String{moduleClass(module)};
}

void appendDiagnostic(ObjectBoneFxPlan& plan, const ModuleData& module,
                      container::String message) {
    plan.diagnostics.push_back(
        diagnosticPrefix(module) + ": " + std::move(message));
}

[[nodiscard]] std::optional<ObjectBoneFxEntry> parseEntry(
    container::StringView value, ObjectBoneFxPayloadKind kind, uint8_t slot) {
    const container::Vector<container::StringView> tokens = splitTokens(value);
    if (tokens.size() != 8 || !asciiEqualIgnoreCase(tokens[0], "BONE") ||
        tokens[1].empty() || !asciiEqualIgnoreCase(tokens[2], "ONLYONCE")) {
        return std::nullopt;
    }
    const std::optional<bool> onlyOnce = parseBool(tokens[3]);
    const std::optional<float> minimum = parseFloat(tokens[4]);
    const std::optional<float> maximum = parseFloat(tokens[5]);
    if (!onlyOnce || !minimum || !maximum || *minimum > *maximum) {
        return std::nullopt;
    }
    const container::StringView expected =
        kind == ObjectBoneFxPayloadKind::FxList ? "FXLIST"
        : kind == ObjectBoneFxPayloadKind::ObjectCreationList ? "OCL"
                                                               : "PSYS";
    if (!asciiEqualIgnoreCase(tokens[6], expected) || tokens[7].empty()) {
        return std::nullopt;
    }

    ObjectBoneFxEntry entry;
    entry.kind = kind;
    entry.boneName = container::String{tokens[1]};
    entry.resource = container::String{tokens[7]};
    entry.minimumDelayMilliseconds = math::q32_32{*minimum};
    entry.maximumDelayMilliseconds = math::q32_32{*maximum};
    entry.slot = slot;
    entry.onlyOnce = *onlyOnce;
    if (asciiEqualIgnoreCase(entry.resource, "NONE")) {
        entry.resource.clear();
    }
    return entry;
}

} // namespace

container::SharedPtr<const ObjectBoneFxPlan>
compileObjectBoneFxPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectBoneFxPlan>();
    size_t damageModuleCount = 0;
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "BoneFXDamage")) {
            continue;
        }
        if (damageModuleCount++ == 0) {
            plan->damageModuleAuthoredOrder = module.authoredOrder;
        }
    }
    plan->hasDamageModule = damageModuleCount != 0;
    constexpr container::Array<container::StringView, kBoneFxStateCount>
        statePrefixes{"Pristine", "Damaged", "ReallyDamaged", "Rubble"};

    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "BoneFXUpdate")) continue;
        ObjectBoneFxRule rule;
        rule.authoredOrder = module.authoredOrder;
        rule.fxDamageTypes = allDamageTypes();
        rule.oclDamageTypes = allDamageTypes();
        rule.particleDamageTypes = allDamageTypes();
        const auto parseMask = [&](container::StringView field,
                                   uint64_t& output) {
            const container::String* value = moduleValueLast(module, field);
            if (!value) return;
            if (const std::optional<uint64_t> parsed =
                    parseDamageTypeMask(*value)) {
                output = *parsed;
            } else {
                appendDiagnostic(*plan, module,
                    container::String{field} +
                    " contains an invalid damage-type mutation list");
            }
        };
        parseMask("DamageFXTypes", rule.fxDamageTypes);
        parseMask("DamageOCLTypes", rule.oclDamageTypes);
        parseMask("DamageParticleTypes", rule.particleDamageTypes);

        for (size_t state = 0; state < statePrefixes.size(); ++state) {
            for (size_t slotIndex = 0; slotIndex < kBoneFxMaximumSlots;
                 ++slotIndex) {
                const container::String prefix{statePrefixes[state]};
                const container::String suffix = std::to_string(slotIndex + 1u);
                const auto append = [&](container::String field,
                                        ObjectBoneFxPayloadKind kind) {
                    const container::String* value =
                        moduleValueLast(module, field);
                    if (!value || trim(*value).empty()) return;
                    const std::optional<ObjectBoneFxEntry> entry = parseEntry(
                        *value, kind, static_cast<uint8_t>(slotIndex));
                    if (entry) {
                        rule.entries[state].push_back(*entry);
                    } else {
                        appendDiagnostic(*plan, module,
                            field + " has invalid BoneFXUpdate syntax");
                    }
                };
                append(prefix + "FXList" + suffix,
                       ObjectBoneFxPayloadKind::FxList);
                append(prefix + "OCL" + suffix,
                       ObjectBoneFxPayloadKind::ObjectCreationList);
                append(prefix + "ParticleSystem" + suffix,
                       ObjectBoneFxPayloadKind::ParticleSystem);
            }
        }
        plan->rules.push_back(std::move(rule));
    }

    if (!plan->rules.empty() && !plan->hasDamageModule) {
        plan->diagnostics.push_back(
            "BoneFXUpdate requires a BoneFXDamage module");
    }
    if (plan->rules.empty() && plan->hasDamageModule) {
        plan->diagnostics.push_back(
            "BoneFXDamage requires a BoneFXUpdate module");
    }
    if (damageModuleCount > 1) {
        plan->diagnostics.push_back(
            "multiple BoneFXDamage modules are ambiguous in the legacy name lookup");
    }
    if (plan->rules.empty() && plan->diagnostics.empty() &&
        !plan->hasDamageModule) return {};
    return plan;
}

} // namespace game
